#include "envswitch.h"
#include "utils/linkedlist.h"
#include "utils/memutilities.h"
#include "utils/network_utils.h"
#include "utils/kvproperties.h"
#include "communication/rmcomm_QD2RM.h"
#include "communication/rmcomm_QD_RM_Protocol.h"
#include "communication/rmcomm_MessageHandler.h"
#include "catalog/pg_resqueue.h"
#include "catalog/catquery.h"
#include "access/xact.h"

#include "commands/comment.h"
#include "catalog/heap.h"

#include "gp-libpq-fe.h"
#include "gp-libpq-int.h"

int updateResqueueCatalog(int					 action,
					      DynResourceQueueTrack  queuetrack,
						  List					*rsqattr);

int performInsertActionForPGResqueue(List *colvalues, Oid *newoid);
int performUpdateActionForPGResqueue(List *colvalues, char *queuename);
int performDeleteActionForPGResqueue(char *queuename);

Datum getDatumFromStringValuForPGResqueue(int	 colindex,
								   		  char  *colvaluestr);

int buildInsertActionForPGResqueue(DynResourceQueue   queue,
							   	   List 			 *rsqattr,
							   	   List		   		**insvalues);

int buildUpdateActionForPGResqueue(DynResourceQueue   queue,
							   	   List 		   	 *rsqattr,
							   	   List		   	    **updvalues);

int buildUpdateStatusActionForPGResqueue(DynResourceQueue   queue,
								  	  	 List 		   	   *rsqattr,
								  	  	 List		   	  **updvalues);

void freeUpdateActionList(MCTYPE context, List **actions);

/* Column names of pg_resqueue table
 * mapping with the definition of table pg_resqueue in pg_resqueue.h
 */
const char* PG_Resqueue_Column_Names[Natts_pg_resqueue] = {
	"rsqname",
	"rsq_parent",
	"rsq_active_stats_cluster",
	"rsq_memory_limit_cluster",
	"rsq_core_limit_cluster",
	"rsq_resource_upper_factor",
	"rsq_allocation_policy",
	"rsq_vseg_resource_quota",
	"rsq_vseg_upper_limit",
	"rsq_creation_time",
	"rsq_update_time",
	"rsq_status"
};

/**
 * HAWQ RM handles the resource queue definition manipulation including CREATE,
 * ALTER and DROP RESOURCE QUEUE statements.
 */
bool handleRMDDLRequestManipulateResourceQueue(void **arg)
{
	int      				res		 		= FUNC_RETURN_OK;
	uint32_t				ddlres   		= FUNC_RETURN_OK;
	ConnectionTrack        *conntrack       = (ConnectionTrack *)arg;
	DynResourceQueueTrack 	newtrack 		= NULL;
	DynResourceQueueTrack   todroptrack		= NULL;
	DynResourceQueueTrack   toupdatetrack	= NULL;
	SelfMaintainBufferData  responsebuff;
	char 					errorbuf[1024] 	= "";
	bool					exist 			= false;
	List 				   *fineattr		= NULL;
	List 				   *rsqattr			= NULL;
	DynResourceQueue 		newqueue 		= NULL;
	DynResourceQueue        oldqueue        = NULL;

	/* Check context and retrieve the connection track based on connection id.*/
	RPCRequestHeadManipulateResQueue request =
		(RPCRequestHeadManipulateResQueue)((*conntrack)->MessageBuff.Buffer);

	elog(LOG, "Resource manager gets a request from ConnID %d to submit resource "
			  "queue DDL statement.",
			  request->ConnID);

	elog(DEBUG3, "With attribute list size %d", request->WithAttrLength);

	if ( (*conntrack)->ConnID == INVALID_CONNID )
	{
		res = retrieveConnectionTrack((*conntrack), request->ConnID);
		if ( res != FUNC_RETURN_OK )
		{
			elog(WARNING, "Not valid resource context with id %d.", request->ConnID);
			goto senderr;
		}

		elog(DEBUG5, "Resource manager fetched existing connection track "
					 "ID=%d, Progress=%d.",
					 (*conntrack)->ConnID,
					 (*conntrack)->Progress);
	}

	/*
	 * Only registered connection can manipulate resource queue, the status
	 * should be CONN_REGISTER_DONE.
	 */
	Assert( (*conntrack)->Progress == CONN_PP_REGISTER_DONE );

	/*
	 * Only the super user can manipulate resource queue. This is already
	 * checked before sending RPC to RM this process.
	 */
	Assert((*conntrack)->User != NULL &&
		   ((UserInfo)((*conntrack)->User))->isSuperUser);

	/* Build property list for the resource queue to be created. */
	request = (RPCRequestHeadManipulateResQueue)((*conntrack)->MessageBuff.Buffer);

	/* Get resource queue name. */
	char *string = (*conntrack)->MessageBuff.Buffer +
				   sizeof(RPCRequestHeadManipulateResQueueData);
	KVProperty nameattr = createPropertyString(PCONTEXT,
											   NULL,
											   getRSQTBLAttributeName(RSQ_TBL_ATTR_NAME),
											   NULL,
											   string);
	{
		MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
		rsqattr = lappend(rsqattr, nameattr);
		MEMORY_CONTEXT_SWITCH_BACK
	}

	string += nameattr->Val.Len+1;

	/* Get with list. <key>=<value> */
	for ( int i = 0 ; i < request->WithAttrLength ; ++i )
	{
		KVProperty withattr = createPropertyEmpty(PCONTEXT);
		setSimpleStringNoLen(&(withattr->Key), string);
		string += withattr->Key.Len + 1;
		setSimpleStringNoLen(&(withattr->Val), string);
		string += withattr->Val.Len + 1;

		MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
		rsqattr = lappend(rsqattr, withattr);
		MEMORY_CONTEXT_SWITCH_BACK
	}

	/* Log the received attributes in DDL request. */
	ListCell *cell = NULL;
	foreach(cell, rsqattr)
	{
		KVProperty attribute = lfirst(cell);
		elog(LOG, "Resource manager received DDL Request: %s=%s",
				  attribute->Key.Str, attribute->Val.Str);
	}

	/* Shallow parse the 'withlist' attributes. */
	res = shallowparseResourceQueueWithAttributes(rsqattr,
												  &fineattr,
												  errorbuf,
												  sizeof(errorbuf));
	if (res != FUNC_RETURN_OK)
	{
		ddlres = res;
		elog(WARNING, ERRORPOS_FORMAT
			 "Can not recognize DDL attribute because %s",
			 ERRREPORTPOS,
			 errorbuf);
		goto senderr;
	}

	cell = NULL;
	foreach(cell, fineattr)
	{
		KVProperty attribute = lfirst(cell);
		elog(LOG, "DDL parsed request: %s=%s", attribute->Key.Str, attribute->Val.Str);

	}

	/* Add into resource queue hierarchy to validate the request. */
	switch(request->ManipulateAction)
	{
		case MANIPULATE_RESQUEUE_CREATE:
			/* Resource queue number check. */
			if (list_length(PQUEMGR->Queues) >= rm_max_resource_queue_number)
			{
				ddlres = RESQUEMGR_EXCEED_MAX_QUEUE_NUMBER;
				snprintf(errorbuf, sizeof(errorbuf),
						"exceed maximum resource queue number %d",
						rm_max_resource_queue_number);
				elog(WARNING, "Resource manager can not create resource queue "
							  "because %s",
							  errorbuf);
				goto senderr;
			}

			newqueue = rm_palloc0(PCONTEXT, sizeof(DynResourceQueueData));
			res = parseResourceQueueAttributes(fineattr,
											   newqueue,
											   errorbuf,
											   sizeof(errorbuf));
			if (res != FUNC_RETURN_OK)
			{
				rm_pfree(PCONTEXT, newqueue);
				ddlres = res;
				elog(WARNING, "Resource manager can not create resource queue "
							  "with its attributes because %s",
							  errorbuf);
				goto senderr;
			}

			res = checkAndCompleteNewResourceQueueAttributes(newqueue,
															 errorbuf,
															 sizeof(errorbuf));
			if (res != FUNC_RETURN_OK)
			{
				rm_pfree(PCONTEXT, newqueue);
				ddlres = res;
				elog(WARNING, "Resource manager can not complete resource queue's "
							  "attributes because %s",
							  errorbuf);
				goto senderr;
			}

			newtrack = NULL;
			res = createQueueAndTrack(newqueue,
									  &newtrack,
									  errorbuf,
									  sizeof(errorbuf));
			if (res != FUNC_RETURN_OK)
			{
				rm_pfree(PCONTEXT, newqueue);
				if (newtrack != NULL)
					rm_pfree(PCONTEXT, newtrack);
				ddlres = res;
				elog(WARNING, "Resource manager can not create resource queue %s "
							  "because %s",
							  newqueue->Name,
							  errorbuf);
				goto senderr;
			}

			res = updateResqueueCatalog(request->ManipulateAction,
										newtrack,
										rsqattr);
			if (res != FUNC_RETURN_OK)
			{
				ddlres = res;
				elog(WARNING, "Cannot update resource queue changes in pg_resqueue.");

				/* If fail in updating catalog table, revert previous operations in RM. */
				res = dropQueueAndTrack(newtrack, errorbuf, sizeof(errorbuf));
				if (res != FUNC_RETURN_OK)
				{
					elog(WARNING, "Resource manager cannot drop queue and track "
								  "because %s",
								  errorbuf);
				}
				goto senderr;
			}

			break;

		case MANIPULATE_RESQUEUE_ALTER:
			toupdatetrack = getQueueTrackByQueueName((char *)(nameattr->Val.Str),
					   	   	   	   	   	   	   	   	nameattr->Val.Len,
					   	   	   	   	   	   	   	   	&exist);
			if (!exist || toupdatetrack == NULL)
			{
				ddlres = RESQUEMGR_NO_QUENAME;
				snprintf(errorbuf, sizeof(errorbuf), "The queue doesn't exist");
				elog(WARNING, ERRORPOS_FORMAT
					 "Resource manager can not alter resource queue %s because %s",
				     ERRREPORTPOS,
				     nameattr->Val.Str,
				     errorbuf);
				goto senderr;
			}
			newqueue = toupdatetrack->QueueInfo;
			oldqueue = (DynResourceQueue)
					   rm_palloc0(PCONTEXT,
								  sizeof(DynResourceQueueData));
			memcpy(oldqueue, newqueue, sizeof(DynResourceQueueData));

			res = updateResourceQueueAttributes(fineattr,
												newqueue,
												errorbuf,
												sizeof(errorbuf));
			if (res != FUNC_RETURN_OK)
			{
				ddlres = res;
				elog(WARNING, ERRORPOS_FORMAT
					 "HAWQ RM Can not alter resource queue with its attributes "
					 "because %s",
					 ERRREPORTPOS,
					 errorbuf);
				/* If fail in updating catalog table, revert previous updates */
				memcpy(newqueue, oldqueue, sizeof(DynResourceQueueData));
				rm_pfree(PCONTEXT, oldqueue);
				goto senderr;
			}

			res = checkAndCompleteNewResourceQueueAttributes(newqueue,
															 errorbuf,
															 sizeof(errorbuf));
			if (res != FUNC_RETURN_OK)
			{
				ddlres = res;
				elog(WARNING, ERRORPOS_FORMAT
					 "HAWQ RM Can not complete resource queue's attributes "
					 "because %s",
					 ERRREPORTPOS,
					 errorbuf);
				/* If fail in updating catalog table, revert previous updates */
				memcpy(newqueue, oldqueue, sizeof(DynResourceQueueData));
				rm_pfree(PCONTEXT, oldqueue);
				goto senderr;
			}

			res = updateResqueueCatalog(request->ManipulateAction,
										toupdatetrack,
										rsqattr);
			if (res != FUNC_RETURN_OK)
			{
				ddlres = res;
				elog(WARNING, ERRORPOS_FORMAT
					 "Cannot alter resource queue changes in pg_resqueue.",
					 ERRREPORTPOS);

				/* If fail in updating catalog table, revert previous updates */
				memcpy(newqueue, oldqueue, sizeof(DynResourceQueueData));
				rm_pfree(PCONTEXT, oldqueue);
				goto senderr;
			}

			if(oldqueue)
			{
				rm_pfree(PCONTEXT, oldqueue);
			}

			break;

		case MANIPULATE_RESQUEUE_DROP:
			todroptrack = getQueueTrackByQueueName((char *)(nameattr->Val.Str),
												   nameattr->Val.Len,
												   &exist);
			if (!exist || todroptrack == NULL )
			{
				/* already check before send RPC to RM */
				Assert(exist);
				ddlres = RESQUEMGR_NO_QUENAME;
				snprintf(errorbuf, sizeof(errorbuf),
						"The queue doesn't exist");
				elog(WARNING, ERRORPOS_FORMAT
					 "Resource manager can not drop resource queue %s because %s",
					 ERRREPORTPOS,
					 nameattr->Val.Str,
					 errorbuf);
				goto senderr;
			}

			if ( list_length(todroptrack->ChildrenTracks) > 0 )
			{
				ddlres = RESQUEMGR_IN_USE;
				snprintf(errorbuf, sizeof(errorbuf),
						"The Resource Queue is a branch queue. "
						"Drop the children queues firstly.");
				elog(WARNING, ERRORPOS_FORMAT
					 "Resource manager can not drop resource queue %s because %s.",
					 ERRREPORTPOS,
					 nameattr->Val.Str,
					 errorbuf);
				goto senderr;
			}

			if (todroptrack->QueueInfo->OID == DEFAULTRESQUEUE_OID)
			{
				/* already check before send RPC to RM */
				Assert(todroptrack->QueueInfo->OID != DEFAULTRESQUEUE_OID);
				ddlres = RESQUEMGR_IN_USE;
				snprintf(errorbuf, sizeof(errorbuf),
						"pg_default as system queue cannot be dropped.");
				elog(WARNING, ERRORPOS_FORMAT
					 "Resource manager can not drop resource queue %s because %s",
					 ERRREPORTPOS,
					 nameattr->Val.Str,
					 errorbuf);
				goto senderr;
			}

			if (todroptrack->QueueInfo->OID == ROOTRESQUEUE_OID)
			{
				/* already check before send RPC to RM */
				Assert(todroptrack->QueueInfo->OID != ROOTRESQUEUE_OID);
				ddlres = RESQUEMGR_IN_USE;
				snprintf(errorbuf, sizeof(errorbuf),
						"pg_root as system queue cannot be dropped.");
				elog(WARNING, ERRORPOS_FORMAT
					 "Resource manager can not drop resource queue %s because %s",
					 ERRREPORTPOS,
					 nameattr->Val.Str,
					 errorbuf);
				goto senderr;
			}

			res = updateResqueueCatalog(request->ManipulateAction,
									    todroptrack,
										rsqattr);
			if (res != FUNC_RETURN_OK)
			{
				ddlres = res;
				snprintf(errorbuf, sizeof(errorbuf),
						 "Cannot update resource queue changes in pg_resqueue");
				elog(WARNING, ERRORPOS_FORMAT
					 "Resource manager cannot drop resource queue %s because %s",
					 ERRREPORTPOS,
					 nameattr->Val.Str,
					 errorbuf);
				goto senderr;
			}

			res = dropQueueAndTrack(todroptrack,errorbuf,sizeof(errorbuf));
			if (res != FUNC_RETURN_OK)
			{
				ddlres = res;
				elog(WARNING, ERRORPOS_FORMAT
				     "Resource manager can not dropQueueAndTrack because %s",
					 ERRREPORTPOS,
					 errorbuf);
				goto senderr;
			}

			break;

		default:
			Assert(false);
		}

	/* Refresh resource queue capacity. */
	refreshResourceQueuePercentageCapacity();
	/* Recalculate all memory/core ratio instances' limits. */
	refreshMemoryCoreRatioLimits();
	/* Refresh memory/core ratio level water mark. */
	refreshMemoryCoreRatioWaterMark();

	/* Build response. */
	RPCResponseHeadManipulateResQueueData response;
	response.Result 	= FUNC_RETURN_OK;
	response.Reserved 	= 0;

	/* Build message saved in the connection track instance. */
	buildResponseIntoConnTrack((*conntrack),
							   (char *)&response,
							   sizeof(response),
							   (*conntrack)->MessageMark1,
							   (*conntrack)->MessageMark2,
							   RESPONSE_QD_DDL_MANIPULATERESQUEUE);
	(*conntrack)->ResponseSent = false;
	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	PCONTRACK->ConnToSend = lappend(PCONTRACK->ConnToSend, *conntrack);
	MEMORY_CONTEXT_SWITCH_BACK

	/* Clean up temporary variable. */
	cleanPropertyList(PCONTEXT, &fineattr);
	cleanPropertyList(PCONTEXT, &rsqattr);
	return true;

senderr:
	initializeSelfMaintainBuffer(&responsebuff, PCONTEXT);
	appendSelfMaintainBuffer(&responsebuff, (void *)&ddlres, sizeof(uint32_t));
	appendSelfMaintainBufferTill64bitAligned(&responsebuff);

	if (ddlres != FUNC_RETURN_OK) {
		appendSelfMaintainBuffer(&responsebuff, errorbuf, strlen(errorbuf)+1);
	}

	appendSelfMaintainBufferTill64bitAligned(&responsebuff);

	/* Build message saved in the connection track instance. */
	buildResponseIntoConnTrack((*conntrack),
							   responsebuff.Buffer,
							   responsebuff.Cursor+1,
							   (*conntrack)->MessageMark1,
							   (*conntrack)->MessageMark2,
							   RESPONSE_QD_DDL_MANIPULATERESQUEUE);
	(*conntrack)->ResponseSent = false;
	{
		MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
		PCONTRACK->ConnToSend = lappend(PCONTRACK->ConnToSend, *conntrack);
		MEMORY_CONTEXT_SWITCH_BACK
	}
	destroySelfMaintainBuffer(&responsebuff);

	/* Clean up temporary variable. */
	cleanPropertyList(PCONTEXT, &fineattr);
	cleanPropertyList(PCONTEXT, &rsqattr);
	return true;
}

bool handleRMDDLRequestManipulateRole(void **arg)
{
	RPCResponseHeadManipulateRoleData response;

    ConnectionTrack conntrack 	= (ConnectionTrack)(*arg);
    UserInfo 		user;
    int 			res			= FUNC_RETURN_OK;

    RPCRequestHeadManipulateRole request =
    		(RPCRequestHeadManipulateRole )conntrack->MessageBuff.Buffer;

    switch(request->Action)
    {
    	case MANIPULATE_ROLE_RESQUEUE_CREATE:
    	{
			user = rm_palloc0(PCONTEXT, sizeof(UserInfoData));
			user->OID 		  = request->RoleOID;
			user->QueueOID 	  = request->QueueOID;
			user->isSuperUser = request->isSuperUser;
			strncpy(user->Name, request->Name, sizeof(user->Name)-1);
			res = createUser(user, NULL, 0);
			elog(LOG, "Resource manager handles request CREATE ROLE oid:%d, "
					  "queueID:%d, isSuper:%d, roleName:%s",
					  request->RoleOID,
					  request->QueueOID,
					  request->isSuperUser,
					  request->Name);
			break;
    	}
    	case MANIPULATE_ROLE_RESQUEUE_ALTER:
    	{
			res = dropUser((int64_t)request->RoleOID, request->Name);
			if ( res != FUNC_RETURN_OK )
			{
				elog(WARNING, "Resource manager cannot find user "INT64_FORMAT
							  " to alter.",
							  (int64_t)(request->RoleOID));
				goto exit;
			}

			/* Create new user instance. */
			user = (UserInfo)rm_palloc0(PCONTEXT, sizeof(UserInfoData));
			user->OID 		  = request->RoleOID;
			user->QueueOID 	  = request->QueueOID;
			user->isSuperUser = request->isSuperUser;
			strncpy(user->Name, request->Name, sizeof(user->Name)-1);
			res = createUser(user, NULL, 0);
			elog(LOG, "Resource manager handles request ALTER ROLE oid:%d, "
					  "queueID:%d, isSuper:%d, roleName:%s",
					  request->RoleOID,
					  request->QueueOID,
					  request->isSuperUser,
					  request->Name);
			break;
    	}
		case MANIPULATE_ROLE_RESQUEUE_DROP:
		{
			res = dropUser((int64_t)request->RoleOID, request->Name);
			if ( res != FUNC_RETURN_OK )
			{
				elog(WARNING, "Resource manager cannot find user "INT64_FORMAT
							  " to drop.",
							  (int64_t)(request->RoleOID));
				goto exit;
			}
			elog(LOG, "Resource manager handles request drop role oid:%d, "
					  "roleName:%s",
					  request->RoleOID,
					  request->Name);
			break;
		}
		default:
		{
			Assert(0);
		}
    }

exit:
	/* Build response. */
	response.Result 	= res;
	response.Reserved 	= 0;

	/* Build message saved in the connection track instance. */
	buildResponseIntoConnTrack(conntrack,
							   (char *)&response,
							   sizeof(response),
							   conntrack->MessageMark1,
							   conntrack->MessageMark2,
							   RESPONSE_QD_DDL_MANIPULATEROLE);
	conntrack->ResponseSent = false;
	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	PCONTRACK->ConnToSend = lappend(PCONTRACK->ConnToSend, conntrack);
	MEMORY_CONTEXT_SWITCH_BACK

	return true;
}

int updateResqueueCatalog(int					 action,
					      DynResourceQueueTrack  queuetrack,
						  List					*rsqattr)
{
	int res = FUNC_RETURN_OK;

	switch( action )
	{
	case MANIPULATE_RESQUEUE_CREATE:
	{
		List *insertaction = NULL;
		Oid	  newoid	   = InvalidOid;
		res = buildInsertActionForPGResqueue(queuetrack->QueueInfo,
											 rsqattr,
											 &insertaction);
		Assert(res == FUNC_RETURN_OK);

		res = performInsertActionForPGResqueue(insertaction, &newoid);
		if(res != FUNC_RETURN_OK)
		{
			elog(WARNING, "Resource manager performs insert operation on "
						  "pg_resqueue failed : %d",
						  res);

			DRMGlobalInstance->ResManagerMainKeepRun = false;
			break;
		}
		Assert(newoid != InvalidOid);

		/* Update queue as new oid and make it indexed by new oid. */
		queuetrack->QueueInfo->OID = newoid;
		setQueueTrackIndexedByQueueOID(queuetrack);

		/* Update the status of the parent queue when the status is leaf. */
		DynResourceQueueTrack parenttrack = queuetrack->ParentTrack;
		char *parentname = parenttrack->QueueInfo->Name;
		/* don't update pg_root */
		if(strcmp(parentname, RESOURCE_QUEUE_ROOT_QUEUE_NAME) != 0)
		{
			List *updateaction 	= NULL;
			List *updateattr	= NULL;
			/*construct the update attr list */
			KVProperty statusattr = createPropertyString(
										PCONTEXT,
										NULL,
										getRSQTBLAttributeName(RSQ_TBL_ATTR_STATUS),
										NULL,
										"branch");
			MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
			updateattr = lappend(updateattr, statusattr);
			MEMORY_CONTEXT_SWITCH_BACK

			res = buildUpdateStatusActionForPGResqueue(parenttrack->QueueInfo,
												       updateattr,
												       &updateaction);
			Assert(res == FUNC_RETURN_OK);
			res = performUpdateActionForPGResqueue(updateaction, parentname);
			if(res != FUNC_RETURN_OK)
			{
				elog(WARNING, "Resource manager updates the status of the parent "
							  "resource queue %s failed when create resource "
							  "queue %s",
							  parenttrack->QueueInfo->Name,
							  queuetrack->QueueInfo->Name);
				DRMGlobalInstance->ResManagerMainKeepRun = false;
			}

			cleanPropertyList(PCONTEXT, &updateattr);
			freeUpdateActionList(PCONTEXT, &updateaction);
		}
		break;
	}
	case MANIPULATE_RESQUEUE_ALTER:
	{
		char *queuename = queuetrack->QueueInfo->Name;
		List *updateaction = NULL;
		res = buildUpdateActionForPGResqueue(queuetrack->QueueInfo,
											 rsqattr,
											 &updateaction);
		Assert(res == FUNC_RETURN_OK);

		res = performUpdateActionForPGResqueue(updateaction, queuename);
		if(res != FUNC_RETURN_OK)
		{
			elog(WARNING, "Resource manager performs update operation on "
						  "pg_resqueue failed when update resource queue %s",
						  queuename);
			DRMGlobalInstance->ResManagerMainKeepRun = false;
		}
		freeUpdateActionList(PCONTEXT, &updateaction);
		break;
	}
	case MANIPULATE_RESQUEUE_DROP:
	{
		char *queuename = queuetrack->QueueInfo->Name;
		res = performDeleteActionForPGResqueue(queuename);
		if(res != FUNC_RETURN_OK)
		{
			elog(WARNING, "Resource manager performs delete operation on "
						  "pg_resqueue failed when drop resource queue %s.",
						  queuename);
			DRMGlobalInstance->ResManagerMainKeepRun = false;
			break;
		}

		/* update the status of the parent queue after the child is removed */
		DynResourceQueueTrack parenttrack = queuetrack->ParentTrack;
		char *parentname = parenttrack->QueueInfo->Name;
		if (list_length(queuetrack->ParentTrack->ChildrenTracks) == 1 )
		{
			List *updateaction = NULL;
			List *updateattr   = NULL;
			/*construct the update attr list to 'branch' status. */
			KVProperty statusattr = createPropertyString(
										PCONTEXT,
										NULL,
										getRSQTBLAttributeName(RSQ_TBL_ATTR_STATUS),
										NULL,
										"");
			MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
			updateattr = lappend(updateattr, statusattr);
			MEMORY_CONTEXT_SWITCH_BACK

			res = buildUpdateStatusActionForPGResqueue(parenttrack->QueueInfo,
													   updateattr,
													   &updateaction);

			Assert(res == FUNC_RETURN_OK);
			res = performUpdateActionForPGResqueue(updateaction, parentname);
			if(res != FUNC_RETURN_OK)
			{
				elog(WARNING, "Resource manager updates the status of the parent "
							  "resource queue %s failed when drop resource queue %s",
							  parenttrack->QueueInfo->Name,
							  queuetrack->QueueInfo->Name);
				DRMGlobalInstance->ResManagerMainKeepRun = false;
			}
			cleanPropertyList(PCONTEXT, &updateattr);
			freeUpdateActionList(PCONTEXT, &updateaction);
		}
		break;
	}
	default:
	{
		Assert(false);
	}
	}
	return res;
}



/*******************************************************************************
 * Build response message for successfully adding new resource queue.
 *
 * Response format (SUCCEED):
 * 		uint32_t		return code
 * 		uint8_t			action count
 * 		uint8_t			reserved[3]
 *
 * 		uint8_t			action code (1=create,2=alter,3=drop)
 * 		uint8_t 		column count
 * 		uint8_t			reserved[2]
 * 		int64_t			queue oid
 * 		uint8_t			column index x column count
 *		column new value \0 column new value \0 ...
 *		append multiple \0  to make 64-bit aligned.
 ******************************************************************************/

#define ADD_PG_RESQUEUE_COLVALUE_CONSTSTR(list,colval,colidx)			   	   \
	{																		   \
		PAIR _pair = createPAIR(PCONTEXT, NULL, NULL);						   \
		_pair->Key = TYPCONVERT(void *, colidx);							   \
		_pair->Value = createSimpleString(PCONTEXT);						   \
		setSimpleStringNoLen((SimpStringPtr)(_pair->Value), (char *)colval);   \
		*list = lappend(*list, _pair);									   	   \
	}

#define ADD_PG_RESQUEUE_COLVALUE_OID(list,colval,colidx)			   	   	   \
	{																		   \
		PAIR _pair = createPAIR(PCONTEXT, NULL, NULL);						   \
		_pair->Key = TYPCONVERT(void *, colidx);							   \
		_pair->Value = createSimpleString(PCONTEXT);						   \
		Oid _oid = colval;													   \
		SimpleStringSetOid((SimpStringPtr)(_pair->Value), _oid);		   	   \
		*list = lappend(*list, _pair);								   	   	   \
	}

#define ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(list,ddlattr,ddlidx,colidx)		   \
	{																		   \
		SimpStringPtr _colvalue = NULL;										   \
		if ( findPropertyValue(											   	   \
				ddlattr, 													   \
				getRSQDDLAttributeName(ddlidx),					   		   	   \
				&_colvalue) == FUNC_RETURN_OK ) {							   \
			PAIR _pair = createPAIR(PCONTEXT, NULL, NULL);					   \
			_pair->Key = TYPCONVERT(void *, colidx);						   \
			_pair->Value = createSimpleString(PCONTEXT);					   \
			SimpleStringCopy((SimpStringPtr)(_pair->Value), _colvalue);		   \
			*list = lappend(*list, _pair);								   	   \
		}																	   \
	}

#define ADD_PG_RESQUEUE_COLVALUE_INATTR(list,ddlattr,ddlidx,colidx)			   \
	{																		   \
		SimpStringPtr _colvalue = NULL;										   \
		if ( findPropertyValue(											   	   \
				ddlattr, 													   \
				getRSQTBLAttributeName(ddlidx),					   			   \
				&_colvalue) == FUNC_RETURN_OK ) {							   \
			PAIR _pair = createPAIR(PCONTEXT, NULL, NULL);					   \
			_pair->Key = TYPCONVERT(void *, colidx);						   \
			_pair->Value = createSimpleString(PCONTEXT);					   \
			SimpleStringCopy((SimpStringPtr)(_pair->Value), _colvalue);		   \
			*list = lappend(*list, _pair);								   	   \
		}																	   \
	}

int buildInsertActionForPGResqueue(DynResourceQueue   queue,
							   	   List 			 *rsqattr,
							   	   List		   		**insvalues)
{
	Assert( queue     != NULL );
	Assert( rsqattr   != NULL );
	Assert( insvalues != NULL );

	int	 res				 	  = FUNC_RETURN_OK;
	char defaultActiveStats[] 	  = DEFAULT_RESQUEUE_ACTIVESTATS;
	char defaultUpperFactor[] 	  = DEFAULT_RESQUEUE_UPPERFACTOR;
	char defaultVSegUpperFactor[] = DEFAULT_RESQUEUE_VSEG_UPPER_LIMIT;
	char defaultPolicy[]		  = DEFAULT_RESQUEUE_POLICY;
	char defaultSegQuota[]		  = DEFAULT_RESQUEUE_SEG_QUOTA;
	PAIR newpair				  = NULL;

	/* Insert resource queue column value. */
	newpair = createPAIR(PCONTEXT,
						 TYPCONVERT(void *, Anum_pg_resqueue_rsqname),
						 createSimpleString(PCONTEXT));

	setSimpleStringWithContent((SimpStringPtr)(newpair->Value),
							   queue->Name,
							   queue->NameLen);

	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)

	*insvalues = lappend(*insvalues, newpair);

	/* Default value for rsq_active_stats_cluster if not set */
	SimpStringPtr colvalue = NULL;
	if (findPropertyValue(
					rsqattr,
					getRSQDDLAttributeName(RSQ_DDL_ATTR_ACTIVE_STATMENTS),
					&colvalue) != FUNC_RETURN_OK)
	{
		ADD_PG_RESQUEUE_COLVALUE_CONSTSTR(insvalues, defaultActiveStats, Anum_pg_resqueue_rsq_active_stats_cluster);
	}
	/* Default value for rsq_resource_upper_factor if not set */
	if (findPropertyValue(
					rsqattr,
					getRSQDDLAttributeName(RSQ_DDL_ATTR_RESOURCE_UPPER_FACTOR),
					&colvalue) != FUNC_RETURN_OK)
	{
		ADD_PG_RESQUEUE_COLVALUE_CONSTSTR(insvalues, defaultUpperFactor, Anum_pg_resqueue_rsq_resource_upper_factor);
	}

	/* Default value for rsq_vseg_upper_limit if not set */
	if (findPropertyValue(
					rsqattr,
					getRSQDDLAttributeName(RSQ_DDL_ATTR_VSEGMENT_UPPER_LIMIT),
					&colvalue) != FUNC_RETURN_OK)
	{
		ADD_PG_RESQUEUE_COLVALUE_CONSTSTR(insvalues, defaultVSegUpperFactor, Anum_pg_resqueue_rsq_vseg_upper_limit);
	}

	/* Default value for rsq_allocation_policy if not set */
	if (findPropertyValue(
					rsqattr,
					getRSQDDLAttributeName(RSQ_DDL_ATTR_ALLOCATION_POLICY),
					&colvalue) != FUNC_RETURN_OK)
	{
		ADD_PG_RESQUEUE_COLVALUE_CONSTSTR(insvalues, defaultPolicy, Anum_pg_resqueue_rsq_allocation_policy);
	}

	/* Default value for rsq_vseg_resource_quota if not set */
	if (findPropertyValue(
					rsqattr,
					getRSQDDLAttributeName(RSQ_DDL_ATTR_VSEGMENT_RESOURCE_QUOTA),
					&colvalue) != FUNC_RETURN_OK)
	{
		ADD_PG_RESQUEUE_COLVALUE_CONSTSTR(insvalues, defaultSegQuota, Anum_pg_resqueue_rsq_vseg_resource_quota);
	}

	ADD_PG_RESQUEUE_COLVALUE_OID(insvalues, queue->ParentOID, Anum_pg_resqueue_rsq_parent);

	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(insvalues, rsqattr, RSQ_DDL_ATTR_ACTIVE_STATMENTS, 		 Anum_pg_resqueue_rsq_active_stats_cluster);
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(insvalues, rsqattr, RSQ_DDL_ATTR_MEMORY_LIMIT_CLUSTER, 	 Anum_pg_resqueue_rsq_memory_limit_cluster);
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(insvalues, rsqattr, RSQ_DDL_ATTR_CORE_LIMIT_CLUSTER, 	 Anum_pg_resqueue_rsq_core_limit_cluster);
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(insvalues, rsqattr, RSQ_DDL_ATTR_RESOURCE_UPPER_FACTOR, 	 Anum_pg_resqueue_rsq_resource_upper_factor);
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(insvalues, rsqattr, RSQ_DDL_ATTR_VSEGMENT_UPPER_LIMIT, 	 Anum_pg_resqueue_rsq_vseg_upper_limit);
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(insvalues, rsqattr, RSQ_DDL_ATTR_ALLOCATION_POLICY, 		 Anum_pg_resqueue_rsq_allocation_policy);
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(insvalues, rsqattr, RSQ_DDL_ATTR_VSEGMENT_RESOURCE_QUOTA, Anum_pg_resqueue_rsq_vseg_resource_quota);

	/* creation time and update time */
	TimestampTz curtime    = GetCurrentTimestamp();
	const char *curtimestr = timestamptz_to_str(curtime);

	ADD_PG_RESQUEUE_COLVALUE_CONSTSTR(insvalues, curtimestr, Anum_pg_resqueue_rsq_creation_time);
	ADD_PG_RESQUEUE_COLVALUE_CONSTSTR(insvalues, curtimestr, Anum_pg_resqueue_rsq_update_time);

	/* status */
	char statusstr[256];
	statusstr[0] = '\0';
	if ( RESQUEUE_IS_BRANCH(queue) )
		strcat(statusstr, "branch");

	ADD_PG_RESQUEUE_COLVALUE_CONSTSTR(insvalues, statusstr, Anum_pg_resqueue_rsq_status);

	MEMORY_CONTEXT_SWITCH_BACK
	return res;
}

int buildUpdateActionForPGResqueue(DynResourceQueue   queue,
							   	   List 		   	 *rsqattr,
							   	   List		   	    **updvalues)
{
	int res = FUNC_RETURN_OK;
	/* Insert resource queue column value. */
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(updvalues, rsqattr, RSQ_DDL_ATTR_ACTIVE_STATMENTS, 		 Anum_pg_resqueue_rsq_active_stats_cluster);
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(updvalues, rsqattr, RSQ_DDL_ATTR_MEMORY_LIMIT_CLUSTER, 	 Anum_pg_resqueue_rsq_memory_limit_cluster);
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(updvalues, rsqattr, RSQ_DDL_ATTR_CORE_LIMIT_CLUSTER, 	 Anum_pg_resqueue_rsq_core_limit_cluster);
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(updvalues, rsqattr, RSQ_DDL_ATTR_RESOURCE_UPPER_FACTOR, 	 Anum_pg_resqueue_rsq_resource_upper_factor);
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(updvalues, rsqattr, RSQ_DDL_ATTR_ALLOCATION_POLICY, 		 Anum_pg_resqueue_rsq_allocation_policy);
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(updvalues, rsqattr, RSQ_DDL_ATTR_VSEGMENT_RESOURCE_QUOTA, Anum_pg_resqueue_rsq_vseg_resource_quota);
	ADD_PG_RESQUEUE_COLVALUE_INDDLATTR(updvalues, rsqattr, RSQ_DDL_ATTR_VSEGMENT_UPPER_LIMIT, 	 Anum_pg_resqueue_rsq_vseg_upper_limit);

	/* creation time and update time */
	TimestampTz curtime = GetCurrentTimestamp();
	const char *curtimestr = timestamptz_to_str(curtime);
	ADD_PG_RESQUEUE_COLVALUE_CONSTSTR(updvalues, curtimestr, Anum_pg_resqueue_rsq_update_time);

	/* status */
	char statusstr[256];
	statusstr[0] = '\0';
	if ( RESQUEUE_IS_BRANCH(queue) )
		strcat(statusstr, "branch");

	ADD_PG_RESQUEUE_COLVALUE_CONSTSTR(updvalues, statusstr, Anum_pg_resqueue_rsq_status);
	return res;
}


int buildUpdateStatusActionForPGResqueue(DynResourceQueue   queue,
								  	  	 List 		   	   *rsqattr,
								  	  	 List		   	  **updvalues)
{
	int res = FUNC_RETURN_OK;

	ListCell *cell = NULL;
	foreach(cell, rsqattr)
	{
		KVProperty attribute = lfirst(cell);
		elog(DEBUG3, "Received update Request: %s=%s",
					 attribute->Key.Str,
					 attribute->Val.Str);
	}

	/* update time */
	TimestampTz curtime = GetCurrentTimestamp();
	const char *curtimestr = timestamptz_to_str(curtime);

	ADD_PG_RESQUEUE_COLVALUE_CONSTSTR(updvalues,curtimestr, Anum_pg_resqueue_rsq_update_time);

	/* status */
	ADD_PG_RESQUEUE_COLVALUE_INATTR(updvalues, rsqattr, RSQ_TBL_ATTR_STATUS, Anum_pg_resqueue_rsq_status);
	return res;
}

int performInsertActionForPGResqueue(List *colvalues, Oid *newoid)
{
	Assert(newoid != NULL);
	Assert(colvalues != NULL);

	int 		res 	 = FUNC_RETURN_OK;
	int			libpqres = CONNECTION_OK;
	PGconn 	   *conn 	 = NULL;
	char 		conninfo[512];
	PQExpBuffer sql 	 = NULL;
	int 		colcnt 	 = 0;
	PGresult   *result 	 = NULL;
	char 		name[65];
	ListCell   *cell	 = NULL;

	sprintf(conninfo, "options='-c gp_session_role=UTILITY "
					           "-c allow_system_table_mods=dml' "
					  "dbname=template1 port=%d connect_timeout=%d",
					  master_addr_port,
					  LIBPQ_CONNECT_TIMEOUT);

	conn = PQconnectdb(conninfo);
	if ((libpqres = PQstatus(conn)) != CONNECTION_OK)
	{
		elog(WARNING, "Resource manager failed to connect database when insert "
					  "row into pg_resqueue, error code: %d, reason: %s",
				      libpqres,
				      PQerrorMessage(conn));
		PQfinish(conn);
		return LIBPQ_FAIL_EXECUTE;
	}

	result = PQexec(conn, "BEGIN");
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Resource manager failed to run SQL: %s when insert row "
					  "into pg_resqueue, reason: %s",
				      "BEGIN",
				      PQresultErrorMessage(result));
		res = LIBPQ_FAIL_EXECUTE;
		goto cleanup;
	}

	PQclear(result);

	/**
	 * compose an INSERT sql statement
	 */
	sql = createPQExpBuffer();
	if ( sql == NULL )
	{
		elog(WARNING, "Resource manager failed to allocate buffer for building "
					  "sql statement.");
		goto cleanup;
	}
	appendPQExpBuffer(sql, "%s", "INSERT INTO pg_resqueue(");
	colcnt = list_length(colvalues);
	memset(name, 0, sizeof(name));

	foreach(cell, colvalues)
	{
		PAIR action = lfirst(cell);
		int colindex = TYPCONVERT(int, action->Key);
		Assert(colindex <= Natts_pg_resqueue && colindex > 0);
		appendPQExpBuffer(sql, "%s", PG_Resqueue_Column_Names[colindex-1]);
		if ( colindex == Anum_pg_resqueue_rsqname )
		{
			strncpy(name,
					((SimpStringPtr)action->Value)->Str,
					sizeof(name)-1);
		}
		colcnt--;
		if (colcnt != 0)
		{
			appendPQExpBuffer(sql,"%s",",");
		}
	}
	appendPQExpBuffer(sql,"%s",") VALUES(");

	colcnt = list_length(colvalues);

	foreach(cell, colvalues)
	{
		PAIR action = lfirst(cell);
		SimpStringPtr valueptr = (SimpStringPtr)(action->Value);
		appendPQExpBuffer(sql, "'%s'", valueptr->Str);
		colcnt--;
		if (colcnt !=0 )
		{
			appendPQExpBuffer(sql,"%s",",");
		}
	}
	appendPQExpBuffer(sql,"%s",")");

	elog(LOG, "Resource manager created a new queue: %s", sql->data);
	result = PQexec(conn, sql->data);
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Resource manager failed to run SQL: %s failed "
				      "when insert row into pg_resqueue, reason : %s",
				      sql->data,
				      PQresultErrorMessage(result));
		res = LIBPQ_FAIL_EXECUTE;
		goto cleanup;
	}

	resetPQExpBuffer(sql);
	PQclear(result);
	appendPQExpBuffer(sql, "SELECT oid FROM pg_resqueue WHERE rsqname = '%s'", name);
	result = PQexec(conn, sql->data);
	if (!result || PQresultStatus(result) != PGRES_TUPLES_OK)
	{
		elog(WARNING, "Resource manager failed to run SQL: %s failed, reason : %s",
				      sql->data,
				      PQresultErrorMessage(result));
		PQexec(conn, "ABORT");
		res = LIBPQ_FAIL_EXECUTE;
		goto cleanup;
	}

	*newoid = (uint32) atoi(PQgetvalue(result, 0, 0));
	if(*newoid == InvalidOid)
	{
		elog(WARNING, "Resource manager gets an invalid oid after insert row "
					  "into pg_resqueue");
		PQexec(conn, "ABORT");
		res = LIBPQ_FAIL_EXECUTE;;
		goto cleanup;
	}

	PQclear(result);
	result = PQexec(conn, "COMMIT");
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Resource manager failed to run SQL: %s "
			      	  "when insert row into pg_resqueue, reason : %s",
					  "COMMIT",
					  PQresultErrorMessage(result));
		PQexec(conn, "ABORT");
		res = LIBPQ_FAIL_EXECUTE;
		goto cleanup;
	}
	elog(LOG, "Resource manager created a new resource queue, oid is: %d", *newoid);

cleanup:
	if(sql != NULL)
	{
		destroyPQExpBuffer(sql);
	}
	if(result != NULL)
	{
		PQclear(result);
	}

	PQfinish(conn);
	return res;
}

int performUpdateActionForPGResqueue(List *colvalues, char *queuename)
{
	int 		res 		= FUNC_RETURN_OK;
	int			libpqres 	= CONNECTION_OK;
	PGconn 	   *conn 		= NULL;
	char 		conninfo[512];
	PQExpBuffer sql 		= NULL;
	int 		colcnt 		= 0;
	PGresult   *result 		= NULL;
	ListCell   *cell		= NULL;

	sprintf(conninfo, "options='-c gp_session_role=UTILITY "
							   "-c allow_system_table_mods=dml' "
					  "dbname=template1 port=%d connect_timeout=%d",
					  master_addr_port,
					  LIBPQ_CONNECT_TIMEOUT);

	conn = PQconnectdb(conninfo);
	if ((libpqres = PQstatus(conn)) != CONNECTION_OK)
	{
		elog(WARNING, "Resource manager failed to connect database when update "
					  "row of pg_resqueue, error code: %d, reason: %s",
			      	  libpqres,
			      	  PQerrorMessage(conn));
		PQfinish(conn);
		return LIBPQ_FAIL_EXECUTE;
	}

	result = PQexec(conn, "BEGIN");
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Resource manager failed to run SQL: %s when update row "
					  "of pg_resqueue, reason : %s",
					  "BEGIN",
					  PQresultErrorMessage(result));
		res = LIBPQ_FAIL_EXECUTE;
		goto cleanup;
	}
	PQclear(result);

	/**
	 * compose an UPDATE sql statement
	 */
	sql = createPQExpBuffer();
	if ( sql == NULL )
	{
		elog(WARNING, "Resource manager failed to allocate buffer for building "
					  "sql statement.");
		goto cleanup;
	}
	appendPQExpBuffer(sql, "%s", "UPDATE pg_resqueue SET ");
	colcnt = list_length(colvalues);

	foreach(cell, colvalues)
	{
		PAIR action = lfirst(cell);
		int colindex = TYPCONVERT(int, action->Key);
		Assert(colindex <= Natts_pg_resqueue && colindex > 0);
		SimpStringPtr valueptr = (SimpStringPtr)(action->Value);
		appendPQExpBuffer(sql,
						  "%s='%s'",
						  PG_Resqueue_Column_Names[colindex-1],
						  valueptr->Str);
		colcnt--;
		if (colcnt != 0)
		{
			appendPQExpBuffer(sql,"%s",",");
		}
	}
	appendPQExpBuffer(sql, " WHERE rsqname='%s'", queuename);

	elog(LOG, "Resource manager updates resource queue: %s",sql->data);
	result = PQexec(conn, sql->data);
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Resource manager failed to run SQL: %s "
				  	  "when update row of pg_resqueue, reason : %s",
				  	  sql->data,
				  	  PQresultErrorMessage(result));
		res = LIBPQ_FAIL_EXECUTE;
		goto cleanup;
	}

	PQclear(result);
	result = PQexec(conn, "COMMIT");
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Resource manager failed to run SQL: %s "
				  	  "when update row of pg_resqueue, reason : %s",
					  "COMMIT",
					  PQresultErrorMessage(result));
		PQexec(conn, "ABORT");
		res = LIBPQ_FAIL_EXECUTE;
		goto cleanup;
	}
	elog(LOG, "Resource queue %s is updated", queuename);

cleanup:
	if(sql != NULL)
	{
		destroyPQExpBuffer(sql);
	}
	if(result != NULL)
	{
		PQclear(result);
	}
	PQfinish(conn);
	return res;
}

int performDeleteActionForPGResqueue(char *queuename)
{
	Assert(queuename != NULL);

	int res = FUNC_RETURN_OK;
	int	libpqres = CONNECTION_OK;
	PGconn *conn = NULL;
	char conninfo[1024];
	PQExpBuffer sql = NULL;
	PGresult* result = NULL;
	Oid queueid = InvalidOid;

	sprintf(conninfo, "options='-c gp_session_role=UTILITY -c allow_system_table_mods=dml' "
			"dbname=template1 port=%d connect_timeout=%d", master_addr_port, LIBPQ_CONNECT_TIMEOUT);
	conn = PQconnectdb(conninfo);
	if ((libpqres = PQstatus(conn)) != CONNECTION_OK) {
		elog(WARNING, "Resource manager failed to connect database when delete a row from pg_resqueue,"
		      	  	  "error code: %d, reason: %s",
		      	  	  libpqres,
		      	  	  PQerrorMessage(conn));
		res = libpqres;
		PQfinish(conn);
		return res;
	}

	result = PQexec(conn, "BEGIN");
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
		elog(WARNING, "Resource manager failed to run SQL: %s "
				  	  "when delete a row from pg_resqueue, reason : %s",
	      	  	  	  "BEGIN",
	      	  	  	  PQresultErrorMessage(result));
		res = PGRES_FATAL_ERROR;
		goto cleanup;
	}
	PQclear(result);

	/**
	 * firstly, get oid of this resource queue.
	 */
	sql = createPQExpBuffer();
	if ( sql == NULL )
	{
		elog(WARNING, "Resource manager failed to allocate buffer for building "
					  "sql statement.");
		goto cleanup;
	}
	appendPQExpBuffer(sql, "SELECT oid FROM pg_resqueue WHERE rsqname = '%s'", queuename);
	result = PQexec(conn, sql->data);
	if (!result || PQresultStatus(result) != PGRES_TUPLES_OK) {
		elog(WARNING, "Resource manager failed to run SQL: %s "
			  	  	  "when delete a row from pg_resqueue, reason : %s",
					  sql->data,
					  PQresultErrorMessage(result));
		res = PGRES_FATAL_ERROR;
		goto cleanup;
	}
	queueid = (uint32) atoi(PQgetvalue(result, 0, 0));
	if(queueid == InvalidOid) {
		elog(WARNING, "Resource manager gets an invalid oid when delete a row from pg_resqueue");
		res = PGRES_FATAL_ERROR;
		goto cleanup;
	}

	/*
	 * Drop resource queue
	 */
	PQclear(result);
	resetPQExpBuffer(sql);
	appendPQExpBuffer(sql, "DELETE FROM pg_resqueue WHERE rsqname = '%s'", queuename);
	elog(LOG, "Resource manager drops a resource queue: %s",sql->data);
	result = PQexec(conn, sql->data);
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
		elog(WARNING, "Resource manager failed to run SQL: %s "
		  	  	  	  "when delete a row from pg_resqueue, reason : %s",
					  sql->data,
					  PQresultErrorMessage(result));
		res = PGRES_FATAL_ERROR;
		goto cleanup;
	}

	/*
	 * Remove any comments on this resource queue
	 */
	PQclear(result);
	resetPQExpBuffer(sql);
	appendPQExpBuffer(sql,
					  "DELETE FROM pg_shdescription WHERE objoid  = %d AND classoid = %d",
					  queueid, ResQueueRelationId);
	result = PQexec(conn, sql->data);
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
		elog(WARNING, "Resource manager failed to run SQL: %s "
		  	  	  	  "when delete a row from pg_resqueue, reason : %s",
					  sql->data,
					  PQresultErrorMessage(result));

	/* MPP-6929, MPP-7583: metadata tracking */
	PQclear(result);
	resetPQExpBuffer(sql);
	appendPQExpBuffer(sql,
				      "DELETE FROM pg_stat_last_shoperation WHERE classid = %d AND objid = %d ",
				      ResQueueRelationId, queueid);
	result = PQexec(conn, sql->data);
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
		elog(WARNING, "Resource manager failed to run SQL: %s "
		  	  	  	  "when delete a row from pg_resqueue, reason : %s",
					  sql->data,
					  PQresultErrorMessage(result));

	/* MPP-6923: drop the extended attributes for this queue */
	PQclear(result);
	resetPQExpBuffer(sql);
	appendPQExpBuffer(sql,
					  "DELETE FROM pg_resqueuecapability WHERE resqueueid = %d",
					  queueid);
	result = PQexec(conn, sql->data);
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
		elog(WARNING, "Resource manager failed to run SQL: %s "
		  	  	  	  "when delete a row from pg_resqueue, reason : %s",
					  sql->data,
					  PQresultErrorMessage(result));

	PQclear(result);
	result = PQexec(conn, "COMMIT");
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
		elog(WARNING, "Resource manager failed to run SQL: %s "
		  	  	  	  "when delete a row from pg_resqueue, reason : %s",
					  "COMMIT",
					  PQresultErrorMessage(result));
		PQexec(conn, "ABORT");
		res = PGRES_FATAL_ERROR;
		goto cleanup;
	}

	elog(LOG, "Resource queue %s is dropped", queuename);

cleanup:
	if(sql != NULL)
	{
		destroyPQExpBuffer(sql);
	}
	if(result != NULL)
	{
		PQclear(result);
	}
	PQfinish(conn);
	return res;
}

/*******************************************************************************
 * Recognize the column value based on column index and convert string format
 * value into Datum instance for caql operations.
 ******************************************************************************/
Datum getDatumFromStringValuForPGResqueue(int	 colindex,
										  char  *colvaluestr)
{
	switch(colindex) {
	case Anum_pg_resqueue_rsqname:
		/* Set value as name format */
		return DirectFunctionCall1(namein, CStringGetDatum(colvaluestr));


	case Anum_pg_resqueue_rsq_creation_time:
	case Anum_pg_resqueue_rsq_update_time:
		return 0;

	case Anum_pg_resqueue_rsq_memory_limit_cluster:
	case Anum_pg_resqueue_rsq_core_limit_cluster:
	case Anum_pg_resqueue_rsq_allocation_policy:
	case Anum_pg_resqueue_rsq_vseg_resource_quota:
	case Anum_pg_resqueue_rsq_status:
		/* Set value as text format */
		return DirectFunctionCall1(textin, CStringGetDatum(colvaluestr));

	case Anum_pg_resqueue_rsq_active_stats_cluster:
	{
		int32_t tmpvalue;
		sscanf(colvaluestr, "%d", &tmpvalue);
		return Int32GetDatum(tmpvalue);
	}
	case Anum_pg_resqueue_rsq_parent:
	{
		int64_t tmpoid;
		Oid 	parentoid;
		sscanf(colvaluestr, INT64_FORMAT, &tmpoid);
		parentoid = tmpoid;
		return ObjectIdGetDatum(parentoid);
	}
	Assert(false);
	}
	return 0;
}

void freeUpdateActionList(MCTYPE context, List **actions)
{
	while( list_length(*actions) > 0 )
	{
		PAIR pair = lfirst(list_head(*actions));
		MEMORY_CONTEXT_SWITCH_TO(context)
		*actions = list_delete_first(*actions);
		MEMORY_CONTEXT_SWITCH_BACK

		SimpStringPtr content = (SimpStringPtr)(pair->Value);
		freeSimpleStringContent(content);
		rm_pfree(context, pair);
	}
}

