#include "dynrm.h"
#include "utils/simplestring.h"
#include "utils/network_utils.h"
#include "utils/kvproperties.h"
#include "utils/memutilities.h"
#include "communication/rmcomm_MessageHandler.h"
#include "communication/rmcomm_QD_RM_Protocol.h"
#include "catalog/pg_resqueue.h"

/*
 * The DDL statement attribute name strings.
 */
char RSQDDLAttrNames[RSQ_DDL_ATTR_COUNT]
					[RESOURCE_QUEUE_DDL_ATTR_LENGTH_MAX] = {
	"parent",
	"active_statements",
	"memory_limit_cluster",
	"core_limit_cluster",
	"vsegment_resource_quota",
	"allocation_policy",
	"resource_upper_factor",
	"vsegment_upper_limit"
};

/*
 * The attribute names for expressing one complete resource queue definition.
 */
static char RSQTBLAttrNames[RSQ_TBL_ATTR_COUNT]
						   [RESOURCE_QUEUE_DDL_ATTR_LENGTH_MAX] = {
	"parent",
	"active_statements",
	"memory_limit_cluster",
	"core_limit_cluster",
	"vsegment_resource_quota",
	"allocation_policy",
	"resource_upper_factor",
	"vsegment_upper_limit",

	"oid",
	"name",
	"creation_time",
	"update_time",
	"status"
};

/*
 * The possible resource allocation policy names.
 */
static char RSQDDLValueAllocationPolicy[RSQ_ALLOCATION_POLICY_COUNT]
									   [RESOURCE_QUEUE_DDL_POLICY_LENGTH_MAX] = {
	"even",
	"fifo"
};

/*
 * The attributes for expressing one complete role/user definition.
 */
static char USRTBLAttrNames[USR_TBL_ATTR_COUNT]
						   [RESOURCE_ROLE_DDL_ATTR_LENGTH_MAX] = {
	"oid",
	"name",
	"target",
	"priority",
	"is_superuser"
};

/* Internal functions. */
int addQueryResourceRequestToQueue(DynResourceQueueTrack queuetrack,
								   ConnectionTrack		 conntrack);

/*------------------------------------------
 * The resource quota calculation functions.
 *------------------------------------------*/
typedef int  (* computeQueryQuotaByPolicy )(DynResourceQueueTrack,
											int32_t *,
											int32_t *,
											int32_t);

int computeQueryQuota_EVEN( DynResourceQueueTrack	track,
							int32_t			   	   *segnum,
							int32_t			   	   *segnummin,
							int32_t					segnumlimit);

int computeQueryQuota_FIFO( DynResourceQueueTrack	track,
							int32_t			   	   *segnum,
							int32_t			   	   *segnummin,
							int32_t					segnumlimit);

int32_t min(int32_t a, int32_t b);
int32_t max(int32_t a, int32_t b);
computeQueryQuotaByPolicy AllocationPolicy[RSQ_ALLOCATION_POLICY_COUNT] = {
	computeQueryQuota_EVEN,
	computeQueryQuota_FIFO
};

int computeQueryQuota( DynResourceQueueTrack  track,
					   int32_t			 	 *max_segcountfix,
					   int32_t			 	 *min_segcountfix,
		   	   	   	   int32_t		       	 *segmemmb,
					   double		       	 *segcore,
					   int32_t		       	 *segnum,
					   int32_t				 *segnummin,
					   int32_t				  segnumlimit);

/*------------------------------------------
 * The resource distribution functions.
 *------------------------------------------*/
typedef int (* dispatchResourceToQueriesByPolicy )(DynResourceQueueTrack);

int dispatchResourceToQueries_EVEN(DynResourceQueueTrack track);
int dispatchResourceToQueries_FIFO(DynResourceQueueTrack track);

dispatchResourceToQueriesByPolicy DispatchPolicy[RSQ_ALLOCATION_POLICY_COUNT] = {
	dispatchResourceToQueries_EVEN,
	dispatchResourceToQueries_FIFO
};

void dispatchResourceToQueriesInOneQueue(DynResourceQueueTrack track);

/* Functions for operating resource queue tracker instance. */
DynResourceQueueTrack createDynResourceQueueTrack(DynResourceQueue queue);

void returnAllocatedResourceToLeafQueue(DynResourceQueueTrack 	track,
								    	int32_t 				memorymb,
								    	double					core);

void refreshResourceQueuePercentageCapacityInternal(uint32_t clustermemmb,
													uint32_t clustercore);

/* Internal APIs for maintaining memory/core ratio trackers. */
int32_t addResourceQueueRatio(DynResourceQueueTrack track);
void removeResourceQueueRatio(DynResourceQueueTrack track);

DynMemoryCoreRatioTrack createDynMemoryCoreRatioTrack(uint32_t ratio,
		  	  	  	  	  	  	  	  	  	  	      int32_t  index );
void freeMemoryCoreTrack(DynMemoryCoreRatioTrack mctrack);
int removeQueueTrackFromMemoryCoreTrack(DynMemoryCoreRatioTrack mctrack,
										DynResourceQueueTrack   track);

int getRSQTBLAttributeNameIndex(SimpStringPtr attrname);
int getRSQDDLAttributeNameIndex(SimpStringPtr attrname);
int getUSRTBLAttributeNameIndex(SimpStringPtr attrname);

void detectAndDealWithDeadLock(DynResourceQueueTrack track);

void markMemoryCoreRatioWaterMark(DQueue 		marks,
								  uint64_t 		curmicrosec,
								  int32_t 		memmb,
								  double 		core);

void buildTimeoutResponseForQueuedRequest(ConnectionTrack conntrack,
										  uint32_t 		  reason);

/*----------------------------------------------------------------------------*/
/*                    RESOURCE QUEUE MANAGER EXTERNAL APIs                    */
/*----------------------------------------------------------------------------*/

int32_t min(int32_t a, int32_t b)
{
	return a>b ? b : a;
}

int32_t max(int32_t a, int32_t b)
{
	return a<b ? b : a;
}

/* Initialize the resource queue manager instance. */
void initializeResourceQueueManager(void)
{
    ASSERT_DRM_GLOBAL_INSTANCE_CREATED

	PQUEMGR->RootTrack    	= NULL;
    PQUEMGR->DefaultTrack 	= NULL;
    PQUEMGR->Queues 		= NULL;

	/* 
	 * The two hash tables hold only the mapping from queue id or queue name to
	 * the queue object saved in Queues.
  	 */
    initializeHASHTABLE(&(PQUEMGR->QueuesIDIndex),
    					PCONTEXT,
						HASHTABLE_SLOT_VOLUME_DEFAULT,
						HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
						HASHTABLE_KEYTYPE_CHARARRAY,
						NULL);

    initializeHASHTABLE(&(PQUEMGR->QueuesNameIndex),
    					PCONTEXT,
						HASHTABLE_SLOT_VOLUME_DEFAULT,
						HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
						HASHTABLE_KEYTYPE_SIMPSTR,
						NULL);

    /* Initialize user information part. */
    PQUEMGR->Users = NULL;

    initializeHASHTABLE(&(PQUEMGR->UsersIDIndex),
    					PCONTEXT,
						HASHTABLE_SLOT_VOLUME_DEFAULT,
						HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
						HASHTABLE_KEYTYPE_CHARARRAY,
						NULL);

    initializeHASHTABLE(&(PQUEMGR->UsersNameIndex),
    					PCONTEXT,
						HASHTABLE_SLOT_VOLUME_DEFAULT,
						HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
						HASHTABLE_KEYTYPE_SIMPSTR,
						NULL);

    /* Initialize memory/core ratio counter and index. */
    PQUEMGR->RatioCount = 0;
    initializeHASHTABLE(&(PQUEMGR->RatioIndex),
    					PCONTEXT,
    			  	  	HASHTABLE_SLOT_VOLUME_DEFAULT,
    			  	  	HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
    			  	  	HASHTABLE_KEYTYPE_UINT32,
    			  	  	NULL);
    for ( int i = 0 ; i < RESOURCE_QUEUE_RATIO_SIZE ; ++i)
    {
    	PQUEMGR->RatioReverseIndex[i]     = -1;
    	PQUEMGR->RatioReferenceCounter[i] = 0;
    	PQUEMGR->RatioTrackers[i]         = NULL;
    	initializeDQueue(&(PQUEMGR->RatioWaterMarks[i]), PCONTEXT);
    }

    PQUEMGR->LastCheckingDeadAllocationTime = 0;
    PQUEMGR->LastCheckingQueuedTimeoutTime  = 0;
    PQUEMGR->GRMQueueMaxCapacity 			= 1.0;
    PQUEMGR->GRMQueueCapacity				= 1.0;
    PQUEMGR->GRMQueueCurCapacity			= 0.0;
    PQUEMGR->GRMQueueResourceTight			= false;
    PQUEMGR->toRunQueryDispatch 			= false;
}

/*
 * Recognize DDL attributes and shallow parse to fine grained attributes. This
 * function should be expanded when we hope to map one DDL attribute to more
 * than one TABLE attributes or we hope to reformat the value.
 */
int shallowparseResourceQueueWithAttributes(List 	*rawattr,
											List   **fineattr,
											char  	*errorbuf,
											int		 errorbufsize)
{
	ListCell *cell = NULL;
	foreach(cell, rawattr)
	{
		KVProperty property = lfirst(cell);

		if ( SimpleStringComp(&(property->Key),
							  (char *)getRSQTBLAttributeName(RSQ_TBL_ATTR_NAME)) == 0 )
		{
			KVProperty newprop = createPropertyString(
									 PCONTEXT,
									 NULL,
									 getRSQTBLAttributeName(RSQ_TBL_ATTR_NAME),
									 NULL,
									 property->Val.Str);

			MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
			*fineattr = lappend(*fineattr, newprop);
			MEMORY_CONTEXT_SWITCH_BACK
			continue;
		}

		int attrindex = getRSQDDLAttributeNameIndex(&(property->Key));
		if ( attrindex == -1 )
		{
			snprintf(errorbuf, errorbufsize,
					 "Not defined DDL attribute name [%s]",
					 property->Key.Str);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return RMDDL_WRONG_ATTRNAME;
		}

		switch(attrindex)
		{
		case RSQ_DDL_ATTR_PARENT:
		{
			/* Find oid of the parent resource queue. */
			bool exist = false;
			DynResourceQueueTrack parentque = getQueueTrackByQueueName(
												  property->Val.Str,
										 	 	  property->Val.Len,
												  &exist);
			if ( !exist )
			{
				snprintf(errorbuf, errorbufsize,
						 "Can not recognize parent resource queue name %s.",
						 property->Val.Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return RMDDL_WRONG_ATTRVALUE;
			}
			Assert( parentque != NULL );

			/* Build property. */
			Oid parentoid = (Oid) (parentque->QueueInfo->OID);
			KVProperty newprop = createPropertyOID(
									 PCONTEXT,
									 NULL,
									 getRSQTBLAttributeName(RSQ_TBL_ATTR_PARENT),
									 NULL,
									 parentoid);
			MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
			*fineattr = lappend(*fineattr, newprop);
			MEMORY_CONTEXT_SWITCH_BACK
			break;
		}
		case RSQ_DDL_ATTR_ACTIVE_STATMENTS:
		case RSQ_DDL_ATTR_MEMORY_LIMIT_CLUSTER:
		case RSQ_DDL_ATTR_CORE_LIMIT_CLUSTER:
		case RSQ_DDL_ATTR_VSEGMENT_RESOURCE_QUOTA:
		case RSQ_DDL_ATTR_ALLOCATION_POLICY:
		case RSQ_DDL_ATTR_RESOURCE_UPPER_FACTOR:
		case RSQ_DDL_ATTR_VSEGMENT_UPPER_LIMIT:
		{
			/*
			 * Build property.
			 *
			 * NOTE, this logic works, because there is a premise.
			 * RSQ_TBL_ATTR_XXX == RSQ_DDL_ATTR_XXX, for all enum values of
			 * RESOURCE_QUEUE_DDL_ATTR_INDEX.
			 *
			 */
			KVProperty newprop = createPropertyString(
									 PCONTEXT,
									 NULL,
									 getRSQTBLAttributeName(attrindex),
									 NULL,
									 property->Val.Str);

			MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
			*fineattr = lappend(*fineattr, newprop);
			MEMORY_CONTEXT_SWITCH_BACK
			break;
		}
		default:
			Assert(false); /* Should never occur. */
		}

	DQUEUE_LOOP_END

	return FUNC_RETURN_OK;
}

/*
 * This function parses the attributes and translate into DynResourceQueue
 * struct's attributes. This functions does not generate logs higher than
 * WARNING, the concrete error is also saved in error buffer to make the caller
 * able to pass back the message to remote process.
 */
int parseResourceQueueAttributes( List 			 	*attributes,
								  DynResourceQueue 	 queue,
								  char 				*errorbuf,
								  int   			 errorbufsize)
{
	int 			res 		 = FUNC_RETURN_OK;
	int 			attrindex 	 = -1;
	Oid 			parentid;
	Oid				oid;

	bool			memlimit_percentage  = false;
	bool			memlimit_value		 = false;
	bool			corelimit_percentage = false;
	bool			corelimit_value		 = false;

	SimpStringPtr 	attrname  			 = NULL;
	SimpStringPtr 	attrvalue 			 = NULL;

	Assert( queue != NULL );

	/* Initialize attributes. */
	queue->OID						= -1;
	queue->ParentOID 				= -1;
	queue->ParallelCount			= -1;
	queue->ClusterMemoryMB			= -1;
	queue->Status					= RESOURCE_QUEUE_STATUS_VALID_LEAF;

	queue->ClusterVCore				= -1.0;
	queue->SegResourceQuotaVCore 	= -1.0;
	queue->SegResourceQuotaMemoryMB = -1;

	queue->ResourceUpperFactor 		= -1;
	queue->VSegUpperLimit			= DEFAULT_RESQUEUE_VSEG_UPPER_LIMIT_N;

	queue->AllocatePolicy 			= -1;
	queue->QueuingPolicy 			= -1;
	queue->InterQueuePolicy 		= -1;

	queue->ClusterMemoryPer			= -1;
	queue->ClusterVCorePer			= -1;

	memset(queue->Name, '\0', sizeof(queue->Name));

	/* Go through each property content. */
	ListCell *cell = NULL;
	foreach(cell, attributes)
	{
		KVProperty value = lfirst(cell);
		attrname  = &(value->Key);
		attrvalue = &(value->Val);

		attrindex = getRSQTBLAttributeNameIndex(attrname);

		if ( SimpleStringEmpty(attrvalue) )
		{
			elog(DEBUG3, "No value for attribute %s.", attrname->Str);
			continue;
		}

		if ( attrindex == -1 )
		{
			res = RESQUEMGR_WRONG_ATTRNAME;
			snprintf(errorbuf, errorbufsize,
					 "Can not recognize resource queue attribute %s",
					 attrname->Str);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}

		/*
		 * Actually parse each attribute.
		 */
		switch(attrindex)
		{
		case RSQ_TBL_ATTR_OID:
			res = SimpleStringToOid(attrvalue, &oid);
			queue->OID = oid;
			break;

		case RSQ_TBL_ATTR_PARENT:
			res = SimpleStringToOid(attrvalue, &parentid);
			queue->ParentOID = parentid;
			break;

		case RSQ_TBL_ATTR_ACTIVE_STATMENTS:
			res = SimpleStringToInt32(attrvalue, &(queue->ParallelCount));
			if ( res != FUNC_RETURN_OK )
			{
				snprintf(errorbuf, errorbufsize,
						 "Active statements %s is not valid.",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
			elog(DEBUG3, "Resource manager parseResourceQueueAttributes() parsed "
						 "active statements %d",
						 queue->ParallelCount);
			break;

		case RSQ_TBL_ATTR_MEMORY_LIMIT_CLUSTER:
			if ( SimpleStringIsPercentage(attrvalue) )
			{
				memlimit_percentage = true;
				int8_t inputval = 0;
				res = SimpleStringToPercentage(attrvalue, &inputval);
				queue->ClusterMemoryPer = inputval;
				queue->Status |= RESOURCE_QUEUE_STATUS_EXPRESS_PERCENT;
			}
			else
			{
				memlimit_value = true;
				res = SimpleStringToStorageSizeMB(attrvalue,
												  &(queue->ClusterMemoryMB));
			}
			break;

		case RSQ_TBL_ATTR_CORE_LIMIT_CLUSTER:
			if ( SimpleStringIsPercentage(attrvalue) )
			{
				corelimit_percentage = true;
				int8_t inputval = 0;
				res = SimpleStringToPercentage(attrvalue, &inputval);
				queue->ClusterVCorePer = inputval;
				queue->Status |= RESOURCE_QUEUE_STATUS_EXPRESS_PERCENT;
			}
			else
			{
				corelimit_value = true;
				res = SimpleStringToDouble(attrvalue, &(queue->ClusterVCore));
			}
			break;

		case RSQ_TBL_ATTR_VSEGMENT_RESOURCE_QUOTA:
			/* Decide it is a memory quota or core quota. */
			if ( SimpleStringStartWith(
						attrvalue,
						RESOURCE_QUEUE_SEG_RES_QUOTA_MEM) == FUNC_RETURN_OK )
			{
				SimpString valuestr;
				setSimpleStringRef(
					&valuestr,
					attrvalue->Str+sizeof(RESOURCE_QUEUE_SEG_RES_QUOTA_MEM)-1,
					attrvalue->Len-sizeof(RESOURCE_QUEUE_SEG_RES_QUOTA_MEM)+1);

				res = SimpleStringToStorageSizeMB(
						&valuestr,
						&(queue->SegResourceQuotaMemoryMB));

				elog(DEBUG3, "Resource manager parseResourceQueueAttributes() "
							 "parsed segment resource quota %d MB",
							 queue->SegResourceQuotaMemoryMB);

			}
			else if ( SimpleStringStartWith(
							attrvalue,
							RESOURCE_QUEUE_SEG_RES_QUOTA_CORE) == FUNC_RETURN_OK )
			{
				SimpString valuestr;
				setSimpleStringRef(
					&valuestr,
					attrvalue->Str+sizeof(RESOURCE_QUEUE_SEG_RES_QUOTA_CORE)-1,
					attrvalue->Len-sizeof(RESOURCE_QUEUE_SEG_RES_QUOTA_CORE)+1);

				res = SimpleStringToDouble(&valuestr,
						   	   	   	   	   &(queue->SegResourceQuotaVCore));

				elog(DEBUG3, "Resource manager parseResourceQueueAttributes() "
							 "parsed segment resource quota %lf CORE",
							 queue->SegResourceQuotaVCore);
			}
			else
			{
				snprintf(errorbuf, errorbufsize,
						 "Resource quota limit %s is not valid.",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
			break;

		case RSQ_DDL_ATTR_RESOURCE_UPPER_FACTOR:
			res = SimpleStringToDouble(attrvalue, &(queue->ResourceUpperFactor));
			if ( res != FUNC_RETURN_OK ) {
				snprintf(errorbuf, errorbufsize,
						 "Resource upper factor %s is not valid.",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
			elog(DEBUG3, "Resource manager parseResourceQueueAttributes() parsed "
						 "resource upper factor %lf",
						 queue->ResourceUpperFactor);
			break;

		case RSQ_DDL_ATTR_VSEGMENT_UPPER_LIMIT:
			res = SimpleStringToInt32(attrvalue, &(queue->VSegUpperLimit));
			if ( res != FUNC_RETURN_OK )
			{
				snprintf(errorbuf, errorbufsize,
						 "Virtual segment upper limit %s is not valid.",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
			elog(DEBUG3, "Resource manager parseResourceQueueAttributes() parsed "
						 "virtual segment upper limit %d",
						 queue->VSegUpperLimit);
			break;

		case RSQ_TBL_ATTR_ALLOCATION_POLICY:
			res = SimpleStringToMapIndexInt8(
						attrvalue,
						(char *)RSQDDLValueAllocationPolicy,
						RSQ_ALLOCATION_POLICY_COUNT,
						sizeof(RSQDDLValueAllocationPolicy[0]),
						&(queue->AllocatePolicy));
			if ( res != FUNC_RETURN_OK )
			{
				snprintf(errorbuf, errorbufsize,
						 "Allocation policy %s is not valid.",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
			break;

		case RSQ_TBL_ATTR_NAME:
			queue->NameLen = attrvalue->Len;
			strncpy(queue->Name, attrvalue->Str, sizeof(queue->Name)-1);

			if ( SimpleStringComp(attrvalue,
								  RESOURCE_QUEUE_DEFAULT_QUEUE_NAME) == 0 )
			{
				queue->Status |= RESOURCE_QUEUE_STATUS_IS_DEFAULT;
			}
			else if ( SimpleStringComp(attrvalue,
									   RESOURCE_QUEUE_ROOT_QUEUE_NAME) == 0 )
			{
				queue->Status |= RESOURCE_QUEUE_STATUS_IS_ROOT;
			}
			break;

		case RSQ_TBL_ATTR_STATUS:
			/*
			 * 'branch' indicates one branch queue in-use.
			 * 'invalidate' indicates the queue is invalid, and not in-use.
			 */
			if ( SimpleStringFind(attrvalue, "branch") == FUNC_RETURN_OK )
			{
				queue->Status |= RESOURCE_QUEUE_STATUS_VALID_BRANCH;
			}
			if ( SimpleStringFind(attrvalue, "invalid") == FUNC_RETURN_OK )
			{
				queue->Status |= RESOURCE_QUEUE_STATUS_VALID_INVALID;
			}

			if ( !RESQUEUE_IS_BRANCH(queue) )
			{
				queue->Status |= RESOURCE_QUEUE_STATUS_VALID_LEAF;
			}

			if ( (queue->Status & RESOURCE_QUEUE_STATUS_VALID_INVALID) == 0 )
			{
				queue->Status |= RESOURCE_QUEUE_STATUS_VALID_INUSE;
			}
			break;

		case RSQ_TBL_ATTR_CREATION_TIME:
		case RSQ_TBL_ATTR_UPDATE_TIME:
			break;
		default:
			/* Should not occur. Invalid attribute name has been checked. */
			Assert(false);
		}

		if ( res != FUNC_RETURN_OK )
		{
			res = RESQUEMGR_WRONG_ATTR;
			snprintf(errorbuf, errorbufsize,
					 "Wrong resource queue attribute setting. %s=%s",
					 attrname->Str, attrvalue->Str);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}
	}

	/*
	 * Memory and Core resource must be specified and they must use the same way
	 * to express the resource.
	 */
	if ( !memlimit_value && !memlimit_percentage )
	{
		res = RESQUEMGR_LACK_ATTR;
		snprintf(errorbuf, errorbufsize,
				 "MEMORY_LIMIT_CLUSTER must be specified.");
		ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
		return res;
	}

	if ( !corelimit_value && !corelimit_percentage )
	{
		res = RESQUEMGR_LACK_ATTR;
		snprintf(errorbuf, errorbufsize,
				 "CORE_LIMIT_CLUSTER must be specified.");
		ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
		return res;
	}

	if ( (memlimit_value 		&& corelimit_percentage	) ||
		 (memlimit_percentage 	&& corelimit_value		)	)
	{
		res = RESQUEMGR_INCONSISTENT_RESOURCE_EXP;
		snprintf(errorbuf, errorbufsize,
				 "MEMORY_LIMIT_CLUSTER and CORE_LIMIT_CLUSTER "
				 "must use the same way to express resource limit.");
		ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
		return res;
	}

	if ( memlimit_percentage && corelimit_percentage )
	{
		queue->Status |= RESOURCE_QUEUE_STATUS_EXPRESS_PERCENT;
	}
	else
	{
		Assert(memlimit_value && corelimit_value);
	}
	return res;
}

/*
 * This function parses the attributes and translate into an exist DynResourceQueue
 * struct's attributes. This functions does not generate logs higher than
 * WARNING, the concrete error is also saved in error buffer to make the caller
 * able to pass back the message to remote process.
 *
 */
int updateResourceQueueAttributes(List 			 	*attributes,
								  DynResourceQueue 	 queue,
								  char 				*errorbuf,
								  int   			 errorbufsize)
{
	int			  res 		 		= FUNC_RETURN_OK;
	int 		  attrindex 	 	= -1;
	int			  percentage_change = 0;
	int			  value_change		= 0;

	SimpStringPtr attrname  		= NULL;
	SimpStringPtr attrvalue 		= NULL;

	Assert(queue != NULL);

	/* We can not have duplicate property keys. */
	ListCell *cell = NULL;
	foreach(cell, attributes)
	{
		KVProperty value1 = lfirst(cell);

		for ( ListCell *cell2 = lnext(cell) ; cell2 != NULL ; cell2 = lnext(cell2) )
		{
			KVProperty value2 = lfirst(cell2);
			if ( SimpleStringComp(&(value1->Key), value2->Key.Str) == 0 )
			{
				res = RESQUEMGR_DUPLICATE_ATTRNAME;
				snprintf(errorbuf, errorbufsize, "Duplicate attributes %s", value1->Key.Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
		}
	}

	/* Go through each property content. */
	foreach(cell, attributes)
	{
		KVProperty value = lfirst(cell);

		attrname  = &(value->Key);
		attrvalue = &(value->Val);

		attrindex = getRSQTBLAttributeNameIndex(attrname);

		if ( SimpleStringEmpty(attrvalue) )
		{
			elog(DEBUG3, "No value for attribute %s.", attrname->Str);
			continue;
		}

		if ( attrindex == -1 )
		{
			res = RESQUEMGR_WRONG_ATTRNAME;
			snprintf(errorbuf, errorbufsize,
					 "Resource manager can not recognize resource queue attribute "
					 "%s",
					 attrname->Str);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}

		/*
		 * Actually parse each attribute.
		 */
		switch(attrindex)
		{
		case RSQ_TBL_ATTR_OID:
			res = RESQUEMGR_WRONG_ATTRNAME;
			snprintf(errorbuf, errorbufsize, "Can not alter resource queue OID ");
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;

		case RSQ_TBL_ATTR_PARENT:
			res = RESQUEMGR_WRONG_ATTRNAME;
			snprintf(errorbuf, errorbufsize, "Can not alter resource queue parent name");
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		case RSQ_TBL_ATTR_NAME:
			break;

		case RSQ_TBL_ATTR_ACTIVE_STATMENTS:
			res = SimpleStringToInt32(attrvalue, &(queue->ParallelCount));
			if ( res != FUNC_RETURN_OK )
			{
				snprintf(errorbuf, errorbufsize,
						 "Active statements %s is not valid.",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}

			elog(DEBUG3, "Resource manager updateResourceQueueAttributes() "
						 "updated active statements %d",
						 queue->ParallelCount);
			break;

		case RSQ_TBL_ATTR_MEMORY_LIMIT_CLUSTER:
			if ( SimpleStringIsPercentage(attrvalue) )
			{
				percentage_change += 1;
				int8_t inputval = 0;
				res = SimpleStringToPercentage(attrvalue, &inputval);
				queue->ClusterMemoryPer = inputval;
			}
			else
			{
				value_change  += 1;
				res = SimpleStringToStorageSizeMB(attrvalue,
												  &(queue->ClusterMemoryMB));
			}
			break;

		case RSQ_TBL_ATTR_CORE_LIMIT_CLUSTER:
			if ( SimpleStringIsPercentage(attrvalue) )
			{
				percentage_change += 1;
				int8_t inputval = 0;
				res = SimpleStringToPercentage(attrvalue, &inputval);
				queue->ClusterVCorePer = inputval;
			}
			else
			{
				value_change  += 1;
				res = SimpleStringToDouble(attrvalue,
										   &(queue->ClusterVCore));
			}
			break;

		case RSQ_TBL_ATTR_VSEGMENT_RESOURCE_QUOTA:
			/* Decide it is a memory quota or core quota. */
			if ( SimpleStringStartWith(
						attrvalue,
						RESOURCE_QUEUE_SEG_RES_QUOTA_MEM) == FUNC_RETURN_OK )
			{
				SimpString valuestr;
				setSimpleStringRef(
					&valuestr,
					attrvalue->Str+sizeof(RESOURCE_QUEUE_SEG_RES_QUOTA_MEM)-1,
					attrvalue->Len-sizeof(RESOURCE_QUEUE_SEG_RES_QUOTA_MEM)+1);

				res = SimpleStringToStorageSizeMB(
						&valuestr,
						&(queue->SegResourceQuotaMemoryMB));

				queue->SegResourceQuotaVCore = -1;
				elog(DEBUG3, "Resource manager updateResourceQueueAttributes() "
							 "updated segment resource quota %d MB",
							 queue->SegResourceQuotaMemoryMB);

			}
			else if ( SimpleStringStartWith(
							attrvalue,
							RESOURCE_QUEUE_SEG_RES_QUOTA_CORE) == FUNC_RETURN_OK )
			{
				SimpString valuestr;
				setSimpleStringRef(
					&valuestr,
					attrvalue->Str+sizeof(RESOURCE_QUEUE_SEG_RES_QUOTA_CORE)-1,
					attrvalue->Len-sizeof(RESOURCE_QUEUE_SEG_RES_QUOTA_CORE)+1);

				res = SimpleStringToDouble(&valuestr,
						   	   	   	   	   &(queue->SegResourceQuotaVCore));

				queue->SegResourceQuotaMemoryMB = -1;
				elog(DEBUG3, "Resource manager updateResourceQueueAttributes() "
							 "updated segment resource quota %lf CORE",
							 queue->SegResourceQuotaVCore);
			}
			else
			{
				snprintf(errorbuf, errorbufsize,
						 "Resource quota limit %s is not valid.",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
			break;

		case RSQ_DDL_ATTR_RESOURCE_UPPER_FACTOR:
			res = SimpleStringToDouble(attrvalue, &(queue->ResourceUpperFactor));
			if ( res != FUNC_RETURN_OK )
			{
				snprintf(errorbuf, errorbufsize,
						 "Resource upper limit factor %s is not valid.",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
			elog(DEBUG3, "Resource manager updateResourceQueueAttributes() "
						 "updated Resource upper limit factor %lf",
						 queue->ResourceUpperFactor);
			break;

		case RSQ_DDL_ATTR_VSEGMENT_UPPER_LIMIT:
			res = SimpleStringToInt32(attrvalue, &(queue->VSegUpperLimit));
			if ( res != FUNC_RETURN_OK )
			{
				snprintf(errorbuf, errorbufsize,
						 "Virtual segment upper limit %s is not valid.",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}

			elog(DEBUG3, "Resource manager updateResourceQueueAttributes() "
						 "updated virtual segment upper limit %d",
						 queue->VSegUpperLimit);
			break;

		case RSQ_TBL_ATTR_ALLOCATION_POLICY:
			res = SimpleStringToMapIndexInt8(
						attrvalue,
						(char *)RSQDDLValueAllocationPolicy,
						RSQ_ALLOCATION_POLICY_COUNT,
						sizeof(RSQDDLValueAllocationPolicy[0]),
						&(queue->AllocatePolicy));
			if ( res != FUNC_RETURN_OK )
			{
				snprintf(errorbuf, errorbufsize,
						 "Allocation policy %s is not valid.",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
			break;

		case RSQ_TBL_ATTR_STATUS:
			res = RESQUEMGR_WRONG_ATTRNAME;
			snprintf(errorbuf, errorbufsize,
					 "Can not alter resource queue status");
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;

		case RSQ_TBL_ATTR_CREATION_TIME:
		case RSQ_TBL_ATTR_UPDATE_TIME:
			break;
		default:
			/* Should not occur. Invalid attribute name has been checked. */
			Assert(false);
		}

		if ( res != FUNC_RETURN_OK )
		{
			res = RESQUEMGR_WRONG_ATTR;
			snprintf(errorbuf, errorbufsize,
					 "Wrong resource queue attribute setting. %s=%s",
					 attrname->Str, attrvalue->Str);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}
	}

	/*
	 * Memory and Core resource must be specified and they must use the same way
	 * to express the resource.
	 */
	if ( RESQUEUE_IS_PERCENT(queue) )
	{
		if (value_change == 1)
		{
			res = RESQUEMGR_INCONSISTENT_RESOURCE_EXP;
			snprintf(errorbuf, errorbufsize,
					 "MEMORY_LIMIT_CLUSTER and CORE_LIMIT_CLUSTER "
					 "must use the same way to express resource limit.");
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}
		if (value_change == 2)
		{
			queue->Status ^= RESOURCE_QUEUE_STATUS_EXPRESS_PERCENT;
		}
	}
	else
	{
		if (value_change == 1)
		{
			res = RESQUEMGR_INCONSISTENT_RESOURCE_EXP;
			snprintf(errorbuf, errorbufsize,
					 "MEMORY_LIMIT_CLUSTER and CORE_LIMIT_CLUSTER "
					 "must use the same way to express resource limit.");
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}
		if (value_change == 2)
		{
			queue->Status |= RESOURCE_QUEUE_STATUS_EXPRESS_PERCENT;
		}
	}
	return res;
}


/**
 * This is one API for checking if new resource queue definition is valid to be
 * created.This functions does not generate logs higher than WARNING, the error
 * is also saved in error buffer to make the caller able to pass the message to
 * remote process.
 *
 * queue[in/out] 		The queue instance to be tested and completed.
 * errorbuf[out]		The error string buffer.
 * errorbufsize[in]		The maximum size of error string buffer.
 *
 * Return values:
 * 		FUNC_RETURN_OK				: Succeed.
 * 		RESQUEMGR_LACK_ATTR			: Necessary attributes are not specified.
 * 		RESQUEMGR_WRONG_ATTR		: Unrecognized wrong attribute value.
 */
int checkAndCompleteNewResourceQueueAttributes(DynResourceQueue  queue,
											   char				*errorbuf,
											   int				 errorbufsize)
{
	DynResourceQueueTrack	parenttrack	 = NULL;
	int 					res 		 = FUNC_RETURN_OK;

	Assert( queue != NULL );

	if ( queue->Status & RESOURCE_QUEUE_STATUS_IS_VER1X ) {

		/* TODO: Validate Version 1.x resource queue definition here. */
		return res;
	}

	/*** Validate HAWQ 2.0 resource queue definition ***/

	/*
	 * STEP 1. Validate parent queue attribute.
	 */
	if ( queue->ParentOID < 0 ) {
		res = RESQUEMGR_LACK_ATTR;
		snprintf(errorbuf, errorbufsize,
				 "Attribute %s must be specified.",
				 RSQDDLAttrNames[RSQ_DDL_ATTR_PARENT]);
		ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
		return res;
	}

	if ( queue->ParentOID != InvalidOid ) {
		bool exist = false;
		parenttrack = getQueueTrackByQueueOID(queue->ParentOID, &exist);
		Assert((exist && parenttrack != NULL) || !exist);

		/* pg_default can not be a parent queue. */
		if ( RESQUEUE_IS_DEFAULT(parenttrack->QueueInfo) ) {
			res = RESQUEMGR_WRONG_ATTR;
			snprintf(errorbuf, errorbufsize,
					 "pg_default can not have children resource queues.");
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}
	}

	/*
	 * Parent queue must exist. Basically, default queue is already created as
	 * the root of whole resource queue tree. This checking is for self-test.
	 */
	if ( RESQUEUE_IS_ROOT(queue) )
	{
		Assert(queue->ParentOID == InvalidOid);
		parenttrack = NULL;
	}

	/*
	 * STEP 2. Validate active_statements attributes. For leaf queue only.
	 */

	if ( queue->ParallelCount <= 0 ) {
		queue->ParallelCount = RESOURCE_QUEUE_PARALLEL_COUNT_DEF;
	}

	/*
     * STEP 3. Validate resource limit attributes.
	 */

	/*======================================*/
	/* STEP 3 CASE1: percentage expression. */
	/*======================================*/
	if ( RESQUEUE_IS_PERCENT(queue) )
	{
		/* MEMORY_LIMIT_CLUSTER and CORE_LIMIT_CLUSTER must be specified.*/
		if ( queue->ClusterMemoryPer == -1 ) {
			res = RESQUEMGR_LACK_ATTR;
			snprintf(
				errorbuf, errorbufsize,
				"%s must be set.",
				RSQDDLAttrNames[RSQ_DDL_ATTR_MEMORY_LIMIT_CLUSTER]);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}

		if ( queue->ClusterVCorePer == -1 ) {
			res = RESQUEMGR_LACK_ATTR;
			snprintf(
				errorbuf, errorbufsize,
				"%s must be set.",
				RSQDDLAttrNames[RSQ_DDL_ATTR_CORE_LIMIT_CLUSTER]);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}

		/*
		 * The values of MEMORY_LIMIT_CLUSTER, CORE_LIMIT_CLUSTER must be greater
		 * than 0, less than 100. This is to guarantee the following automatic
		 * deduction of the limits.
		 */
		if ( queue->ClusterVCorePer <= 0 || queue->ClusterVCorePer > 100 ) {
			res = RESQUEMGR_WRONG_ATTR;
			snprintf(
				errorbuf, errorbufsize,
				"The explicit value of %s must be between 1%% and 100%%. "
				"Wrong value = %lf%%",
				RSQDDLAttrNames[RSQ_DDL_ATTR_MEMORY_LIMIT_CLUSTER],
				queue->ClusterVCorePer);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}

		if ( queue->ClusterMemoryPer <= 0 || queue->ClusterMemoryPer > 100 ) {
			res = RESQUEMGR_WRONG_ATTR;
			snprintf(
				errorbuf, errorbufsize,
				"The explicit value of %s must be between 1%% and 100%%. "
				"Wrong value = %lf%%",
				RSQDDLAttrNames[RSQ_DDL_ATTR_CORE_LIMIT_CLUSTER],
				queue->ClusterMemoryPer);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}

		/*
		 * The values of MEMORY_LIMIT_CLUSER, CORE_LIMIT_CLUSTER must be
		 * identical.
		 */
		if ( queue->ClusterVCorePer != queue->ClusterMemoryPer )
		{
			res = RESQUEMGR_WRONG_ATTR;
			snprintf(
				errorbuf, errorbufsize,
				"The value of %s must be identical with the value of %s. "
				"Wrong value of %s = %lf%%. "
				"Wrong value of %s = %lf%%. ",
				RSQDDLAttrNames[RSQ_DDL_ATTR_MEMORY_LIMIT_CLUSTER],
				RSQDDLAttrNames[RSQ_DDL_ATTR_CORE_LIMIT_CLUSTER],
				RSQDDLAttrNames[RSQ_DDL_ATTR_MEMORY_LIMIT_CLUSTER],
				queue->ClusterMemoryPer,
				RSQDDLAttrNames[RSQ_DDL_ATTR_CORE_LIMIT_CLUSTER],
				queue->ClusterVCorePer);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}

		/*
		 * check siblings' resource limit
		 */
		if (queue->ParentOID != InvalidOid)
		{
			double current = 0.0;
			bool   exist   = false;
			parenttrack = getQueueTrackByQueueOID(queue->ParentOID, &exist);
			if (exist && parenttrack != NULL)
			{
				ListCell *cell = NULL;
				foreach(cell, parenttrack->ChildrenTracks)
				{
					DynResourceQueueTrack track = (DynResourceQueueTrack)lfirst(cell);
					if (strncmp(track->QueueInfo->Name, queue->Name, queue->NameLen) != 0)
					{
						current += track->QueueInfo->ClusterMemoryPer;
					}
				}

				if ((current + queue->ClusterMemoryPer) > 100)
				{
					res = RESQUEMGR_WRONG_ATTR;
					snprintf(
						errorbuf, errorbufsize,
						"The value of %s and %s exceeds its parent's limit. "
						"Wrong value = %lf%%",
						RSQDDLAttrNames[RSQ_DDL_ATTR_CORE_LIMIT_CLUSTER],
						RSQDDLAttrNames[RSQ_DDL_ATTR_MEMORY_LIMIT_CLUSTER],
						queue->ClusterMemoryPer);
					ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
					return res;
				}
			}
		}
	}
	/*================================*/
	/* STEP 3 CASE2: value expression. */
	/*================================*/
	else {

		/* MEMORY_LIMIT_CLUSTER and CORE_LIMIT_CLUSTER must be specified.*/
		if ( queue->ClusterMemoryMB == -1 ) {
			res = RESQUEMGR_LACK_ATTR;
			snprintf(
				errorbuf, errorbufsize,
				"%s must be set.",
				RSQDDLAttrNames[RSQ_DDL_ATTR_MEMORY_LIMIT_CLUSTER]);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}

		if ( queue->ClusterVCore == -1 ) {
			res = RESQUEMGR_LACK_ATTR;
			snprintf(
				errorbuf, errorbufsize,
				"%s must be set.",
				RSQDDLAttrNames[RSQ_DDL_ATTR_CORE_LIMIT_CLUSTER]);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}

		/*
		 * The values of MEMORY_LIMIT_CLUSTER, CORE_LIMIT_CLUSTER must be greater
		 * than 0. This is to guarantee the following automatic deduction of the
		 * limits.
		 */
		if ( queue->ClusterVCore <= 0  ) {
			res = RESQUEMGR_WRONG_ATTR;
			snprintf(
				errorbuf, errorbufsize,
				"The explicit value of %s must be greater than 0. "
				"Wrong value = %f",
				RSQDDLAttrNames[RSQ_DDL_ATTR_CORE_LIMIT_CLUSTER],
				queue->ClusterVCore);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}

		if ( queue->ClusterMemoryMB <= 0 ) {
			res = RESQUEMGR_WRONG_ATTR;
			snprintf(
				errorbuf, errorbufsize,
				"The explicit value of %s must be greater than 0. "
				"Wrong value = %dMB",
				RSQTBLAttrNames[RSQ_DDL_ATTR_MEMORY_LIMIT_CLUSTER],
				queue->ClusterMemoryMB);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}
	}

	/*
	 * STEP 4: Check resource quota.
	 */
	if ( queue->SegResourceQuotaMemoryMB == -1 &&
		 queue->SegResourceQuotaVCore    == -1.0 ) {
		queue->SegResourceQuotaMemoryMB = RESOURCE_QUEUE_SEG_RES_QUOTA_DEF;
	}

	if ( queue->SegResourceQuotaMemoryMB != -1 ) {
		/* The quota value must be greater than 0. */
		if ( queue->SegResourceQuotaMemoryMB <= 0 ) {
			res = RESQUEMGR_WRONG_ATTR;
			snprintf(
				errorbuf, errorbufsize,
				"%s must be greater than 0.",
				RSQDDLAttrNames[RSQ_DDL_ATTR_VSEGMENT_RESOURCE_QUOTA]);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}
	}
	else if ( queue->SegResourceQuotaVCore != -1.0 ) {

		/* The quota value must be greater than 0. */
		if ( queue->SegResourceQuotaVCore <= 0.0 ) {
			res = RESQUEMGR_WRONG_ATTR;
			snprintf(
				errorbuf, errorbufsize,
				"%s must be greater than 0.0.",
				RSQTBLAttrNames[RSQ_DDL_ATTR_VSEGMENT_RESOURCE_QUOTA]);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}
	}
	else {
		Assert(0); /* Should never come here. */
	}

	/*
	 * STEP 5: Check policy and set default value.
	 */
	if ( queue->AllocatePolicy == -1 ) {
		queue->AllocatePolicy = RSQ_ALLOCATION_POLICY_EVEN;
	}

	/*
	 * STEP 6: Check resource factors.
	 */
	if ( queue->ResourceUpperFactor == -1.0 ) {
		queue->ResourceUpperFactor = RESOURCE_QUEUE_RES_UPPER_FACTOR_DEF;
	}

	if ( queue->ResourceUpperFactor < 1.0 ) {
		res = RESQUEMGR_WRONG_ATTR;
		snprintf(
			errorbuf, errorbufsize,
			"%s must be no less than 1.0. Wrong value %lf",
			RSQDDLAttrNames[RSQ_DDL_ATTR_RESOURCE_UPPER_FACTOR],
			queue->ResourceUpperFactor);
		ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
		return res;
	}

	return res;
}

/**
 * Create queue definition and tracker in the resource queue manager.
 *
 * queue[in]			The resource queue definition instance.
 * track[out]			The corresponding resource queue tracker instance. This
 * 						is a new created instance.
 * errorbuf[out]		The error message if something is wrong.
 * errorbufsize[out]	The limit of error message buffer.
 *
 * Return	FUNC_RETURN_OK : Everything is ok.
 * 			RESQUEMGR_DUPLICATE_QUEID : Duplicate resource queue id.
 * 			RESQUEMGR_NO_QUENAME : No resource queue name specified.
 *			RESQUEMGR_DUPLICATE_QUENAME : Duplicate queue name.
 *			RESQUEMGR_WRONG_PARENT_QUEUE : The parent queue is wrongly specified.
 *
 */
int createQueueAndTrack( DynResourceQueue		queue,
						 DynResourceQueueTrack *track,
						 char				   *errorbuf,
						 int					errorbufsize)
{
	int 				  	res 			 = FUNC_RETURN_OK;
	DynResourceQueueTrack	parenttrack		 = NULL;
    DynResourceQueueTrack 	newqueuetrack 	 = NULL;
    bool					exist			 = false;
    bool					isDefaultQueue   = false;
    bool					isRootQueue		 = false;

    /* Input validation. */
    Assert(track != NULL);
    Assert(queue != NULL);

	/*
	 * Create new queue tracking instance. If there is something wrong, this
	 * instance will be freed.
	 */
	newqueuetrack = createDynResourceQueueTrack(queue);

    /*
     * Check queue oid ( for loading existing queues only ). In case loading
     * existing queues from file or catalog, the oid should be explicitly
     * specified. In case the queue is to be created, no need to check this.
     */
    if ( queue->OID > InvalidOid )
    {
		getQueueTrackByQueueOID(queue->OID, &exist);
		if (exist)
		{
			res = RESQUEMGR_DUPLICATE_QUEID;
			snprintf(errorbuf, errorbufsize,
					 ERRORPOS_FORMAT "Duplicate queue ID " INT64_FORMAT
					 " for new resource queue.",
					 ERRREPORTPOS,
					 queue->OID);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			goto exit;
		}
    }

    /* New queue name must be set and unique. */
	if ( queue->NameLen <= 0 )
	{
		res = RESQUEMGR_NO_QUENAME;
		snprintf(errorbuf, errorbufsize,
				 ERRORPOS_FORMAT "Unset queue name string.",
			     ERRREPORTPOS);
		ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
		goto exit;
	}

	getQueueTrackByQueueName((char *)(queue->Name), queue->NameLen, &exist);
	if (exist) {
		res = RESQUEMGR_DUPLICATE_QUENAME;
		snprintf(errorbuf, errorbufsize,
				 ERRORPOS_FORMAT
				 "Duplicate queue name %s for creating resource queue.",
			     ERRREPORTPOS,
			    queue->Name);
		ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
		goto exit;
	}

	/* Decide if the queue is special one: pg_root or pg_default */
	isDefaultQueue = RESQUEUE_IS_DEFAULT(queue);
	isRootQueue	   = RESQUEUE_IS_ROOT(queue);

	elog(RMLOG, "HAWQ RM :: To create resource queue instance %s", queue->Name);

	/*
	 * Check the queue parent-child relationship. No matter the queue is to be
	 * created or to be loaded, the parent queue must have been loaded. The only
	 * queue has no parent is 'pg_root' say isRootQueue. The queue 'pg_default'
	 * must has 'pg_root' as the parent queue.
	 */
	if ( !isRootQueue ) {
		/* Check if the parent queue id exists. */
		parenttrack = getQueueTrackByQueueOID(queue->ParentOID, &exist);
		if (exist) {
			/* Can not set pg_default as parent queue. */
			if ( RESQUEUE_IS_DEFAULT(parenttrack->QueueInfo) ) {
				res = RESQUEMGR_WRONG_PARENT_QUEUE;
				snprintf( errorbuf, errorbufsize,
						  ERRORPOS_FORMAT
						  "The parent queue of can not be pg_default.",
						  ERRREPORTPOS);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				goto exit;
			}

			/* 'pg_default' must has 'pg_root' as parent. */
			if ( isDefaultQueue &&
				 !RESQUEUE_IS_ROOT(parenttrack->QueueInfo) ) {
				res = RESQUEMGR_WRONG_PARENT_QUEUE;
				snprintf( errorbuf, errorbufsize,
						  ERRORPOS_FORMAT
						  "The parent queue of pg_default must be pg_root.",
						  ERRREPORTPOS);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				goto exit;
			}

			/* The parent queue can not have connections. */
			if ( parenttrack->CurConnCounter > 0 ) {
				res = RESQUEMGR_IN_USE;
				snprintf( errorbuf, errorbufsize,
						  ERRORPOS_FORMAT
						  "The parent queue %s has active connections.",
						  ERRREPORTPOS,
						  parenttrack->QueueInfo->Name);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				goto exit;
			}
		}
		else {
			res = RESQUEMGR_WRONG_PARENT_QUEUE;
			snprintf(errorbuf, errorbufsize,
					 ERRORPOS_FORMAT "No expected parent queue " INT64_FORMAT,
					 ERRREPORTPOS,
					 queue->ParentOID);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			goto exit;
		}

		/*
		 * If the parent track changes the role from LEAF to BRANCH, its memory
		 * core ratio related information should also be updated.
		 */
		if ( RESQUEUE_IS_LEAF(parenttrack->QueueInfo) &&
			 parenttrack->trackedMemCoreRatio ) {

			/* Remove parent track from memory core ratio track */
			removeResourceQueueRatio(parenttrack);

			/* Change parent track to branch queue. */
			parenttrack->QueueInfo->Status &= NOT_RESOURCE_QUEUE_STATUS_VALID_LEAF;
			parenttrack->QueueInfo->Status |= RESOURCE_QUEUE_STATUS_VALID_BRANCH;
		}
	}



	/* Set parent resource queue track reference. */
	newqueuetrack->ParentTrack = parenttrack;

	/* Build resource queue tree structure. Save 'pg_root' and 'pg_default' */
	if ( isRootQueue )
	{
		PQUEMGR->RootTrack = newqueuetrack;
	}
	else
	{
		MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
		parenttrack->ChildrenTracks = lappend(parenttrack->ChildrenTracks,
											  newqueuetrack);
		MEMORY_CONTEXT_SWITCH_BACK
	}

	if ( isDefaultQueue )
	{
		PQUEMGR->DefaultTrack = newqueuetrack;
	}

	/* Save the queue track into the list and build index.*/
	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	PQUEMGR->Queues = lappend(PQUEMGR->Queues, newqueuetrack);
	MEMORY_CONTEXT_SWITCH_BACK
	if( newqueuetrack->QueueInfo->OID != InvalidOid ) {
		setQueueTrackIndexedByQueueOID(newqueuetrack);
	}
	setQueueTrackIndexedByQueueName(newqueuetrack);

	/* Update overall ratio index. */
	if ( !RESQUEUE_IS_PERCENT(newqueuetrack->QueueInfo) ) {
		newqueuetrack->MemCoreRatio =
			trunc(newqueuetrack->QueueInfo->ClusterMemoryMB /
				  newqueuetrack->QueueInfo->ClusterVCore);
		addResourceQueueRatio(newqueuetrack);
	}

	/* Set return value. */
	*track = newqueuetrack;

exit:
	if ( res != FUNC_RETURN_OK ) {
		/* Free resource queue track instance. */
		freeDynResourceQueueTrack(newqueuetrack);
		*track = NULL;
	}
	return res;
}

int dropQueueAndTrack( DynResourceQueueTrack track,
					   char				     *errorbuf,
					   int					  errorbufsize)
{
	int 				  	res 			 = FUNC_RETURN_OK;
	DynResourceQueueTrack	parenttrack		 = NULL;
	ListCell			   *cell			 = NULL;
	ListCell			   *prevcell		 = NULL;

	/* remove track from parent queue's children list */
	parenttrack = track->ParentTrack;
	foreach(cell, parenttrack->ChildrenTracks)
	{
		DynResourceQueueTrack todeltrack = (DynResourceQueueTrack)lfirst(cell);
		if ( todeltrack == track )
		{
			MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
			parenttrack->ChildrenTracks = list_delete_cell(parenttrack->ChildrenTracks,
														   cell,
														   prevcell);
			MEMORY_CONTEXT_SWITCH_BACK
			break;
		}
		prevcell = cell;
	}

	/*
	 * If the resource queue has been assigned by a memory/core ratio, the
	 * corresponding reference in memory/core ratio tracker should also be
	 * updated.
	 */
	if ( track->trackedMemCoreRatio ) {
		removeResourceQueueRatio(track);
	}

	/* Directly remove the instances. */
	removeQueueTrackIndexedByQueueName(track);
	removeQueueTrackIndexedByQueueOID(track);

	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	cell 	 = NULL;
	prevcell = NULL;
	foreach(cell, PQUEMGR->Queues)
	{
		DynResourceQueueTrack todeltrack = lfirst(cell);
		if ( todeltrack == track )
		{
			PQUEMGR->Queues = list_delete_cell(PQUEMGR->Queues, cell, prevcell);
			break;
		}
		prevcell = cell;
	}
	MEMORY_CONTEXT_SWITCH_BACK

	rm_pfree(PCONTEXT, track->QueueInfo);

	freeDynResourceQueueTrack(track);
	return res;
}


DynResourceQueueTrack getQueueTrackByQueueOID (int64_t 	 queoid,
											   bool		*exist)
{
	PAIR pair = NULL;
	SimpArray key;
	setSimpleArrayRef(&key, (char *)&queoid, sizeof(int64_t));
	pair = getHASHTABLENode(&(PQUEMGR->QueuesIDIndex),
							(void *)&key);
	if ( pair == NULL ) {
		*exist = false;
		return NULL;
	}
	*exist = true;
	return (DynResourceQueueTrack)(pair->Value);
}

DynResourceQueueTrack getQueueTrackByQueueName(char 	*quename,
											   int 		 quenamelen,
											   bool 	*exist)
{
	SimpString quenamestr;
	setSimpleStringRef(&quenamestr, quename, quenamelen);
	PAIR pair = getHASHTABLENode(&(PQUEMGR->QueuesNameIndex),
								 (void *)(&quenamestr));
	if ( pair == NULL ) {
		*exist = false;
		return NULL;
	}
	*exist = true;
	return (DynResourceQueueTrack)(pair->Value);
}

void setQueueTrackIndexedByQueueOID(DynResourceQueueTrack queuetrack)
{
	SimpArray key;
	setSimpleArrayRef(&key,
					  (void *)&(queuetrack->QueueInfo->OID),
					  sizeof(int64_t));
	setHASHTABLENode(&(PQUEMGR->QueuesIDIndex), &key, queuetrack, false);
}

void removeQueueTrackIndexedByQueueOID(DynResourceQueueTrack queuetrack)
{
	SimpArray key;
	setSimpleArrayRef(&key,
					(void *)&(queuetrack->QueueInfo->OID),
					sizeof(int64_t));
	removeHASHTABLENode(&(PQUEMGR->QueuesIDIndex),&key);
}

void setQueueTrackIndexedByQueueName(DynResourceQueueTrack queuetrack)
{
	SimpString quenamestr;
	setSimpleStringRef(&quenamestr,
					   queuetrack->QueueInfo->Name,
					   queuetrack->QueueInfo->NameLen);
	setHASHTABLENode(&(PQUEMGR->QueuesNameIndex),
					 (void *)(&quenamestr),
					 queuetrack,
					 false);
}

void removeQueueTrackIndexedByQueueName(DynResourceQueueTrack queuetrack)
{
	SimpString quenamestr;
	setSimpleStringRef(&quenamestr,
					   queuetrack->QueueInfo->Name,
					   queuetrack->QueueInfo->NameLen);
	removeHASHTABLENode(&(PQUEMGR->QueuesNameIndex), (void *) (&quenamestr));
}

const char *getRSQTBLAttributeName(int attrindex)
{
	Assert( attrindex >= 0 && attrindex < RSQ_TBL_ATTR_COUNT );
	return RSQTBLAttrNames[attrindex];
}

const char *getRSQDDLAttributeName(int colindex)
{
	Assert( colindex >= 0 && colindex < RSQ_DDL_ATTR_COUNT );
	return RSQDDLAttrNames[colindex];
}

/**
 * Get memory/core ratio index. Return -1 if no such ratio tracked.
 */
int32_t getResourceQueueRatioIndex(uint32_t ratio)
{
	if ( ratio <= 0 )
		return -1;
	PAIR ratiopair = getHASHTABLENode(&(PQUEMGR->RatioIndex),
									  TYPCONVERT(void *, ratio));
	return ratiopair == NULL ? -1 : TYPCONVERT(int32_t, ratiopair->Value);
}

DynMemoryCoreRatioTrack createDynMemoryCoreRatioTrack(uint32_t ratio,
													  int32_t index )
{
	DynMemoryCoreRatioTrack res = rm_palloc(PCONTEXT,
				  	  	  	  	  	  	    sizeof(DynMemoryCoreRatioTrackData));
	res->MemCoreRatio 				= ratio;
	res->RatioIndex   				= -1;
	res->ClusterMemory				= 0;
	res->ClusterVCore				= 0.0;
	res->ClusterMemoryMaxMB 		= 0;
	res->ClusterVCoreMax   			= 0.0;
	res->TotalPendingStartTime 		= 0;
	res->QueueTrackers				= NULL;
	res->ClusterWeightMarker 		= 0;
	res->QueueIndexForLeftResource 	= 0;

	resetResourceBundleData(&(res->TotalPending)  , 0, 0.0, ratio);
	resetResourceBundleData(&(res->TotalAllocated), 0, 0.0, ratio);
	resetResourceBundleData(&(res->TotalRequest)  , 0, 0.0, ratio);
	resetResourceBundleData(&(res->TotalUsed)     , 0, 0.0, ratio);

	return res;
}

void freeMemoryCoreTrack(DynMemoryCoreRatioTrack mctrack)
{
	Assert(list_length(mctrack->QueueTrackers) == 0);
	rm_pfree(PCONTEXT, mctrack);
}

int removeQueueTrackFromMemoryCoreTrack(DynMemoryCoreRatioTrack mctrack,
											DynResourceQueueTrack   track)
{
	ListCell *cell 	   = NULL;
	ListCell *prevcell = NULL;
	foreach(cell, mctrack->QueueTrackers)
	{
		DynResourceQueueTrack trackiter = (DynResourceQueueTrack)lfirst(cell);
		if ( trackiter == track )
		{
			MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
			mctrack->QueueTrackers = list_delete_cell(mctrack->QueueTrackers,
													  cell,
													  prevcell);
			MEMORY_CONTEXT_SWITCH_BACK
			return FUNC_RETURN_OK;
		}
		prevcell = cell;
	}
	return RESQUEMGR_NO_QUE_IN_RATIO;
}
/**
 * Add one queue track of memory/core ratio into resource queue manager.
 */
int32_t addResourceQueueRatio(DynResourceQueueTrack track)
{
	if ( track->MemCoreRatio <= 0 )
		return -1;

	uint32_t ratio = track->MemCoreRatio;
	int32_t res = getResourceQueueRatioIndex(ratio);
	if ( res >= 0 ) {
		PQUEMGR->RatioReferenceCounter[res]++;
	} else {
		res = PQUEMGR->RatioCount;
		PQUEMGR->RatioReverseIndex[PQUEMGR->RatioCount] = ratio;
		setHASHTABLENode(&(PQUEMGR->RatioIndex),
						 TYPCONVERT(void *, ratio),
						 TYPCONVERT(void *, PQUEMGR->RatioCount),
						 false);
		PQUEMGR->RatioCount++;
		PQUEMGR->RatioReferenceCounter[res] = 1;
		PQUEMGR->RatioTrackers[res] = createDynMemoryCoreRatioTrack(ratio, res);
		elog(RMLOG, "Added new memory/core ratio %u, assigned index %d.",
					ratio, res);

		/* Add all resource info into the alloc/avail resource ordered indices */
		BBST newindex = NULL; /* variable for calling the following function. */
		addOrderedResourceAllocTreeIndexByRatio(ratio, &newindex);
		addOrderedResourceAvailTreeIndexByRatio(ratio, &newindex);
	}

	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	PQUEMGR->RatioTrackers[res]->QueueTrackers =
		lappend(PQUEMGR->RatioTrackers[res]->QueueTrackers, track);
	MEMORY_CONTEXT_SWITCH_BACK

	track->trackedMemCoreRatio = true;
	return res;
}

void removeResourceQueueRatio(DynResourceQueueTrack track)
{
	int res = FUNC_RETURN_OK;

	/* Ignore invalid ratio number. */
	if ( track->MemCoreRatio <= 0 )
		return;

	uint32_t ratio      = track->MemCoreRatio;
	int32_t  ratioindex = getResourceQueueRatioIndex(ratio);

	/* Ignore unkonwn ratio number. */
	if ( ratioindex < 0 ) {
		elog( WARNING, "HAWQ RM :: Cannot track resource queue %s with memory "
					   "core ratio %d MB Per CORE.",
					   track->QueueInfo->Name,
					   track->MemCoreRatio);
		return;
	}

	/* Minus ratio counter, if the counter is set to 0, delete this ratio. */
	Assert(PQUEMGR->RatioReferenceCounter[ratioindex] > 0);
	PQUEMGR->RatioReferenceCounter[ratioindex]--;

	/* Remove the reference from memory/core ratio tracker */
	res = removeQueueTrackFromMemoryCoreTrack(PQUEMGR->RatioTrackers[ratioindex],
											  track);
	if ( res != FUNC_RETURN_OK ) {
		elog( WARNING, "HAWQ RM :: Cannot find resource queue %s with memory "
					   "core ratio %d MB Per CORE in memory core ratio tracker.",
					   track->QueueInfo->Name,
					   track->MemCoreRatio);
		return;
	}

	if (PQUEMGR->RatioReferenceCounter[ratioindex] == 0) {

		/* Free the memory/core tracker instance. */
		freeMemoryCoreTrack(PQUEMGR->RatioTrackers[ratioindex]);
		PQUEMGR->RatioTrackers[ratioindex] = NULL;

		/*
		 * The top one is to be deleted, this is easy, we just adjust the counter
		 * and leave the value dirty. Because, when new ratio is added to reuse
		 * the same slots, the value is reset to zero. Check addResourceQueueRatio()
		 */
		if ( ratioindex == PQUEMGR->RatioCount - 1 ) {
			removeHASHTABLENode(&(PQUEMGR->RatioIndex), TYPCONVERT(void *, ratio));
			PQUEMGR->RatioCount--;
		}
		/*
		 * If another one is to be deleted, the idea is to copy the counters of
		 * the top one to the slots to be deleted.
		 */
		else{
			int top = PQUEMGR->RatioCount - 1;

			/* Change the tracker array */
			PQUEMGR->RatioTrackers[ratioindex] = PQUEMGR->RatioTrackers[top];

			/* Change the hash table. */
			setHASHTABLENode(&(PQUEMGR->RatioIndex),
							 TYPCONVERT(void *, PQUEMGR->RatioReverseIndex[top]),
							 TYPCONVERT(void *, ratioindex),
							 false);

			removeHASHTABLENode(&(PQUEMGR->RatioIndex),
							    TYPCONVERT(void *, ratio));

			/* Change the reverse index. */
			PQUEMGR->RatioReverseIndex[ratioindex] = PQUEMGR->RatioReverseIndex[top];
			PQUEMGR->RatioCount--;
		}

		elog(RMLOG, "HAWQ RM :: Removed ratio %d MBPCORE", ratio);
	}

	track->trackedMemCoreRatio = false;
}

void generateQueueReport( int queid, char *buff, int buffsize )
{
	DynResourceQueue 	  que 		= NULL;
	bool 			 	  exist 	= false;
	DynResourceQueueTrack quetrack 	= getQueueTrackByQueueOID(queid, &exist);

	if ( !exist ) {
		sprintf(buff, "UNKNOWN QUEUE.\n");
		return;
	}

	Assert( quetrack != NULL );
	que = quetrack->QueueInfo;

	if ( RESQUEUE_IS_PERCENT(que) )
	{
		sprintf(buff,
				"\n"
				"RESQUEUE:ID="INT64_FORMAT",Name=%s,"
				"PARENT="INT64_FORMAT","
				"LIMIT(MEM=%lf%%,CORE=%lf%%),"
				"RATIO=%d MBPCORE,"
				"INUSE(%d MB, %lf CORE),"
				"CONN=%d,"
				"INQUEUE=%d.\n",
				que->OID, que->Name,
				que->ParentOID,
				que->ClusterMemoryPer,
				que->ClusterVCorePer,
				quetrack->MemCoreRatio,
				quetrack->TotalUsed.MemoryMB,
				quetrack->TotalUsed.Core,
				quetrack->CurConnCounter,
				quetrack->QueryResRequests.NodeCount);
	}
	else {
		sprintf(buff,
				"\n"
				"RESQUEUE:ID="INT64_FORMAT",Name=%s,"
				"PARENT="INT64_FORMAT","
				"LIMIT(MEM=%d MB,CORE=%f CORE),"
				"RATIO=%d MBPCORE,"
				"INUSE(%d MB, %lf CORE),"
				"CONN=%d,"
				"INQUEUE=%d.\n",
				que->OID, que->Name,
				que->ParentOID,
				que->ClusterMemoryMB,
				que->ClusterVCore,
				quetrack->MemCoreRatio,
				quetrack->TotalUsed.MemoryMB,
				quetrack->TotalUsed.Core,
				quetrack->CurConnCounter,
				quetrack->QueryResRequests.NodeCount);
	}
}

void generateUserReport( const char   *userid,
						 int 		   useridlen,
						 char 		  *buff,
						 int 		   buffsize)
{
	UserInfo 	userinfo	= NULL;
	bool		exist		= false;

	userinfo = getUserByUserName(userid, useridlen, &exist);

	if ( !exist ) {
		sprintf(buff, "NULL USER.\n");
	}
	else {
		Assert(userinfo != NULL);
		sprintf(buff, "USER:ID=%s,QUEUEID=" INT64_FORMAT ",ISSUPERUSER=%s\n",
					  userinfo->Name,
					  userinfo->QueueOID,
					  userinfo->isSuperUser?"YES":"NO");
	}
}

/**
 * Register and check the parallel limitation.
 *
 * conntrack[in] 		: The result will be saved in this connection track.
 *
 * Return:
 * 	FUNC_RETURN_OK				: Everything is ok.
 * 	RESQUEMGR_NO_USERID			: Can not find the user.
 * 	RESQUEMGR_NO_ASSIGNEDQUEUE	: No assigned queue for current user.
 * 	RESQUEMGR_PARALLEL_FULL		: Can not accept more connection.
 * 	RESQUEMGR_INTERNAL_ERROR	: HAWQ RM internal error.
 *
 * The progress attribute is updated.
 *
 * NOTE: In order to facilitate test automation, currently all undefined users
 * 	 	 are assigned to 'default' queue.
 */
int registerConnectionByUserID( ConnectionTrack			 conntrack)
{
	int 					res 		  = FUNC_RETURN_OK;
	UserInfo 				userinfo 	  = NULL;
	DynResourceQueueTrack 	queuetrack 	  = NULL;
	bool					exist		  = false;

	Assert( conntrack != NULL );
	Assert( conntrack->Progress == CONN_PP_ESTABLISHED );

	/* Check if the user exists and save reference to corresponding user. */
	userinfo = getUserByUserName(conntrack->UserID,
							     strlen(conntrack->UserID),
							     &exist);
	if ( exist )
	{
		/* Mark the user is in use in some connections.*/
		userinfo->isInUse++;
		/* Get the queue, and check if the parallel limit is achieved. */
		queuetrack = getQueueTrackByQueueOID(userinfo->QueueOID, &exist);
	}
	else
	{
		elog(LOG, "No user %s defined for registering connection.",
				  conntrack->UserID);
		res = RESQUEMGR_NO_USERID;
		goto exit;
	}

	if ( queuetrack == NULL )
	{
		elog(LOG, "Resource manager fails to find target resource queue for user %s.",
				  conntrack->UserID);
		res = RESQUEMGR_NO_ASSIGNEDQUEUE;
		goto exit;
	}

//	/* Acquire new connection. */
//	if (queuetrack->CurConnCounter >= queuetrack->QueueInfo->ParallelCount) {
//		res = RESQUEMGR_PARALLEL_FULL;
//		elog(DEBUG5, "Queue %s is full connected with %d connections.",
//				     queuetrack->QueueInfo->Name,
//				     queuetrack->CurConnCounter);
//		goto exit;
//	}

	queuetrack->CurConnCounter++;

	conntrack->User 		= (void *)userinfo;
	conntrack->QueueTrack 	= (void *)queuetrack;
	conntrack->RegisterTime = gettime_microsec();
	conntrack->LastActTime  = conntrack->RegisterTime;

	transformConnectionTrackProgress(conntrack, CONN_PP_REGISTER_DONE);
exit:
	if ( res != FUNC_RETURN_OK )
	{
		conntrack->User 		= NULL;
		conntrack->QueueTrack	= NULL;
		transformConnectionTrackProgress(conntrack, CONN_PP_REGISTER_FAIL);
	}
	return res;
}


/**
 * Return one connection to resource queue.
 */
void returnConnectionToQueue(ConnectionTrack conntrack, bool normally)
{
	DynResourceQueueTrack track = (DynResourceQueueTrack)(conntrack->QueueTrack);
	if ( normally )
	{
		transformConnectionTrackProgress(conntrack, CONN_PP_ESTABLISHED);
	}
	track->CurConnCounter--;
	if ( track->CurConnCounter == 0 ) {
		track->isBusy = false;
		refreshMemoryCoreRatioLimits();
		refreshMemoryCoreRatioWaterMark();
	}
}

/*
 * Cancel one queued resource allocation request.
 */
void cancelResourceAllocRequest(ConnectionTrack conntrack)
{
	if ( conntrack->Progress != CONN_PP_RESOURCE_QUEUE_ALLOC_WAIT )
	{
		Assert(false);
	}
	DynResourceQueueTrack queuetrack = (DynResourceQueueTrack)(conntrack->QueueTrack);

	/* Remove from queueing list.  */
	DQUEUE_LOOP_BEGIN(&(queuetrack->QueryResRequests), iter, ConnectionTrack, track)
		if ( track == conntrack )
		{
			removeDQueueNode(&(queuetrack->QueryResRequests), iter);
			break;
		}
	DQUEUE_LOOP_END

	/* Unlock session in deadlock */
	unlockSessionResource(&(queuetrack->DLDetector), conntrack->SessionID);

	buildTimeoutResponseForQueuedRequest(conntrack, RESQUEMGR_NORESOURCE_TIMEOUT);
}

/* Acquire resource from queue. */
int acquireResourceFromResQueMgr(ConnectionTrack conntrack)
{
	int						res			  	= FUNC_RETURN_OK;

	DynResourceQueueTrack   queuetrack	  	= conntrack->QueueTrack;

	if ( queuetrack->ClusterSegNumberMax == 0 ) {
		elog(LOG, "The queue %s has no resource available to run queries.",
				  queuetrack->QueueInfo->Name);
		return RESQUEMGR_NO_RESOURCE;
	}

	/* Call quota logic to make decision of resource for current query. */
	res = computeQueryQuota(queuetrack,
							&conntrack->MaxSegCountFixed,
							&conntrack->MinSegCountFixed,
							&(conntrack->SegMemoryMB),
							&(conntrack->SegCore),
							&(conntrack->SegNum),
							&(conntrack->SegNumMin),
							conntrack->VSegLimit);

	if ( res == FUNC_RETURN_OK ) {

		int32_t Rmax  = conntrack->SegNum;
		int32_t RmaxL = conntrack->VSegLimitPerSeg * PRESPOOL->AvailNodeCount;
		int32_t Rmin  = conntrack->SegNumMin;
		elog(LOG, "HAWQ RM :: original quota min seg num:%d, max seg num:%d",
				  conntrack->SegNumMin,
				  conntrack->SegNum);

		/* Ensure quota [min,max] is between request [min,max] */
		int32_t Gmax= conntrack->MaxSegCountFixed;
		int32_t Gmin= conntrack->MinSegCountFixed;

		/* Apply upper vseg limit. */
		if ( conntrack->MaxSegCountFixed >  queuetrack->QueueInfo->VSegUpperLimit &&
			 conntrack->MinSegCountFixed <= queuetrack->QueueInfo->VSegUpperLimit )
		{
			Gmax = queuetrack->QueueInfo->VSegUpperLimit;
			elog(LOG, "Maximum vseg num is limited to %d", Gmax);
		}

		if(Gmin==1)
		{
			/* case 1 */
			conntrack->SegNumMin = min(min(Gmax,Rmin),RmaxL);
			conntrack->SegNum = min(Gmax,RmaxL);
			if(conntrack->SegNumMin > conntrack->SegNum)
			{
				return RESQUEMGR_NO_RESOURCE;
			}
		}
		else if(Gmax == Gmin)
		{
			/* case 2 */
			conntrack->SegNumMin = Gmax;
			conntrack->SegNum = Gmax;
			if(Rmax < Gmax)
			{
				return RESQUEMGR_NO_RESOURCE;
			}
		}
		else
		{
			/* case 3 */
			conntrack->SegNumMin = min(max(Gmin,Rmin),Gmax);
			conntrack->SegNum = min(max(min(RmaxL,Gmax),Gmin),Rmax);
			if(conntrack->SegNumMin > conntrack->SegNum)
			{
				return RESQUEMGR_NO_RESOURCE;
			}
		}

		elog(LOG, "HAWQ RM :: Expect (%d MB, %lf CORE) x %d ( min %d ) resource.",
				   conntrack->SegMemoryMB,
				   conntrack->SegCore,
				   conntrack->SegNum,
				   conntrack->SegNumMin);



		/* Add request to the resource queue and return. */
		res = addQueryResourceRequestToQueue(queuetrack, conntrack);
		if ( res == FUNC_RETURN_OK ) {
			transformConnectionTrackProgress(conntrack,
											 CONN_PP_RESOURCE_QUEUE_ALLOC_WAIT);
			return res;
		}
	}
	elog(LOG, "Not accepted resource acquiring request.");
	transformConnectionTrackProgress(conntrack, CONN_PP_RESOURCE_ACQUIRE_FAIL);
	return res;
}

int acquireResourceQuotaFromResQueMgr(ConnectionTrack conntrack)
{
	int 					res 		= FUNC_RETURN_OK;
	DynResourceQueueTrack   queuetrack	= conntrack->QueueTrack;
	UserInfo 				userinfo 	= NULL;
	bool					exist		= false;
	/* Check if the user exists and save reference to corresponding user. */
	userinfo = getUserByUserName(conntrack->UserID,
							     strlen(conntrack->UserID),
							     &exist);
	if ( exist ) {
		/* Get the queue, and check if the parallel limit is achieved. */
		queuetrack = getQueueTrackByQueueOID(userinfo->QueueOID, &exist);
	}
	else {
		elog(LOG, "No user %s defined for registering connection. Assign to "
				  "default queue.",
				  conntrack->UserID);
		queuetrack = PQUEMGR->DefaultTrack;
		userinfo = NULL;
	}

	if ( queuetrack == NULL ) {
		elog(LOG, "Resource manager fails to find target resource queue for user %s.",
				  conntrack->UserID);
		res = RESQUEMGR_NO_ASSIGNEDQUEUE;
		goto exit;
	}

	/* Compute query quota */
	res = computeQueryQuota(queuetrack,
							&conntrack->MaxSegCountFixed,
							&conntrack->MinSegCountFixed,
							&(conntrack->SegMemoryMB),
							&(conntrack->SegCore),
							&(conntrack->SegNum),
							&(conntrack->SegNumMin),
							conntrack->VSegLimit);

	if ( res == FUNC_RETURN_OK ) {

		int32_t Rmax = conntrack->SegNum;
		int32_t RmaxL =conntrack->VSegLimitPerSeg *	PRESPOOL->AvailNodeCount;
		int32_t Rmin = conntrack->SegNumMin;
		elog(LOG, "HAWQ RM :: original quota min seg num:%d, max seg num:%d",
					conntrack->SegNumMin,
					conntrack->SegNum);

		/* Ensure quota [min,max] is between request [min,max] */
		int32_t Gmax= conntrack->MaxSegCountFixed;
		int32_t Gmin= conntrack->MinSegCountFixed;

		/* Apply upper vseg limit. */
		if ( conntrack->MaxSegCountFixed >  queuetrack->QueueInfo->VSegUpperLimit &&
			 conntrack->MinSegCountFixed <= queuetrack->QueueInfo->VSegUpperLimit )
		{
			Gmax = queuetrack->QueueInfo->VSegUpperLimit;
			elog(LOG, "Maximum vseg num is limited to %d", Gmax);
		}

		if(Gmin==1)
		{
			/* case 1 */
			conntrack->SegNumMin = min(min(Gmax,Rmin),RmaxL);
			conntrack->SegNum = min(Gmax,RmaxL);
			if(conntrack->SegNumMin > conntrack->SegNum)
			{
				return RESQUEMGR_NO_RESOURCE;
			}
		}
		else if(Gmax == Gmin)
		{
			/* case 2 */
			conntrack->SegNumMin = Gmax;
			conntrack->SegNum = Gmax;
			if(Rmax < Gmax)
			{
				return RESQUEMGR_NO_RESOURCE;
			}
		}
		else
		{
			/* case 3 */
			conntrack->SegNumMin = min(max(Gmin,Rmin),Gmax);
			conntrack->SegNum = min(max(min(RmaxL,Gmax),Gmin),Rmax);
			if(conntrack->SegNumMin > conntrack->SegNum)
			{
				return RESQUEMGR_NO_RESOURCE;
			}
		}

		elog(LOG, "Expect (%d MB, %lf CORE) x %d ( min %d ) resource quota.",
				   conntrack->SegMemoryMB,
				   conntrack->SegCore,
				   conntrack->SegNum,
				   conntrack->SegNumMin);
	}
	else {
		elog(LOG, "Not accepted resource acquiring request.");
	}
exit:
	return res;
}

/* Resource is returned from query to resource queue. */
int returnResourceToResQueMgr(ConnectionTrack conntrack)
{
	int res = FUNC_RETURN_OK;

	if ( !conntrack->isOld ) {

		/* return resource quota to resource queue. */
		returnAllocatedResourceToLeafQueue(
				conntrack->QueueTrack,
				conntrack->SegMemoryMB * conntrack->SegNumActual,
				conntrack->SegCore     * conntrack->SegNumActual);

		/* Minus resource usage by session. */
		if ( conntrack->SessionID >= 0 ) {
			DynResourceQueueTrack quetrack = (DynResourceQueueTrack)
											 (conntrack->QueueTrack);
			minusSessionInUserResource(&(quetrack->DLDetector),
									   conntrack->SessionID,
									   conntrack->SegMemoryMB * conntrack->SegNumActual,
									   conntrack->SegCore     * conntrack->SegNumActual);
		}
	}

	((DynResourceQueueTrack)(conntrack->QueueTrack))->NumOfRunningQueries--;

	/* return allocated resource to resource pool. */
	returnResourceToResourcePool(conntrack->SegMemoryMB,
								 conntrack->SegCore,
								 conntrack->SegIOBytes,
								 conntrack->SliceSize,
								 &(conntrack->Resource),
								 conntrack->isOld);

	transformConnectionTrackProgress(conntrack, CONN_PP_REGISTER_DONE);

	/* Some resource is returned. Try to dispatch resource to queries. */
	PQUEMGR->toRunQueryDispatch = true;

	validateResourcePoolStatus(true);

	return res;
}

/* Refresh actual resource queue capacity. */
void refreshResourceQueuePercentageCapacity(void)
{
	/*
	 * Decide The actual capacity. This is necessary because there maybe some
	 * physical machines having different ratio capacity.
	 */
	uint32_t mem  = 0;
	uint32_t core = 0;

	if ( PQUEMGR->RootTrack != NULL ) {

		if ( DRMGlobalInstance->ImpType == YARN_LIBYARN ) {
			mem  = PRESPOOL->GRMTotal.MemoryMB * PQUEMGR->GRMQueueMaxCapacity;
			core = PRESPOOL->GRMTotal.Core     * PQUEMGR->GRMQueueMaxCapacity;
		}
		else if ( DRMGlobalInstance->ImpType == NONE_HAWQ2 ) {
			mem  = PRESPOOL->FTSTotal.MemoryMB;
			core = PRESPOOL->FTSTotal.Core;
		}
		else {
			Assert(false);
		}
	}
	else {
		return;
	}

	/* Logic to ensure the cluster total resource follows memory core ratio. */
	if ( PRESPOOL->MemCoreRatio > 0 && core > 0 && mem > 0 ) {
		/* Cluster has more memory resource */
		if ( mem > PRESPOOL->MemCoreRatio * core) {
			mem = core * PRESPOOL->MemCoreRatio;
		}
		/* Cluster has more core resource */
		else {
			core = trunc(mem * 1.0 / PRESPOOL->MemCoreRatio);
		}
	}
	else {
		return;
	}

	elog(DEBUG3, "HAWQ RM :: Use cluster (%d MB, %d CORE) resources as whole.",
				mem, core);

	refreshResourceQueuePercentageCapacityInternal(mem, core);

	/*
	 * After freshing resource queue capacity, it is necessary to try to dispatch
	 * resource to queries.
	 */
	PQUEMGR->toRunQueryDispatch = true;
}

void refreshMemoryCoreRatioLevelUsage(uint64_t curmicrosec)
{
	ListCell *cell = NULL;

	for( int i = 0 ; i < PQUEMGR->RatioCount ; ++i ) {

		DynMemoryCoreRatioTrack mctrack = PQUEMGR->RatioTrackers[i];

		resetResourceBundleData(&(mctrack->TotalUsed),    0, 0.0, mctrack->TotalUsed.Ratio);
		resetResourceBundleData(&(mctrack->TotalRequest), 0, 0.0, mctrack->TotalUsed.Ratio);

		foreach(cell, mctrack->QueueTrackers)
		{
			DynResourceQueueTrack track = lfirst(cell);
			addResourceBundleData(&(mctrack->TotalUsed),
								  track->TotalUsed.MemoryMB,
								  track->TotalUsed.Core);
			if (track->TotalRequest.MemoryMB > track->ClusterMemoryMaxMB)
			{
				addResourceBundleData(&(mctrack->TotalRequest),
								  	  track->ClusterMemoryMaxMB,
									  track->ClusterVCoreMax);
			}
			else
			{
				addResourceBundleData(&(mctrack->TotalRequest),
									  track->TotalRequest.MemoryMB,
									  track->TotalRequest.Core);
			}
		}

		if ( mctrack->TotalRequest.MemoryMB > mctrack->ClusterMemoryMaxMB )
		{
			mctrack->TotalRequest.MemoryMB = mctrack->ClusterMemoryMaxMB;
		}

		/*
		elog(DEBUG5, "HAWQ RM :: Memory/ratio[%d] %d MBPCORE has "
					 "(%d MB, %lf CORE) in use, (%d MB, %lf CORE) requested.",
					 i,
					 mctrack->MemCoreRatio,
					 mctrack->TotalUsed.MemoryMB,
					 mctrack->TotalUsed.Core,
					 mctrack->TotalRequest.MemoryMB,
					 mctrack->TotalRequest.Core);
		*/

		markMemoryCoreRatioWaterMark(&(PQUEMGR->RatioWaterMarks[i]),
									 curmicrosec,
									 mctrack->TotalUsed.MemoryMB,
									 mctrack->TotalUsed.Core);
	}
}

void markMemoryCoreRatioWaterMark(DQueue 		marks,
								  uint64_t 		curmicrosec,
								  int32_t 		memmb,
								  double 		core)
{
	/* Each mark instance records the maximum usage in one sencond. */
	uint64_t 					cursec   = curmicrosec / 1000000;
	DynMemoryCoreRatioWaterMark lastmark = NULL;
	int32_t						oldmarkmem = 0;
	double 						oldmarkcore = 0;

	elog(DEBUG5, "Resource water mark candidate (%d MB, %lf CORE) "UINT64_FORMAT,
			     memmb,
			     core,
			     cursec);

	if ( marks->NodeCount > 0 ) {
		DynMemoryCoreRatioWaterMark firstmark =
				(DynMemoryCoreRatioWaterMark)
				getDQueueContainerData(getDQueueContainerHead(marks));
		oldmarkmem  = firstmark->ClusterMemoryMB;
		oldmarkcore = firstmark->ClusterVCore;
		elog(DEBUG5, "Resource water mark old (%d MB, %lf CORE)",
				     oldmarkmem,
				     oldmarkcore);
	}

	if ( marks->NodeCount > 0 ) {
		lastmark = (DynMemoryCoreRatioWaterMark)
				   getDQueueContainerData(getDQueueContainerTail(marks));

		if ( lastmark->LastRecordTime == cursec ) {
			lastmark->ClusterMemoryMB = lastmark->ClusterMemoryMB > memmb ?
									    lastmark->ClusterMemoryMB :
									    memmb;
			lastmark->ClusterVCore    = lastmark->ClusterVCore > core ?
					                    lastmark->ClusterVCore :
									    core;
			/* Get the last mark cut. We will process the left marks now. */
			removeDQueueTailNode(marks);
		}
		else {
			lastmark = NULL;
		}
	}

	if ( lastmark == NULL) {
		lastmark = rm_palloc0(PCONTEXT, sizeof(DynMemoryCoreRatioWaterMarkData));
		lastmark->LastRecordTime  = cursec;
		lastmark->ClusterMemoryMB = memmb;
		lastmark->ClusterVCore    = core;
	}

	elog(DEBUG5, "Resource water mark list size %d before timeout old marks.",
			     marks->NodeCount);

	/* Check if we should remove some marks from the head. */
	while( marks->NodeCount > 0 ) {
		DynMemoryCoreRatioWaterMark firstmark =
				(DynMemoryCoreRatioWaterMark)
				getDQueueContainerData(getDQueueContainerHead(marks));
		if ( lastmark->LastRecordTime - firstmark->LastRecordTime > rm_resource_timeout ) {
			removeDQueueHeadNode(marks);
			rm_pfree(PCONTEXT, firstmark);
		}
		else {
			break;
		}
	}

	elog(DEBUG5, "Resource water mark list size %d after timeout old marks.",
			     marks->NodeCount);

	/* Check if we can skip some marks before the last one (the one we have cut). */
	while( marks->NodeCount > 0 ) {
		DynMemoryCoreRatioWaterMark last2mark =
				(DynMemoryCoreRatioWaterMark)
				getDQueueContainerData(getDQueueContainerTail(marks));
		if ( last2mark->ClusterMemoryMB <= lastmark->ClusterMemoryMB ) {
			removeDQueueTailNode(marks);
			rm_pfree(PCONTEXT, last2mark);
		}
		else {
			break;
		}
	}

	elog(DEBUG5, "Resource water mark list size %d after remove low marks.",
			     marks->NodeCount);

	/* Add the last one back to the tail. */
	insertDQueueTailNode(marks, lastmark);

	Assert(marks->NodeCount > 0);
	{
		DynMemoryCoreRatioWaterMark firstmark =
				(DynMemoryCoreRatioWaterMark)
				getDQueueContainerData(getDQueueContainerHead(marks));
		if ( firstmark->ClusterMemoryMB != oldmarkmem ) {
			elog(LOG, "Resource water mark changes from (%d MB, %lf CORE) to "
					  "(%d MB, %lf CORE)",
					  oldmarkmem,
					  oldmarkcore,
					  firstmark->ClusterMemoryMB,
					  firstmark->ClusterVCore);
		}
	}
}

/**
 * This function parses the attributes and translate into UserInfo structure's
 * attributes. This functions does not generate logs higher than WARNING, the
 * concrete error is also saved in error buffer to make the caller able to pass
 * back the message to remote process.
 */
int parseUserAttributes( List 	 	*attributes,
						 UserInfo 	 user,
						 char		*errorbuf,
						 int		 errorbufsize)
{
	int 					res 		 = FUNC_RETURN_OK;
	int						res2		 = FUNC_RETURN_OK;
	int 					attrindex 	 = -1;
	DynResourceQueueTrack 	track		 = NULL;
	Oid 					useroid 	 = InvalidOid;
	Oid						queueoid	 = InvalidOid;
	bool					exist		 = false;
	int64_t					queueoid64	 = -1;

	/* Initialize attributes. */
	user->isSuperUser 	= false;
	user->QueueOID 		= -1;

	ListCell *cell = NULL;
	foreach(cell, attributes)
	{
		KVProperty value = lfirst(cell);

		SimpStringPtr attrname  = &(value->Key);
		SimpStringPtr attrvalue = &(value->Val);

		attrindex = getUSRTBLAttributeNameIndex(attrname);

		if ( SimpleStringEmpty(attrvalue) )
		{
			if ( SimpleStringEmpty(attrvalue) )
			{
				elog(LOG, "No value for attribute [%s].", attrname->Str);
				continue;
			}
		}

		if ( attrindex == -1 )
		{
			res = RESQUEMGR_WRONG_ATTRNAME;
			snprintf(errorbuf, errorbufsize,
					 "Resource manager cannot recognize resource queue attribute "
					 "[%s]",
					 attrname->Str);
			ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
			return res;
		}

		switch(attrindex)
		{
		case USR_TBL_ATTR_OID:
			/* The oid is expected to be unique. */
			res2 = SimpleStringToOid(attrvalue, &useroid);
			getUserByUserOID(useroid, &exist);
			if ( exist )
			{
				res = RESQUEMGR_DUPLICATE_USERID;
				snprintf(errorbuf, errorbufsize, "Duplicate user oid %s", attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
			user->OID = useroid;
			break;

		case USR_TBL_ATTR_NAME:

			/* The user name must be unique. */
			getUserByUserName(attrvalue->Str, attrvalue->Len, &exist);
			if ( exist )
			{
				res = RESQUEMGR_DUPLICATE_USERID;
				snprintf(errorbuf, errorbufsize, "Duplicate user name %s", attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}

			/* Set user name value. */
			strncpy(user->Name, attrvalue->Str, attrvalue->Len);
			break;

		case USR_TBL_ATTR_TARGET_QUEUE:

			res2 = SimpleStringToOid(attrvalue, &queueoid);
			if ( res2 != FUNC_RETURN_OK )
			{
				res = RESQUEMGR_WRONG_ATTR;
				snprintf(errorbuf, errorbufsize,
						 "Wrong target resource queue oid %s.",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
			/* The target queue must exist. */
			queueoid64 = queueoid;
			track = getQueueTrackByQueueOID(queueoid64, &exist);
			if ( !exist )
			{
				res = RESQUEMGR_WRONG_TARGET_QUEUE;
				snprintf(errorbuf, errorbufsize,
						 "Can not find target resource queue %s",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
			Assert(exist && track != NULL);

			/* Set value. */
			user->QueueOID = track->QueueInfo->OID;

			break;
		case USR_TBL_ATTR_PRIORITY:
			break;
		case USR_TBL_ATTR_IS_SUPERUSER:
		{
			bool issuper = false;
			res2 = SimpleStringToBool(attrvalue,&issuper);
			if ( res2 != FUNC_RETURN_OK )
			{
				res = RESQUEMGR_WRONG_ATTR;
				snprintf(errorbuf, errorbufsize,
						 "Wrong user issuper setting '%s'",
						 attrvalue->Str);
				ELOG_ERRBUF_MESSAGE(WARNING, errorbuf)
				return res;
			}
			user->isSuperUser = issuper ? 1 : 0;
			break;
		}
		default:
			Assert(0);
		}
	}

	return FUNC_RETURN_OK;
}

int checkUserAttributes( UserInfo user, char *errorbuf, int errorbufsize)
{
	if ( user->QueueOID == -1 ) {
		user->QueueOID = DEFAULTRESQUEUE_OID;
	}
	return FUNC_RETURN_OK;
}

/* Create one user */
int createUser(UserInfo userinfo, char *errorbuf, int errorbufsize)
{
	int	res	= FUNC_RETURN_OK;

	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	PQUEMGR->Users = lappend(PQUEMGR->Users, userinfo);
	MEMORY_CONTEXT_SWITCH_BACK

	if ( userinfo->OID > InvalidOid )
	{
		setUserIndexedByUserOID(userinfo);
	}
	setUserIndexedByUserName(userinfo);

	return res;
}

void setUserIndexedByUserOID(UserInfo userinfo)
{
	SimpArray key;
	setSimpleArrayRef(&key, (void *)&(userinfo->OID), sizeof(int64_t));
	setHASHTABLENode(&(PQUEMGR->UsersIDIndex), &key, userinfo, false);
}

void setUserIndexedByUserName(UserInfo userinfo)
{
	SimpString key;
	setSimpleStringRefNoLen(&key, userinfo->Name);
	setHASHTABLENode(&(PQUEMGR->UsersNameIndex),
					 (void *)(&(key)),
					 userinfo,
					 false);
}

UserInfo getUserByUserName( const char *userid, int useridlen, bool *exist)
{
	PAIR						pair				= NULL;
	SimpString					key;

	/* Check if the user exists. */
	setSimpleStringRef(&key, (char *)userid, useridlen);
	pair = getHASHTABLENode(&(PQUEMGR->UsersNameIndex), &key);
	if( pair == NULL ) {
		*exist = false;
		return NULL;
	}

	*exist = true;
	return (UserInfo)(pair->Value);
}

UserInfo getUserByUserOID ( int64_t useroid, bool *exist)
{
	PAIR		pair	= NULL;
	SimpArray 	key;
	setSimpleArrayRef(&key, (void *)&useroid, sizeof(int64_t));
	pair = getHASHTABLENode(&(PQUEMGR->UsersIDIndex), &key);
	if ( pair == NULL ) {
		*exist = false;
		return NULL;
	}
	*exist = true;
	return (UserInfo)(pair->Value);
}

int dropUser(int64_t useroid, char* name)
{
	Assert(useroid != InvalidOid && name != NULL);

	ListCell *cell 	   = NULL;
	ListCell *prevcell = NULL;
	foreach(cell, PQUEMGR->Users)
	{
		UserInfo user = lfirst(cell);
		if(user->OID == useroid)
		{
			MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
			PQUEMGR->Users = list_delete_cell(PQUEMGR->Users, cell, prevcell);
			MEMORY_CONTEXT_SWITCH_BACK
			elog(LOG, "Resource manager finds user oid "INT64_FORMAT" and delete.",
					  useroid);

			SimpArray key1;
			setSimpleArrayRef(&key1, (void *)&useroid, sizeof(int64_t));
			int res = removeHASHTABLENode(&(PQUEMGR->UsersIDIndex), &key1);
			elog(DEBUG3, "Resource manager removed node from UsersIDIndex returns %d",
						 res);
			Assert(res == FUNC_RETURN_OK);

			SimpString key2;
			setSimpleStringRef(&key2, name, strlen(name));
			res = removeHASHTABLENode(&(PQUEMGR->UsersNameIndex), &key2);
			elog(DEBUG3, "Resource manager removed node from UsersNameIndex returns %d",
						 res);
			Assert(res == FUNC_RETURN_OK);
			return FUNC_RETURN_OK;
		}
		prevcell = cell;
	}

	return RESQUEMGR_NO_USERID;
}

void dispatchResourceToQueries(void)
{
	bool 		hasresourceallocated = false;
	bool 		hasrequest 		  	 = false;
	/*
	 *--------------------------------------------------------------------------
	 * STEP 1. Re-balance resource among different mem/core ratio trackers. After
	 * 		   this step, each mem/core ratio trackers process their own queues
	 * 		   only.
	 *
	 * 		   IN CURRENT VERSION. This is not implemented.
	 *--------------------------------------------------------------------------
	 */

	/*
	 * STEP 2. Decide how much resource are dispatched to each segment.
	 */
	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i )
	{
		DQueueData  toallocqueues;
		initializeDQueue(&toallocqueues, PCONTEXT);
		DynMemoryCoreRatioTrack mctrack = PQUEMGR->RatioTrackers[i];

		/* Ignore the memory/core ratio 1) not in use. 2) no resource allocated. */
		if ( (mctrack->ClusterMemoryMaxMB == 0 || mctrack->ClusterVCoreMax == 0) ||
			 (mctrack->TotalAllocated.MemoryMB == 0 && mctrack->TotalAllocated.Core == 0) )
		{
			continue;
		}

		uint32_t	allmemory 		  = mctrack->TotalAllocated.MemoryMB;
		uint32_t	availmemory 	  = mctrack->TotalAllocated.MemoryMB;
		double		availcore		  = mctrack->TotalAllocated.Core;
		uint32_t	totalmemoryweight = 0;

		/*
		 * Count out over used resource queues. They will pause allocating resource
		 * until the water mark is lower than expected weight.
		 */
		ListCell *cell = NULL;
		foreach(cell, mctrack->QueueTrackers)
		{
			DynResourceQueueTrack track = (DynResourceQueueTrack)lfirst(cell);

			/* Reset queue resource expect status. */
			track->expectMoreResource = false;

			/* Ignore the queues not in use. */
			if ( !track->isBusy )
			{
				continue;
			}

			double expweight  = 1.0 * track->QueueInfo->ClusterMemoryMB /
							    	mctrack->ClusterMemory;
			double actweight  = allmemory == 0 ?
									0 :
									(1.0 * track->TotalUsed.MemoryMB / allmemory);

			/* If the queue is overusing resource, keep it. */
			if ( actweight > expweight )
			{
				resetResourceBundleData(&(track->TotalAllocated),
										track->TotalUsed.MemoryMB,
										track->TotalUsed.Core,
										track->TotalAllocated.Ratio);
				track->pauseAllocation = true;
				availmemory -= track->TotalUsed.MemoryMB;
				availcore   -= track->TotalUsed.Core;

				elog(DEBUG3, "Resource queue %s over uses resource with weight "
							 "%lf, expect weight %lf. Currently total used "
							 "(%d MB, %lf CORE). Allocation to queries is paused.",
							 track->QueueInfo->Name,
							 actweight,
							 expweight,
							 track->TotalUsed.MemoryMB,
							 track->TotalUsed.Core);
			}
			else
			{
				insertDQueueTailNode(&toallocqueues, track);
				track->pauseAllocation = false;

				totalmemoryweight += track->QueueInfo->ClusterMemoryMB;

				elog(DEBUG3, "Resource queue %s uses resource with weight "
							 "%lf, expect weight %lf. Currently total used "
							 "(%d MB, %lf CORE). To assign more resource.",
							 track->QueueInfo->Name,
							 actweight,
							 expweight,
							 track->TotalUsed.MemoryMB,
							 track->TotalUsed.Core);
			}
		}

		/* Assign resource to not over used resource queues. */
		elog(DEBUG3, "Reassignable resource is (%d MB, %lf CORE)",
					 availmemory,
					 availcore);

		/*
		 * Handle all the other queues to assign resource to queues. Remaining
		 * resource is dispatched to the chosen queues based on their resource
		 * weight.
		 */
		uint32_t leftmemory2 = availmemory;
		DQUEUE_LOOP_BEGIN(&toallocqueues, iter, DynResourceQueueTrack, track)
			double expweight  = 1.0 * track->QueueInfo->ClusterMemoryMB /
							    totalmemoryweight;

			uint32_t potentialmemuse =
				track->TotalUsed.MemoryMB + track->TotalRequest.MemoryMB >
					track->ClusterMemoryMaxMB ?
				track->ClusterMemoryMaxMB :
				track->TotalUsed.MemoryMB + track->TotalRequest.MemoryMB;

			double actweight2 = 1.0 * potentialmemuse  / availmemory;

			/*
			 * CASE 1. The queue acquire only a little resource that does not
			 * 		   exceed the target weight in this memory core ratio. We
			 * 		   exactly allocate the resource it wants.
			 */
			if ( actweight2 < expweight )
			{
				resetResourceBundleData(&(track->TotalAllocated),
										potentialmemuse,
										1.0 * potentialmemuse / track->MemCoreRatio,
										track->TotalAllocated.Ratio);
				leftmemory2 -= potentialmemuse;
				Assert(leftmemory2 >= 0);
				elog(DEBUG3, "Resource manager fully satisfies to resource queue "
							 "%s with (%d MB, %lf CORE) allocated.",
							 track->QueueInfo->Name,
							 track->TotalAllocated.MemoryMB,
							 track->TotalAllocated.Core);
			}

			/*
			 * CASE 2. The queue can only get partial requested resource.
			 */
			else
			{
				uint32_t allocmemory = trunc(expweight * availmemory);
				double   alloccore   = 1.0 * availmemory / track->MemCoreRatio;

				resetResourceBundleData(&(track->TotalAllocated),
										allocmemory,
										alloccore,
										track->TotalAllocated.Ratio);
				/* Mark that the queue needs more resource if possible. */
				track->expectMoreResource = true;

				leftmemory2 -= allocmemory;
				Assert(leftmemory2 >= 0);
				elog(DEBUG3, "Resource manager partially satisfies to resource "
							 "queue %s with (%d MB, %lf CORE) allocated.",
							 track->QueueInfo->Name,
							 track->TotalAllocated.MemoryMB,
							 track->TotalAllocated.Core);
			}

			elog(DEBUG3, "Resource manager allocates resource (%d MB, %lf CORE) "
						 "in queue %s.",
						 track->TotalAllocated.MemoryMB,
						 track->TotalAllocated.Core,
						 track->QueueInfo->Name);

			Assert( track->TotalAllocated.MemoryMB >= track->TotalUsed.MemoryMB &&
					track->TotalAllocated.Core     >= track->TotalUsed.Core) ;
		DQUEUE_LOOP_END

		/*
		 * Decide left resource. The resource is assigned to one in-use queue
		 * expecting more resource in a round-robin way. Add resource to as few
		 * queues as possible.
		 */
		if ( list_length(mctrack->QueueTrackers) > 0 && leftmemory2 > 0 )
		{
			/* In case the count of queues is less than before. */
			if ( mctrack->QueueIndexForLeftResource >= list_length(mctrack->QueueTrackers) )
			{
				mctrack->QueueIndexForLeftResource = 0;
			}

			ListCell *p = list_nth_cell(mctrack->QueueTrackers,
										mctrack->QueueIndexForLeftResource);
			DynResourceQueueTrack q = NULL;
			for ( int cq = 0 ; cq < list_length(mctrack->QueueTrackers) ; ++cq )
			{
				DynResourceQueueTrack tmpq = lfirst(p);
				if ( tmpq->expectMoreResource )
				{
					q = tmpq;
					if ( leftmemory2 + q->TotalAllocated.MemoryMB <= q->ClusterMemoryMaxMB)
					{
						elog(DEBUG3, "Resource manager allocates resource (%d MB, %lf CORE) "
									 "in queue %s.",
									 leftmemory2,
									 1.0 * leftmemory2 / q->MemCoreRatio,
									 q->QueueInfo->Name);

						addResourceBundleData(&(q->TotalAllocated),
										  	  leftmemory2,
											  1.0 * leftmemory2 / q->MemCoreRatio);
						leftmemory2 = 0;
						break;
					}
					else {
						uint32_t memorydelta = q->ClusterMemoryMaxMB - q->TotalAllocated.MemoryMB;

						elog(DEBUG3, "Resource manager allocates resource (%d MB, %lf CORE) "
									 "in queue %s.",
									 memorydelta,
									 1.0 * memorydelta / q->MemCoreRatio,
									 q->QueueInfo->Name);

						addResourceBundleData(&(q->TotalAllocated),
											  memorydelta,
											  1.0 * memorydelta / q->MemCoreRatio);
						leftmemory2 -= memorydelta;
					}
					break;
				}

				/* Try next queue in next iteration. */
				p = lnext(p);
				if ( p == NULL )
				{
					mctrack->QueueIndexForLeftResource = 0;
					p = list_head(mctrack->QueueTrackers);
				}
				else
				{
					mctrack->QueueIndexForLeftResource++;
				}
			}

			mctrack->QueueIndexForLeftResource++;
		}

		/*
		 * Dispatch resource to queries. We firstly handle the queues having
		 * resource fragment problem. Then the left queues.
		 */
		for ( int i = 0 ; i < toallocqueues.NodeCount ; ++i )
		{
			DynResourceQueueTrack track = (DynResourceQueueTrack)
										  (removeDQueueHeadNode(&toallocqueues));
			if ( !track->troubledByFragment )
			{
				insertDQueueTailNode(&toallocqueues, track);
				continue;
			}
			int oldreqnum = track->QueryResRequests.NodeCount;
			hasrequest = oldreqnum > 0;
			dispatchResourceToQueriesInOneQueue(track);
			int newreqnum = track->QueryResRequests.NodeCount;
			if ( newreqnum != oldreqnum )
			{
				hasresourceallocated = true;
			}
		}

		while( toallocqueues.NodeCount > 0 )
		{
			DynResourceQueueTrack track = (DynResourceQueueTrack)
										  (removeDQueueHeadNode(&toallocqueues));
			int oldreqnum = track->QueryResRequests.NodeCount;
			hasrequest = oldreqnum > 0;
			dispatchResourceToQueriesInOneQueue(track);
			int newreqnum = track->QueryResRequests.NodeCount;
			if ( newreqnum != oldreqnum )
			{
				hasresourceallocated = true;
			}
		}
		Assert(toallocqueues.NodeCount == 0);
		cleanDQueue(&toallocqueues);

	}

	PQUEMGR->toRunQueryDispatch = !hasrequest || hasresourceallocated;
	if ( !PQUEMGR->toRunQueryDispatch )
	{
		elog(DEBUG3, "Resource manager pauses allocating resource to query because of "
				     "lack of resource.");
	}
}

/*----------------------------------------------------------------------------*/
/*                    RESOURCE QUEUE MANAGER INTERNAL APIs                    */
/*----------------------------------------------------------------------------*/

/**
 * Create new resource queue tracker instance for one resource queue.
 */
DynResourceQueueTrack createDynResourceQueueTrack(DynResourceQueue queue)
{
	DynResourceQueueTrack newtrack =
		(DynResourceQueueTrack)rm_palloc0(PCONTEXT,
										  sizeof(DynResourceQueueTrackData));

	initializeDQueue(&(newtrack->QueryResRequests), PCONTEXT);

	newtrack->QueueInfo   		  = queue;
	newtrack->ParentTrack 		  = NULL;
	newtrack->ChildrenTracks 	  = NULL;
	newtrack->CurConnCounter 	  = 0;
	newtrack->RatioIndex          = -1;
	newtrack->ClusterSegNumber    = 0;
	newtrack->ClusterSegNumberMax = 0;
	newtrack->ClusterMemoryMaxMB  = 0;
	newtrack->ClusterVCoreMax     = 0;
	newtrack->ClusterMemoryActPer = 0;
	newtrack->ClusterMemoryMaxPer = 0;
	newtrack->ClusterVCoreActPer  = 0;
	newtrack->ClusterVCoreMaxPer  = 0;
	newtrack->trackedMemCoreRatio = false;
	newtrack->isBusy			  = false;
	newtrack->pauseAllocation	  = false;
	newtrack->troubledByFragment  = false;
	newtrack->NumOfRunningQueries = 0;

	resetResourceBundleData(&(newtrack->TotalAllocated), 0, 0.0, 0);
	resetResourceBundleData(&(newtrack->TotalRequest)  , 0, 0.0, 0);
	resetResourceBundleData(&(newtrack->TotalUsed)     , 0, 0.0, 0);

	initializeResqueueDeadLockDetector(&(newtrack->DLDetector), newtrack);
	return newtrack;
}

/**
 * Free one resource queue tracker instance, expect that this tracker has no
 * active connection information saved, the connection with the other tracker
 * instance has be cut.
 */
void freeDynResourceQueueTrack(DynResourceQueueTrack track)
{
	Assert( list_length(track->ChildrenTracks) == 0 );
	Assert( track->CurConnCounter == 0 );
	Assert( track->QueryResRequests.NodeCount == 0 );
	cleanDQueue(&(track->QueryResRequests));
	rm_pfree(PCONTEXT, track);
}

int getRSQTBLAttributeNameIndex(SimpStringPtr attrname)
{
	for ( int i = 0 ; i < RSQ_TBL_ATTR_COUNT ; ++i ) {
		if ( SimpleStringComp(attrname, RSQTBLAttrNames[i]) == 0 ) {
			return i;
		}
	}
	return -1;
}

int getRSQDDLAttributeNameIndex(SimpStringPtr attrname)
{
	for ( int i = 0 ; i < RSQ_DDL_ATTR_COUNT ; ++i ) {
		if ( SimpleStringComp(attrname, RSQDDLAttrNames[i]) == 0 ) {
			return i;
		}
	}
	return -1;
}

int getUSRTBLAttributeNameIndex(SimpStringPtr attrname)
{
	for ( int i = 0 ; i < USR_TBL_ATTR_COUNT ; ++i ) {
		if ( SimpleStringComp(attrname, USRTBLAttrNames[i]) == 0 ) {
			return i;
		}
	}
	return -1;
}

const char *getUSRTBLAttributeName(int attrindex)
{
	Assert( attrindex >= 0 && attrindex < USR_TBL_ATTR_COUNT );
	return USRTBLAttrNames[attrindex];
}

void resetResourceBundleData(ResourceBundle detail,
							 uint32_t mem,
							 double core,
							 uint32_t ratio)
{
	detail->MemoryMB = mem;
	detail->Core     = core;
	detail->Ratio	 = ratio;
}

void addResourceBundleData(ResourceBundle detail, int32_t mem, double core)
{
	detail->MemoryMB += mem;
	detail->Core += core;
}

void minusResourceBundleData(ResourceBundle detail, int32_t mem, double core)
{
	detail->MemoryMB -= mem;
	detail->Core -= core;
}

void resetResourceBundleDataByBundle(ResourceBundle detail, ResourceBundle source)
{
	resetResourceBundleData(detail, source->MemoryMB, source->Core, source->Ratio);
}

void addResourceBundleDataByBundle(ResourceBundle detail, ResourceBundle source)
{
	addResourceBundleData(detail, source->MemoryMB, source->Core);
}

void minusResourceBundleDataByBundle(ResourceBundle detail, ResourceBundle source)
{
	minusResourceBundleData(detail, source->MemoryMB, source->Core);
}

/**
 * Compute the query quota.
 */
int computeQueryQuota( DynResourceQueueTrack	 track,
		   	   	   	   int32_t			 		*max_segcountfix,
		   	   	   	   int32_t			 		*min_segcountfix,
		   	   	   	   int32_t		       		*segmemmb,
					   double		       		*segcore,
					   int32_t		       		*segnum,
					   int32_t					*segnummin,
					   int32_t					 segnumlimit)
{
	int			res				= FUNC_RETURN_OK;
	int			policy			= 0;

	Assert( track != NULL );

	policy = track->QueueInfo->AllocatePolicy;
	Assert( policy >= 0 && policy < RSQ_ALLOCATION_POLICY_COUNT );

	/* Get one segment resource quota. */
	*segmemmb = track->QueueInfo->SegResourceQuotaMemoryMB;
	*segcore  = track->QueueInfo->SegResourceQuotaVCore;

	/* Decide segment number and minimum runnable segment number. */

	if (*min_segcountfix > segnumlimit)
	{
		res = RESQUEMGR_TOO_MANY_FIXED_SEGNUM;
		elog(LOG, " Expect too many virtual segments %d, can not be more "
		"than %d",
		*min_segcountfix,
		segnumlimit);
		return res;
	}
	if(*max_segcountfix > segnumlimit)
	{
		*max_segcountfix = segnumlimit;
	}

	/* Compute total resource quota. */
	res = AllocationPolicy[policy] (track, segnum, segnummin, segnumlimit);

	if ( *segnum < *min_segcountfix )
	{
		res = RESQUEMGR_TOO_MANY_FIXED_SEGNUM;
		elog(LOG, " Expect too many virtual segments %d, can not be more "
		"than %d",
		*min_segcountfix,
		*segnum);
		return res;

	}

	/* Always respect the expected minimum vseg num. */
	*segnummin = *min_segcountfix;

	elog(DEBUG3, "Expect cluster resource (%d MB, %lf CORE) x %d "
				 "minimum runnable %d segment(s).",
			     *segmemmb,
			     *segcore,
				 *segnum,
				 *segnummin);

	return FUNC_RETURN_OK;
}

/* Implementation of homogeneous resource allocation. */
int computeQueryQuota_EVEN(DynResourceQueueTrack	track,
						   int32_t			   	   *segnum,
						   int32_t			   	   *segnummin,
						   int32_t					segnumlimit)
{
	DynResourceQueue queue = track->QueueInfo;

	/* Decide one connection should have how many segments reserved. */
	int reservsegnum = trunc(track->ClusterSegNumber / queue->ParallelCount);
	reservsegnum = reservsegnum <= 0 ? 1 : reservsegnum;

	*segnum = track->ClusterSegNumberMax;
	*segnum = segnumlimit < *segnum ? segnumlimit : *segnum;

	*segnummin = reservsegnum;
	*segnummin = *segnummin > *segnum ? *segnum : *segnummin;

	Assert( *segnummin > 0 && *segnummin <= *segnum );
	return FUNC_RETURN_OK;
}

int computeQueryQuota_FIFO(DynResourceQueueTrack	 track,
						   int32_t			   		*segnum,
						   int32_t			   		*segnummin,
						   int32_t					 segnumlimit)
{
	DynResourceQueue queue = track->QueueInfo;

	/* Decide one connection should have how many segments reserved. */
	int reservsegnum = trunc(track->ClusterSegNumber / queue->ParallelCount);
	reservsegnum = reservsegnum <= 0 ? 1 : reservsegnum;

	/*
	 * FIFO allocation policy does not guarantee the concurrency specified in
	 * active_statements. Always give as more resource as possible.
	 */
	*segnum    = track->ClusterSegNumberMax;
	*segnummin = track->ClusterSegNumber;
	*segnum    = segnumlimit < *segnum ?
				 segnumlimit :
				 *segnum;

	Assert( *segnummin > 0 && *segnummin <= *segnum );
	return FUNC_RETURN_OK;
}

int addQueryResourceRequestToQueue(DynResourceQueueTrack queuetrack,
								   ConnectionTrack		 conntrack)
{
	insertDQueueTailNode(&(queuetrack->QueryResRequests), conntrack);

	/* add resource request counter. */
	addResourceBundleData(&(queuetrack->TotalRequest),
						  conntrack->SegMemoryMB * conntrack->SegNum,
						  conntrack->SegCore * conntrack->SegNum);

	/*
	 * Set session tracker and make its corresponding session in-use resource
	 * locked.
	 */
	createAndLockSessionResource(&(queuetrack->DLDetector), conntrack->SessionID);

	if ( queuetrack->DLDetector.LockedTotalMemoryMB > 0 )
	{
		PQUEMGR->ForcedReturnGRMContainerCount = 0;
		elog(LOG, "Locking resource and stop forced GRM container breathe out.");
	}

	/*
	 * If this causes the queue to be busy, refresh the limits and weights of
	 * each memory/core ratio tracker.
	 */
	if ( !queuetrack->isBusy )
	{
		queuetrack->isBusy = true;
		refreshMemoryCoreRatioLimits();
		refreshMemoryCoreRatioWaterMark();
	}
	PQUEMGR->toRunQueryDispatch = true;
	return FUNC_RETURN_OK;
}

/*
 * Update the overall resource queue percentage capacity.
 */
void refreshResourceQueuePercentageCapacityInternal(uint32_t clustermemmb,
													uint32_t clustercore)
{
	/*
	 * STEP 1. Decide the limit ranges of memory and core, decide the memory/core
	 * 		   ratio.
	 */
	ListCell *cell = NULL;
	foreach(cell, PQUEMGR->Queues)
	{
		DynResourceQueueTrack track = lfirst(cell);

		if ( RESQUEUE_IS_PERCENT(track->QueueInfo) && RESQUEUE_IS_LEAF(track->QueueInfo) )
		{

			track->ClusterMemoryActPer = track->QueueInfo->ClusterMemoryPer;
			track->ClusterVCoreActPer  = track->QueueInfo->ClusterVCorePer;
			DynResourceQueueTrack ptrack = track->ParentTrack;
			while( ptrack != NULL && RESQUEUE_IS_PERCENT(ptrack->QueueInfo))
			{
				track->ClusterMemoryActPer *= (ptrack->QueueInfo->ClusterMemoryPer / 100);
				track->ClusterVCoreActPer  *= (ptrack->QueueInfo->ClusterVCorePer / 100);
				ptrack = ptrack->ParentTrack;
			}

			track->ClusterMemoryMaxPer = track->ClusterMemoryActPer *
										 track->QueueInfo->ResourceUpperFactor;
			track->ClusterMemoryMaxPer = track->ClusterMemoryMaxPer > 100 ?
										 100.0 :
										 track->ClusterMemoryMaxPer;
			track->ClusterVCoreMaxPer = track->ClusterVCoreActPer *
										track->QueueInfo->ResourceUpperFactor;
			track->ClusterVCoreMaxPer = track->ClusterVCoreMaxPer > 100 ?
										100.0 :
										track->ClusterVCoreMaxPer;

			uint32_t tmpratio = 0;
			if ( ptrack == NULL &&
				 track->ClusterMemoryActPer == track->ClusterVCoreActPer )
			{
				tmpratio = clustermemmb / clustercore;
				track->QueueInfo->ClusterMemoryMB =
					1.0 * clustermemmb * track->ClusterMemoryActPer / 100;
				track->QueueInfo->ClusterVCore    =
					1.0 * clustercore * track->ClusterVCoreActPer / 100;

				track->ClusterMemoryMaxMB =
					1.0 * clustermemmb * track->ClusterMemoryMaxPer / 100;
				track->ClusterVCoreMax =
					1.0 * clustercore * track->ClusterVCoreMaxPer / 100;
			}
			else
			{
				/*
				 * In case the path from root to this queue contains only queues
				 * expressed by percentages. We should use the cluster capacity
				 * to calculate the capacity.
				 *
				 * Otherwise, choose the first ancestor queue that is expressed
				 * by exact memory size and core number.
				 */
				int32_t memmb = ptrack == NULL ?
								clustermemmb :
								ptrack->QueueInfo->ClusterMemoryMB;
				double core = ptrack == NULL ?
							  clustercore :
							  ptrack->QueueInfo->ClusterVCore;

				track->QueueInfo->ClusterMemoryMB =
					1.0 * memmb * track->ClusterMemoryActPer / 100;
				track->QueueInfo->ClusterVCore    =
					1.0 * core * track->ClusterVCoreActPer / 100;

				track->ClusterMemoryMaxMB =
					1.0 * memmb * track->ClusterMemoryMaxPer / 100;
				track->ClusterVCoreMax =
					1.0 * core * track->ClusterVCoreMaxPer / 100;

				/* Decide and update ratio. */
				tmpratio = trunc(track->QueueInfo->ClusterMemoryMB /
								 track->QueueInfo->ClusterVCore);
			}

			if ( tmpratio != track->MemCoreRatio && track->trackedMemCoreRatio )
			{
				removeResourceQueueRatio(track);
			}

			if ( !track->trackedMemCoreRatio )
			{
				track->MemCoreRatio = tmpratio;
				addResourceQueueRatio(track);
			}
		}
	}

	/*
	 * STEP 2. Decide the maximum limit of memory and core of each leaf queue.
	 * 		   That means for each leaf queue, its maximum memory limit adding
	 * 		   all other leaf queues' minimum limits can not exceed cluster
	 * 		   capacity. Maximum core limit follows the same logic.
	 */
	cell = NULL;
	foreach(cell, PQUEMGR->Queues)
	{
		DynResourceQueueTrack track = lfirst(cell);
		if ( !(RESQUEUE_IS_LEAF(track->QueueInfo)) )
		{
			continue;
		}

		/* Follows the memory/core ratio to adjust the maximum limits. */
		if ( track->ClusterMemoryMaxMB / track->ClusterVCoreMax > track->MemCoreRatio )
		{
			track->ClusterMemoryMaxMB = track->ClusterVCoreMax * track->MemCoreRatio;
		}
		else
		{
			track->ClusterVCoreMax = track->ClusterMemoryMaxMB / track->MemCoreRatio;
		}

		/* Decide cluster segment resource quota. */
		track->QueueInfo->SegResourceQuotaMemoryMB =
				track->QueueInfo->SegResourceQuotaMemoryMB == -1 ?
				track->QueueInfo->SegResourceQuotaVCore * track->MemCoreRatio :
				track->QueueInfo->SegResourceQuotaMemoryMB;
		track->QueueInfo->SegResourceQuotaVCore =
				track->QueueInfo->SegResourceQuotaVCore == -1 ?
				1.0 * track->QueueInfo->SegResourceQuotaMemoryMB / track->MemCoreRatio :
				track->QueueInfo->SegResourceQuotaVCore;

		/* Decide the cluster segment number quota. */
		track->ClusterSegNumber = trunc(track->QueueInfo->ClusterMemoryMB /
										track->QueueInfo->SegResourceQuotaMemoryMB);

		track->ClusterSegNumberMax = trunc(track->ClusterMemoryMaxMB /
										   track->QueueInfo->SegResourceQuotaMemoryMB);

		Assert( track->ClusterSegNumber <= track->ClusterSegNumberMax );

		elog(DEBUG3, "Resource manager refreshed resource queue capacity : %s "
				  	 "(%d MB, %lf CORE) x %d. MAX %d. FACTOR:%lf",
					 track->QueueInfo->Name,
					 track->QueueInfo->SegResourceQuotaMemoryMB,
					 track->QueueInfo->SegResourceQuotaVCore,
					 track->ClusterSegNumber,
					 track->ClusterSegNumberMax,
					 track->QueueInfo->ResourceUpperFactor);
	}
}

void refreshMemoryCoreRatioLimits(void)
{
	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i ) {

		PQUEMGR->RatioTrackers[i]->ClusterMemoryMaxMB = 0;
		PQUEMGR->RatioTrackers[i]->ClusterVCoreMax    = 0;
		PQUEMGR->RatioTrackers[i]->ClusterMemory	  = 0;
		PQUEMGR->RatioTrackers[i]->ClusterVCore		  = 0;

		ListCell *cell = NULL;
		foreach(cell, PQUEMGR->RatioTrackers[i]->QueueTrackers)
		{
			DynResourceQueueTrack track = lfirst(cell);

			/* We calculate only the queues having connections, which means
			 * potential resource request. */
			if ( !track->isBusy )
			{
				continue;
			}

			PQUEMGR->RatioTrackers[i]->ClusterMemory += track->QueueInfo->ClusterMemoryMB;
			PQUEMGR->RatioTrackers[i]->ClusterVCore  += track->QueueInfo->ClusterVCore;
			PQUEMGR->RatioTrackers[i]->ClusterMemoryMaxMB += track->ClusterMemoryMaxMB;
			PQUEMGR->RatioTrackers[i]->ClusterVCoreMax    += track->ClusterVCoreMax;
		}

		elog(DEBUG3, "Limit of memory/core ratio[%d] %d MBPCORE "
					 "is (%d MB, %lf CORE) maximum (%d MB, %lf CORE).",
					 i,
					 PQUEMGR->RatioTrackers[i]->MemCoreRatio,
					 PQUEMGR->RatioTrackers[i]->ClusterMemory,
					 PQUEMGR->RatioTrackers[i]->ClusterVCore,
					 PQUEMGR->RatioTrackers[i]->ClusterMemoryMaxMB,
					 PQUEMGR->RatioTrackers[i]->ClusterVCoreMax);
	}
}

/* TODO: Not useful yet. */
void refreshMemoryCoreRatioWaterMark(void)
{
	double totalweightmem = 0;
	double totalweightcore = 0;
	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i ) {
		totalweightmem  += PQUEMGR->RatioTrackers[i]->ClusterMemory;
		totalweightcore += PQUEMGR->RatioTrackers[i]->ClusterVCore;
	}

	double overcommitmem  = 1;
	double overcommitcore = 1;
	double overcommit     = 1;
	if ( DRMGlobalInstance->ImpType == YARN_LIBYARN ) {
		overcommitmem  = totalweightmem  / PRESPOOL->GRMTotal.MemoryMB;
		overcommitcore = totalweightcore / PRESPOOL->GRMTotal.Core;
	}
	else if ( DRMGlobalInstance->ImpType == NONE_HAWQ2 ) {
		overcommitmem  = totalweightmem  / PRESPOOL->FTSTotal.MemoryMB;
		overcommitcore = totalweightcore / PRESPOOL->FTSTotal.Core;
	}
	else {
		Assert(false);
	}

	overcommit = overcommitmem > overcommitcore ? overcommitmem : overcommitcore;
	overcommit = overcommit > 1 ? overcommit : 1;
	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i ) {
		PQUEMGR->RatioTrackers[i]->ClusterWeightMarker =
			PQUEMGR->RatioTrackers[i]->ClusterMemoryMaxMB / overcommit;
		elog(DEBUG5, "HAWQ RM :: Weight balance marker of memory/core ratio "
					 "[%d] %d MBPCORE is %lf MB with overcommit %lf",
					 i,
					 PQUEMGR->RatioTrackers[i]->MemCoreRatio,
					 PQUEMGR->RatioTrackers[i]->ClusterWeightMarker,
					 overcommit);
	}
}

void dispatchResourceToQueriesInOneQueue(DynResourceQueueTrack track)
{
	int			policy			= 0;
	Assert( track != NULL );

	if ( track->QueryResRequests.NodeCount > 0 )
	{
		ConnectionTrack topwaiter = getDQueueHeadNodeData(&(track->QueryResRequests));
		if ( topwaiter->HeadQueueTime == 0 )
		{
			topwaiter->HeadQueueTime = gettime_microsec();
			elog(DEBUG3, "Set timestamp of waiting at head of queue.");
		}
	}

	policy = track->QueueInfo->AllocatePolicy;
	Assert( policy >= 0 && policy < RSQ_ALLOCATION_POLICY_COUNT );
	DispatchPolicy[policy] (track);

	/* Check if the queue has resource fragment problem. */
	track->troubledByFragment = false;
	if ( track->QueryResRequests.NodeCount > 0 )
	{
		ConnectionTrack topwaiter = getDQueueHeadNodeData(&(track->QueryResRequests));
		track->troubledByFragment = topwaiter->troubledByFragment;
	}
}

int addNewResourceToResourceManagerByBundle(ResourceBundle bundle)
{
	return addNewResourceToResourceManager(bundle->MemoryMB, bundle->Core);
}

int addNewResourceToResourceManager(int32_t memorymb, double core)
{
	if ( memorymb == 0 && core == 0 ) {
		return FUNC_RETURN_OK;
	}
	Assert( memorymb != 0 && core != 0 );

	/* Expect integer cores to add. */
	Assert( trunc(core) == core );
	uint32_t ratio = trunc(1.0 * memorymb / core);
	int32_t  ratioindex = getResourceQueueRatioIndex(ratio);
	Assert( ratioindex >= 0 );

	if ( ratioindex >= 0 ) {
		addResourceBundleData(&(PQUEMGR->RatioTrackers[ratioindex]->TotalAllocated),
						  	  memorymb,
							  core);
	}
	else {
		elog(LOG, "To add resource (%d MB, %lf CORE), resource manager gets "
				  "ratio %u not tracked.",
				  memorymb,
				  core,
				  ratio);
		return RESQUEMGR_NO_RATIO;
	}

	/* New resource is added. Try to dispatch resource to queries. */
	PQUEMGR->toRunQueryDispatch = true;
	return FUNC_RETURN_OK;
}

int minusResourceFromResourceManagerByBundle(ResourceBundle bundle)
{
	return minusResourceFromReourceManager(bundle->MemoryMB, bundle->Core);
}

int minusResourceFromReourceManager(int32_t memorymb, double core)
{
	if ( memorymb == 0 && core ==0 )
		return FUNC_RETURN_OK;

	/* Expect integer cores to add. */
	Assert( trunc(core) == core );
	uint32_t ratio = trunc(1.0 * memorymb / core);
	int32_t  ratioindex = getResourceQueueRatioIndex(ratio);
	Assert( ratioindex >= 0 );

	if ( ratioindex >= 0 ) {
		minusResourceBundleData(&(PQUEMGR->RatioTrackers[ratioindex]->TotalAllocated),
						  	  	memorymb,
						  	  	core);
	}
	else {
		elog(WARNING, "HAWQ RM :: minusResourceFromReourceManager: "
					  "Wrong ratio %u not tracked.", ratio);
		return RESQUEMGR_NO_RATIO;
	}
	return FUNC_RETURN_OK;
}

void returnAllocatedResourceToLeafQueue(DynResourceQueueTrack track,
										int32_t			  	  memorymb,
										double				  core)
{
	minusResourceBundleData(&(track->TotalUsed), memorymb, core);

	elog(DEBUG3, "Return resource to queue %s (%d MB, %lf CORE).",
			  track->QueueInfo->Name,
			  memorymb, core);
}

void removePendingResourceRequestInRootQueue(int32_t memorymb, uint32_t core)
{
	if ( memorymb ==0 && core == 0 )
		return;
	Assert(memorymb > 0 && core > 0);

	uint32_t ratio 	    = memorymb / core;
	int32_t  ratioindex = 0;

	/* Get ratio index. */
	PAIR ratiopair = getHASHTABLENode(&(PQUEMGR->RatioIndex),
									  TYPCONVERT(void *, ratio));
	Assert( ratiopair != NULL );
	ratioindex = TYPCONVERT(int, ratiopair->Value);

	/* Add resource quota to free resource statistics. */
	minusResourceBundleData(&(PQUEMGR->RatioTrackers[ratioindex]->TotalPending),
							memorymb,
							core);
	Assert(PQUEMGR->RatioTrackers[ratioindex]->TotalPending.MemoryMB >= 0 &&
		   PQUEMGR->RatioTrackers[ratioindex]->TotalPending.Core >= 0);

	if ( PQUEMGR->RatioTrackers[ratioindex]->TotalPending.MemoryMB == 0 &&
		 PQUEMGR->RatioTrackers[ratioindex]->TotalPending.Core == 0 )
	{
		PQUEMGR->RatioTrackers[ratioindex]->TotalPendingStartTime = 0;
	}
	else if ( memorymb > 0 && core > 0 ){
		PQUEMGR->RatioTrackers[ratioindex]->TotalPendingStartTime = gettime_microsec();
	}

	elog(LOG, "Removed pending GRM request from root resource queue by "
			  "(%d MB, %lf CORE) to (%d MB, %lf CORE)",
			  memorymb,
			  core * 1.0,
			  PQUEMGR->RatioTrackers[ratioindex]->TotalPending.MemoryMB,
			  PQUEMGR->RatioTrackers[ratioindex]->TotalPending.Core);
}


void clearPendingResourceRequestInRootQueue(void)
{
	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i )
	{
		if ( PQUEMGR->RatioTrackers[i]->TotalPending.MemoryMB > 0 )
		{
			removePendingResourceRequestInRootQueue(
					PQUEMGR->RatioTrackers[i]->TotalPending.MemoryMB,
					PQUEMGR->RatioTrackers[i]->TotalPending.Core);
		}
	}
}

/*
 * Dispatching allocated resource to queuing queries.
 */
int dispatchResourceToQueries_EVEN(DynResourceQueueTrack track)
{
	/* Check how many segments are available to dispatch. */
	int availsegnum = trunc((track->TotalAllocated.MemoryMB -
							 track->TotalUsed.MemoryMB) /
					  	  	track->QueueInfo->SegResourceQuotaMemoryMB);
	int counter = 0;
	int segcounter = 0;
	int segmincounter = 0;

	DQUEUE_LOOP_BEGIN(&(track->QueryResRequests), iter, ConnectionTrack, conntrack)
		/* Consider concurrency no more than defined parallel count. */
		/* TODO: Consider more here... */
		if ( counter + track->NumOfRunningQueries >= track->QueueInfo->ParallelCount )
			break;
		/* Check if the minimum segment requirement is met. */
		if ( segmincounter + conntrack->SegNumMin > availsegnum )
		{
			break;
		}
		segcounter    += conntrack->SegNum;
		segmincounter += conntrack->SegNumMin;
		counter++;
	DQUEUE_LOOP_END

	if ( counter == 0 )
	{
		/* TODO:: Maybe too conservative. */
		detectAndDealWithDeadLock(track);
		return FUNC_RETURN_OK; /* Expect requests are processed in next loop. */
	}

	/* Dispatch segments */
	DQueueData todisp;
	initializeDQueue(&todisp, PCONTEXT);
	for ( int i = 0 ; i < counter ; ++i )
	{
		ConnectionTrack conn = removeDQueueHeadNode(&(track->QueryResRequests));
		conn->SegNumActual = conn->SegNumMin;
		insertDQueueTailNode(&todisp, conn);
		availsegnum -= conn->SegNumMin;
	}

	DQueueNode pnode = getDQueueContainerHead(&todisp);
	int fullcount = 0;
	while(availsegnum > 0)
	{
		ConnectionTrack conn = (ConnectionTrack)(pnode->Data);
		if ( conn->SegNum > conn->SegNumActual )
		{
			conn->SegNumActual++;
			availsegnum--;
			fullcount=0;
		}
		else
		{
			fullcount++;
		}
		if ( fullcount == counter )
		{
			break;
		}
		pnode = pnode->Next == NULL ? getDQueueContainerHead(&todisp) : pnode->Next;
	}

	/* Actually allocate segments from hosts in resource pool and send response.*/
	for ( int processidx = 0 ; processidx < counter ; ++processidx )
	{
		ConnectionTrack conn = removeDQueueHeadNode(&todisp);
		elog(DEBUG3, "Resource manager tries to dispatch resource to connection %d. "
		   		  	 "Expect (%d MB, %lf CORE) x %d(max %d min %d) segment(s). "
		   		  	 "Original vseg %d(min %d). "
		   		  	 "VSeg limit per segment %d VSeg limit per query %d",
					 conn->ConnID,
				     conn->SegMemoryMB,
					 conn->SegCore,
					 conn->SegNumActual,
					 conn->SegNum,
					 conn->SegNumMin,
					 conn->MaxSegCountFixed,
					 conn->MinSegCountFixed,
					 conn->VSegLimitPerSeg,
					 conn->VSegLimit);

		/* Build resource. */
		int32_t segnumact = 0;
		allocateResourceFromResourcePool(conn->SegNumActual,
										 conn->SegNumMin,
										 conn->SegMemoryMB,
										 conn->SegCore,
										 conn->IOBytes,
										 conn->SliceSize,
										 conn->VSegLimitPerSeg,
										 conn->SegPreferredHostCount,
										 conn->SegPreferredHostNames,
										 conn->SegPreferredScanSizeMB,
										 /* If the segment count is fixed. */
										 conn->MinSegCountFixed == conn->MaxSegCountFixed,
										 &(conn->Resource),
										 &segnumact,
										 &(conn->SegIOBytes));
		if ( segnumact >= conn->SegNumMin )
		{
			elog(DEBUG3, "Resource manager dispatched %d segment(s) to connection %d",
						 segnumact,
						 conn->ConnID);
			conn->SegNumActual = segnumact;

			/* Mark resource used in resource queue. */
			addResourceBundleData(&(track->TotalUsed),
								  conn->SegMemoryMB * conn->SegNumActual,
								  conn->SegCore     * conn->SegNumActual);
			minusResourceBundleData(&(track->TotalRequest),
									conn->SegMemoryMB * conn->SegNum,
									conn->SegCore     * conn->SegNum);
			track->NumOfRunningQueries++;

			/* Unlock and update session resource usage. */
			unlockSessionResource(&(track->DLDetector), conn->SessionID);
			addSessionInUseResource(&(track->DLDetector),
									conn->SessionID,
									conn->SegMemoryMB * conn->SegNumActual,
									conn->SegCore     * conn->SegNumActual);

			/* Transform the connection track status */
			transformConnectionTrackProgress(conn, CONN_PP_RESOURCE_QUEUE_ALLOC_DONE);

			/* Build response message and send it out. */
			buildAcquireResourceResponseMessage(conn);
		}
		else
		{
			/*
			 * In case we have 0 segments allocated. This may occur because we
			 * have too many resource small pieces. In this case, we treat the
			 * resource allocation failed, and HAWQ RM tries this connection
			 * again later. We expect some other connections return the resource
			 * back later or some more resource allocated from GRM.
		     */
			elog(WARNING, "HAWQ RM :: Can not find enough number of hosts "
						  "containing sufficient resource for the connection %d.",
						  conn->ConnID);
			elog(WARNING, "HAWQ RM :: Found %d vsegments allocated", segnumact);
			if ( segnumact > 0 )
			{
				Assert(!conn->isOld);
				returnResourceToResourcePool(conn->SegMemoryMB,
											 conn->SegCore,
											 conn->SegIOBytes,
											 conn->SliceSize,
											 &(conn->Resource),
											 conn->isOld);
			}

			/* Mark the request has resource fragment problem. */
			if ( !conn->troubledByFragment )
			{
				conn->troubledByFragmentTimestamp = gettime_microsec();
				conn->troubledByFragment 		  = true;
			}

			elog(LOG, "Resource fragment problem is probably encountered. "
					  "Session "INT64_FORMAT" expects minimum %d virtual segments.",
					  conn->SessionID,
					  conn->SegNumMin);

			/* Decide whether continue to process next query request. */
			if ( rm_force_fifo_queue )
			{
				insertDQueueHeadNode(&todisp, conn);
				break;
			}
			else
			{
				insertDQueueTailNode(&todisp, conn);
			}
		}
	}

	/* Return the request not completed yet. */
	while( todisp.NodeCount > 0 ) {
		ConnectionTrack conn = (ConnectionTrack)(removeDQueueTailNode(&todisp));
		insertDQueueHeadNode(&(track->QueryResRequests), (void *)conn);
	}
	cleanDQueue(&todisp);

	return FUNC_RETURN_OK;
}
int dispatchResourceToQueries_FIFO(DynResourceQueueTrack track)
{
	return FUNC_RETURN_OK;
}

void buildAcquireResourceResponseMessage(ConnectionTrack conn)
{
	ListCell *cell = NULL;

	Assert( conn != NULL );
	resetSelfMaintainBuffer(&(conn->MessageBuff));

	/* Set message head. */
	RPCResponseHeadAcquireResourceFromRMData response;
	response.Result 	  			= FUNC_RETURN_OK;
	response.Reserved1	  			= 0;
	response.SegCount	  			= conn->SegNumActual;
	response.SegMemoryMB  			= conn->SegMemoryMB;
	response.SegCore	  			= conn->SegCore;
	response.HostCount				= list_length(conn->Resource);
	response.Reserved2				= 0;
	appendSMBVar(&(conn->MessageBuff), response);

	/* Append HDFS host name index values. */
	uint32_t hdfsidxsize = __SIZE_ALIGN64(sizeof(uint32_t) * conn->SegNumActual);
	prepareSelfMaintainBuffer(&(conn->MessageBuff), hdfsidxsize, true);
	uint32_t *indexarray = (uint32_t *)getSMBCursor(&(conn->MessageBuff));

	int segi = 0;
	foreach(cell, conn->Resource)
	{
		VSegmentCounterInternal vsegcnt = (VSegmentCounterInternal)lfirst(cell);
		for ( int i = 0 ; i < vsegcnt->VSegmentCount ; ++i )
		{
			indexarray[segi] = vsegcnt->HDFSNameIndex;
			segi++;
		}
	}
	jumpforwardSelfMaintainBuffer(&(conn->MessageBuff), hdfsidxsize);

	/* Prepare machine id information. */
	uint32_t messagecursize = getSMBContentSize(&(conn->MessageBuff));
	uint32_t hoffsetsize    = __SIZE_ALIGN64(sizeof(uint32_t) * conn->SegNumActual);
	uint32_t *hoffsetarray  = (uint32_t *)rm_palloc0(PCONTEXT, hoffsetsize);

	/* Temporary buffer containing all distinct machineid instances. */
	SelfMaintainBufferData machineids;
	initializeSelfMaintainBuffer(&machineids, PCONTEXT);

	segi = 0;
	foreach(cell, conn->Resource)
	{
		VSegmentCounterInternal vsegcnt = (VSegmentCounterInternal)lfirst(cell);
		/* Set host offset. */
		for ( int i = 0 ; i < vsegcnt->VSegmentCount ; ++i ) {
			hoffsetarray[segi] = messagecursize +
							     hoffsetsize +
								 getSMBContentSize(&(machineids));
			segi++;
		}

		/* Append machine id. */
		appendSelfMaintainBuffer(&machineids,
								 (char *)(&(vsegcnt->Resource->Stat->Info)),
								 vsegcnt->Resource->Stat->Info.Size);

		elog(DEBUG3, "Resource manager added machine %s:%d containing %d segment(s) "
					 "in response of acquiring resource.",
					 GET_SEGRESOURCE_HOSTNAME(vsegcnt->Resource),
					 vsegcnt->Resource->Stat->Info.port,
					 vsegcnt->VSegmentCount);
	}

	/* Build complete message. */
	appendSelfMaintainBuffer(&(conn->MessageBuff),
							 (char *)hoffsetarray,
							 hoffsetsize);
	appendSelfMaintainBuffer(&(conn->MessageBuff),
							 machineids.Buffer,
							 machineids.Cursor + 1);

	conn->MessageSize  = conn->MessageBuff.Cursor + 1;
	conn->MessageID    = RESPONSE_QD_ACQUIRE_RESOURCE;
	conn->ResAllocTime = gettime_microsec();

	elog(LOG, "Latency of getting resource allocated is "UINT64_FORMAT "us",
			  conn->ResAllocTime - conn->ResRequestTime);

	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	PCONTRACK->ConnToSend = lappend(PCONTRACK->ConnToSend, conn);
	MEMORY_CONTEXT_SWITCH_BACK

	/* Clean up temporary variables. */
	destroySelfMaintainBuffer(&machineids);
	rm_pfree(PCONTEXT, hoffsetarray);
}

void detectAndDealWithDeadLock(DynResourceQueueTrack track)
{
	uint32_t availmemorymb = track->ClusterMemoryMaxMB -
						     track->DLDetector.LockedTotalMemoryMB;
	double   availcore     = track->ClusterVCoreMax -
						     track->DLDetector.LockedTotalCore;

	ConnectionTrack firstreq = (ConnectionTrack)
							   getDQueueHeadNodeData(&(track->QueryResRequests));
	if ( firstreq == NULL )
	{
		return;
	}

	uint32_t expmemorymb = firstreq->SegMemoryMB * firstreq->SegNumMin;
	double   expcore     = firstreq->SegCore     * firstreq->SegNumMin;

	if ( expmemorymb > track->ClusterMemoryMaxMB &&
		 expcore     > track->ClusterVCoreMax )
	{
		/* It is impossible to satisfy the request by checking deadlock. */
		return;
	}

	while((availmemorymb < expmemorymb ||
		   availcore     < expcore) &&
		  track->QueryResRequests.NodeCount > 0 ) {
		DQueueNode tail = getDQueueContainerTail(&(track->QueryResRequests));
		SessionTrack strack = NULL;
		while(tail != NULL) {
			strack = findSession(&(track->DLDetector),
								 ((ConnectionTrack)(tail->Data))->SessionID);
			if ( strack != NULL && strack->InUseTotalMemoryMB > 0 )
				break;
			tail = tail->Prev;
		}
		if ( tail != NULL ) {
			ConnectionTrack canceltrack = (ConnectionTrack)
										  removeDQueueNode(&(track->QueryResRequests), tail);
			Assert(canceltrack != NULL);
			availmemorymb += strack->InUseTotalMemoryMB;
			availcore     += strack->InUseTotalCore;

			/* Unlock the resource. */
			unlockSessionResource(&(track->DLDetector), canceltrack->SessionID);

			/* Cancel this request. */
			RPCResponseAcquireResourceFromRMERRORData errresponse;
			/* Send error message. */
			errresponse.Result   = RESQUEMGR_DEADLOCK_DETECTED;
			errresponse.Reserved = 0;

			buildResponseIntoConnTrack( canceltrack,
										(char *)&errresponse,
										sizeof(errresponse),
										canceltrack->MessageMark1,
										canceltrack->MessageMark2,
										RESPONSE_QD_ACQUIRE_RESOURCE);
			transformConnectionTrackProgress(canceltrack,
											 CONN_PP_RESOURCE_QUEUE_ALLOC_FAIL);

			canceltrack->ResponseSent = false;

			MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
			PCONTRACK->ConnToSend = lappend(PCONTRACK->ConnToSend, canceltrack);
			MEMORY_CONTEXT_SWITCH_BACK
		}
	}
}

void timeoutDeadResourceAllocation(void)
{
	uint64_t curmsec = gettime_microsec();

	if ( curmsec - PQUEMGR->LastCheckingDeadAllocationTime < 1000000L * 5 ) {
		return;
	}

	/* Go through all current allocated connection tracks. */
	List     *allcons = NULL;
	ListCell *cell	  = NULL;

	getAllPAIRRefIntoList(&(PCONTRACK->Connections), &allcons);

	foreach(cell, allcons)
	{
		ConnectionTrack curcon = (ConnectionTrack)(((PAIR)lfirst(cell))->Value);

		switch(curcon->Progress)
		{

		case CONN_PP_RESOURCE_QUEUE_ALLOC_DONE:
		{
			elog(DEBUG5, "Find allocated resource that should check timeout. "
						 "ConnID %d",
						 curcon->ConnID);

			if ( curmsec - curcon->LastActTime >
				 1000000L * rm_resource_noaction_timeout )
			{
				elog(LOG, "The allocated resource timeout is detected. "
						  "ConnID %d",
						  curcon->ConnID);
				returnResourceToResQueMgr(curcon);
				returnConnectionToQueue(curcon, false);
				if ( curcon->CommBuffer != NULL )
				{
					curcon->CommBuffer->toClose = true;
					curcon->CommBuffer->forcedClose = true;
				}
			}
			break;
		}

		case CONN_PP_RESOURCE_QUEUE_ALLOC_WAIT:
		{
			if ( curmsec - curcon->LastActTime >
				 1000000L * rm_resource_noaction_timeout )
			{
				elog(LOG, "The queued resource request timeout is detected. "
						  "ConnID %d",
						  curcon->ConnID);
				cancelResourceAllocRequest(curcon);
				returnConnectionToQueue(curcon, false);
				if ( curcon->CommBuffer != NULL )
				{
					curcon->CommBuffer->toClose = true;
					curcon->CommBuffer->forcedClose = true;
				}
			}
			break;
		}

		case CONN_PP_REGISTER_DONE:
		{
			if ( curmsec - curcon->LastActTime >
				 1000000L * rm_resource_noaction_timeout )
			{
				elog(LOG, "The registered connection timeout is detected. "
						  "ConnID %d",
						  curcon->ConnID);
				returnConnectionToQueue(curcon, false);
				if ( curcon->CommBuffer != NULL )
				{
					curcon->CommBuffer->toClose = true;
					curcon->CommBuffer->forcedClose = true;
				}
			}
			break;
		}
		}

	}
	freePAIRRefList(&(PCONTRACK->Connections), &allcons);

	PQUEMGR->LastCheckingDeadAllocationTime = curmsec;
}

void timeoutQueuedRequest(void)
{
	uint64_t curmsec = gettime_microsec();

	if ( curmsec - PQUEMGR->LastCheckingQueuedTimeoutTime < 1000000L * 5 ) {
		return;
	}

	/* Go through all to be processed requests. */
	ConnectionTrack  ct    		= NULL;
	List			*tryagain	= NULL;

	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	while( list_length(PCONTRACK->ConnHavingRequests) > 0)
	{
		ct = (ConnectionTrack)lfirst(list_head(PCONTRACK->ConnHavingRequests));
		PCONTRACK->ConnHavingRequests = list_delete_first(PCONTRACK->ConnHavingRequests);

		/*
		 * Case 1. RM has no workable cluster built yet, the request is not
		 * 		   added into resource queue manager queues.
		 */
		elog(DEBUG3, "Deferred connection track is found. "
					 " Conn Time " UINT64_FORMAT
					 " Curr Time " UINT64_FORMAT
					 " Delta " UINT64_FORMAT,
					 ct->ConnectTime,
					 curmsec,
					 curmsec - ct->ConnectTime);

		if ( curmsec - ct->ConnectTime > 1000000L * rm_query_resource_noresource_timeout )
		{
			elog(WARNING, "Waiting request timeout is detected due to no "
						  "available cluster.");
			/* Build timeout response. */
			buildTimeoutResponseForQueuedRequest(ct, RESQUEMGR_NOCLUSTER_TIMEOUT);
		}
		else
		{
			tryagain = lappend(tryagain, ct);
		}
	}

	while( list_length(tryagain) > 0 )
	{
		void *move = lfirst(list_head(tryagain));
		tryagain = list_delete_first(tryagain);
		PCONTRACK->ConnHavingRequests = lappend(PCONTRACK->ConnHavingRequests, move);
	}

	/* Go through all current allocated connection tracks. */
	curmsec = gettime_microsec();

	List     *allcons = NULL;
	ListCell *cell	  = NULL;

	getAllPAIRRefIntoList(&(PCONTRACK->Connections), &allcons);

	foreach(cell, allcons)
	{
		ConnectionTrack curcon = (ConnectionTrack)(((PAIR)lfirst(cell))->Value);

		if ( curcon->Progress == CONN_PP_RESOURCE_QUEUE_ALLOC_WAIT )
		{
			elog(DEBUG3, "Check waiting connection track now.");
			/*
			 * Check if corresponding mem core ratio tracker has long enough
			 * time to waiting for GRM containers.
			 */
			DynResourceQueueTrack queuetrack = (DynResourceQueueTrack)(curcon->QueueTrack);
			int index = getResourceQueueRatioIndex(queuetrack->MemCoreRatio);
			/* Case 1. No available cluster information yet. We check only top
			 * 		   query waiting time and resource request time.
			 * Case 2. We have available cluster information, we check the
			 * 		   resource increase pending time and top query waiting time.
			 */
			Assert(PQUEMGR->RootTrack != NULL);

			bool tocancel = false;

			if ( ( (PQUEMGR->RootTrack->ClusterSegNumberMax == 0) &&
				   (curmsec - curcon->ResRequestTime >
						1000000L * rm_query_resource_noresource_timeout ) ) ||
				 ( (PQUEMGR->RatioTrackers[index]->TotalPendingStartTime > 0) &&
				   (curmsec - PQUEMGR->RatioTrackers[index]->TotalPendingStartTime >
						1000000L * rm_query_resource_noresource_timeout) &&
				   (curcon->HeadQueueTime > 0) &&
				   (curmsec - curcon->HeadQueueTime >
				 	 	1000000L * rm_query_resource_noresource_timeout) ) )
			{
				elog(LOG, "The queued resource request no resource timeout is "
						  "detected. ConnID %d",
						  curcon->ConnID);
				tocancel = true;
			}

			/* Case 3. Check if resource fragment problem lasts too long time. */
			if ( curcon->troubledByFragment &&
			     curmsec - curcon->troubledByFragmentTimestamp >
					 1000000L * rm_query_resource_noresource_timeout &&
				 ((DynResourceQueueTrack)(curcon->QueueTrack))->NumOfRunningQueries == 0 )
			{
				elog(LOG, "The queued resource request timeout is detected due to "
						  "resource fragment problem. ConnID %d",
						  curcon->ConnID);
				tocancel = true;
			}

			/*
			 * Case 4. Check if resource is not possible to be met based on
			 * 		   current cluster resource. This may occur if the table was
			 * 		   created by a big cluster but now the cluster shrinks too
			 * 		   much.
			 */
			if ( (curcon->HeadQueueTime > 0) &&
				 (curmsec - curcon->HeadQueueTime >
	 	 			  1000000L * rm_query_resource_noresource_timeout) &&
				 (curcon->SegNumMin * curcon->SegMemoryMB >
				 	  ((DynResourceQueueTrack)curcon->QueueTrack)->ClusterMemoryMaxMB) )
			{
				elog(LOG, "The queued resource request timeout is detected due to "
						  "no enough cluster resource. ConnID %d",
						  curcon->ConnID);
				tocancel = true;
			}

			if ( tocancel )
			{
				cancelResourceAllocRequest(curcon);
				returnConnectionToQueue(curcon, false);
			}
		}
	}
	freePAIRRefList(&(PCONTRACK->Connections), &allcons);
	PQUEMGR->LastCheckingQueuedTimeoutTime = curmsec;
	MEMORY_CONTEXT_SWITCH_BACK
}

void buildTimeoutResponseForQueuedRequest(ConnectionTrack conntrack, uint32_t reason)
{
	RPCResponseAcquireResourceFromRMERRORData errresponse;
	errresponse.Result   = reason;
	errresponse.Reserved = 0;
	buildResponseIntoConnTrack( conntrack,
								(char *)&errresponse,
								sizeof(errresponse),
								conntrack->MessageMark1,
								conntrack->MessageMark2,
								RESPONSE_QD_ACQUIRE_RESOURCE);
	transformConnectionTrackProgress(conntrack, CONN_PP_TIMEOUT_FAIL);
	conntrack->ResponseSent = false;
	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	PCONTRACK->ConnToSend = lappend(PCONTRACK->ConnToSend, conntrack);
	MEMORY_CONTEXT_SWITCH_BACK
}

bool isAllResourceQueueIdle(void)
{
	ListCell *cell = NULL;
	foreach(cell, PQUEMGR->Queues)
	{
		DynResourceQueueTrack quetrack = lfirst(cell);
		if ( quetrack->TotalUsed.MemoryMB > 0 || quetrack->TotalUsed.Core > 0 )
		{
			return false;
		}
	}
	return true;
}

void resetAllDeadLockDetector(void)
{
	ListCell *cell = NULL;
	foreach(cell, PQUEMGR->Queues)
	{
		DynResourceQueueTrack quetrack = lfirst(cell);
		resetResourceDeadLockDetector(&(quetrack->DLDetector));
	}
}

void getIdleResourceRequest(int32_t *mem, double *core)
{
	*mem  = PRESPOOL->MemCoreRatio * PRESPOOL->AvailNodeCount *
		    rm_seg_container_default_waterlevel;
	*core = 1.0 * PRESPOOL->AvailNodeCount *
			rm_seg_container_default_waterlevel;
}

void setForcedReturnGRMContainerCount(void)
{
	/* If some queue has locked resource, dont do GRM container breathe. */
	ListCell *cell = NULL;
	foreach(cell, PQUEMGR->Queues)
	{
		DynResourceQueueTrack quetrack = lfirst(cell);

		if ( quetrack->DLDetector.LockedTotalMemoryMB > 0 )
		{
			elog(LOG, "Queue %s has potential resource deadlock, skip breathe.",
					  quetrack->QueueInfo->Name);
			PQUEMGR->GRMQueueCurCapacity   = PQUEMGR->GRMQueueCapacity;
			PQUEMGR->GRMQueueResourceTight = false;
			return;
		}
	}

	/* Get current GRM container size. */
	int clusterctnsize = getClusterGRMContainerSize();
	int toretctnsize = 0;
	if ( PQUEMGR->GRMQueueCurCapacity > PQUEMGR->GRMQueueCapacity )
	{
		/*
		 * We would like to return as many containers as possible to make queue
		 * usage lower than expected capacity.
		 */
		double r = (PQUEMGR->GRMQueueCurCapacity - PQUEMGR->GRMQueueCapacity) /
				   PQUEMGR->GRMQueueCurCapacity;
		elog(DEBUG3, "GRM queue cur capacity %lf is larger than capacity %lf. "
					 "ratio %lf, curent GRM container size %d",
					 PQUEMGR->GRMQueueCurCapacity,
					 PQUEMGR->GRMQueueCapacity,
					 r,
					 clusterctnsize);
		toretctnsize = ceil(r * clusterctnsize);
	}
	else
	{
		if ( rm_grm_breath_return_percentage > 0 )
		{
			double r = 1.0 * clusterctnsize * rm_grm_breath_return_percentage / 100;
			toretctnsize = ceil(r);
			elog(DEBUG3, "GRM queue cur capacity %lf is not larger than capacity %lf. "
						 "Calculated r %lf",
						 PQUEMGR->GRMQueueCurCapacity,
						 PQUEMGR->GRMQueueCapacity,
						 r);
		}
	}

	elog(LOG, "Resource manager expects to breathe out %d GRM containers. "
			  "Total %d GRM containers, ",
			  toretctnsize,
			  clusterctnsize);

	/* Restore queue report to avoid force return again. */
	PQUEMGR->ForcedReturnGRMContainerCount = toretctnsize;
	PQUEMGR->GRMQueueCurCapacity		   = PQUEMGR->GRMQueueCapacity;
	PQUEMGR->GRMQueueResourceTight 		   = false;
}

void dumpResourceQueueStatus(const char *filename)
{
	if ( filename == NULL ) { return; }
	FILE *fp = fopen(filename, "w");

	fprintf(fp, "Maximum capacity of queue in global resource manager cluster %lf",
				PQUEMGR->GRMQueueMaxCapacity);
	fprintf(fp, "Number of resource queues : %d\n", list_length(PQUEMGR->Queues));

	/* Output each resource queue. */
	ListCell *cell = NULL;
	foreach(cell, PQUEMGR->Queues)
	{
		DynResourceQueueTrack quetrack = lfirst(cell);

		fprintf(fp, "QUEUE(name=%s:parent=%s:children=%d:busy=%d:paused=%d),",
				    quetrack->QueueInfo->Name,
					quetrack->ParentTrack != NULL ?
							quetrack->ParentTrack->QueueInfo->Name :
							"NULL",
					list_length(quetrack->ChildrenTracks),
					quetrack->isBusy ? 1 : 0,
					quetrack->pauseAllocation ? 1 : 0);

		fprintf(fp, "REQ(conn=%d:request=%d:running=%d),",
					quetrack->CurConnCounter,
					quetrack->QueryResRequests.NodeCount,
					quetrack->NumOfRunningQueries);

		fprintf(fp, "SEGCAP(ratio=%u:ratioidx=%d:segmem=%dMB:segcore=%lf:"
					"segnum=%d:segnummax=%d),",
					quetrack->MemCoreRatio,
					quetrack->RatioIndex,
					quetrack->QueueInfo->SegResourceQuotaMemoryMB,
					quetrack->QueueInfo->SegResourceQuotaVCore,
					quetrack->ClusterSegNumber,
					quetrack->ClusterSegNumberMax);

		fprintf(fp, "QUECAP(memmax=%u:coremax=%lf:"
					"memper=%lf:mempermax=%lf:coreper=%lf:corepermax=%lf),",
					quetrack->ClusterMemoryMaxMB,
					quetrack->ClusterVCoreMax,
					quetrack->ClusterMemoryActPer,
					quetrack->ClusterMemoryMaxPer,
					quetrack->ClusterVCoreActPer,
					quetrack->ClusterVCoreMaxPer);

		fprintf(fp, "QUEUSE(alloc=(%u MB,%lf CORE):"
					       "request=(%u MB,%lf CORE):"
					       "inuse=(%u MB,%lf CORE))\n",
					quetrack->TotalAllocated.MemoryMB,
					quetrack->TotalAllocated.Core,
					quetrack->TotalRequest.MemoryMB,
					quetrack->TotalRequest.Core,
					quetrack->TotalUsed.MemoryMB,
					quetrack->TotalUsed.Core);
	}

	fprintf(fp, "Number of mem/core ratios : %d\n", PQUEMGR->RatioCount);

	/* Output each mem/core ratio. */
	for( int i = 0 ; i < PQUEMGR->RatioCount ; ++i )
	{
		fprintf(fp, "RATIO(ratio=%u:",
					PQUEMGR->RatioReverseIndex[i]);

		if ( PQUEMGR->RatioWaterMarks[i].NodeCount == 0 )
		{
			fprintf(fp, "mem=0MB:core=0.0:time=NULL)\n");
		}
		else
		{
			DynMemoryCoreRatioWaterMark mark =
				(DynMemoryCoreRatioWaterMark)
				getDQueueHeadNodeData(&(PQUEMGR->RatioWaterMarks[i]));
			fprintf(fp, "mem=%uMB:core=%lf:time=%s)\n",
						mark->ClusterMemoryMB,
						mark->ClusterVCore,
						format_time_microsec(mark->LastRecordTime*1000000));
		}
	}

	fclose(fp);
}
