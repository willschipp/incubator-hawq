#include "envswitch.h"
#include "dynrm.h"
#include "include/communication/rmcomm_RM2GRM.h"
#include "include/communication/rmcomm_RM2RMSEG.h"
#include "utils/simplestring.h"
#include "utils/network_utils.h"
#include "resourcepool.h"
#include "gp-libpq-fe.h"
#include "gp-libpq-int.h"

void addSegResourceAvailIndex(SegResource segres);
void addSegResourceAllocIndex(SegResource segres);
void addSegResourceIOBytesWorkloadIndex(SegResource segres);
int  reorderSegResourceAvailIndex(SegResource segres, uint32_t ratio);
int  reorderSegResourceAllocIndex(SegResource segres, uint32_t ratio);
int  reorderSegResourceIOBytesWorkloadIndex(SegResource segres);

int allocateResourceFromSegment(SegResource 	segres,
							 	GRMContainerSet ctns,
								int32_t			memory,
								double 			core,
								int32_t			slicesize);

int recycleResourceToSegment(SegResource	 segres,
						  	 GRMContainerSet ctns,
							 int32_t		 memory,
							 double			 core,
							 int64_t		 iobytes,
							 int32_t		 slicesize);

int getSegIDByHostNameInternal(HASHTABLE   hashtable,
							   const char *hostname,
							   int 		   hostnamelen,
							   int32_t    *id);

void timeoutIdleGRMResourceToRBByRatio(int 		 ratioindex,
								       uint32_t	 retcontnum,
									   uint32_t *realretcontnum,
									   int		 segminnum);

void getSegResResourceCountersByMemCoreCounters(SegResource  resinfo,
												int32_t		*allocmem,
												double		*alloccore,
												int32_t		*availmem,
												double		*availcore);

VSegmentCounterInternal createVSegmentCounter(uint32_t 		hdfsnameindex,
											  SegResource	segres);

/* Functions for BBST indices. */
int __DRM_NODERESPOOL_comp_ratioFree(void *arg, void *val1, void *val2);
int __DRM_NODERESPOOL_comp_ratioAlloc(void *arg, void *val1, void *val2);
int __DRM_NODERESPOOL_comp_iobytes(void *arg, void *val1, void *val2);

/*
 * The balanced BST index comparing function. The segment containing most
 * available resource is ordered at the left most, the segment not in
 * available status is treated always the minimum.
 */
int __DRM_NODERESPOOL_comp_ratioFree(void *arg, void *val1, void *val2)
{
	SegResource 	node1 	= (SegResource) val1;
	SegResource 	node2 	= (SegResource) val2;
	GRMContainerSet contset	= NULL;

	uint32_t 		ratio  	= TYPCONVERT(uint32_t,arg);
	int32_t  		v1 		= INT32_MIN;
	int32_t  		v2 		= INT32_MIN;
	int      		ridx   	= getResourceQueueRatioIndex(ratio);

	if ( IS_SEGRESOURCE_USABLE(node1) )
	{
		contset = ridx < 0 ? NULL : node1->ContainerSets[ridx];
		v1 = contset != NULL ? contset->Available.MemoryMB : 0;
	}

	if ( IS_SEGRESOURCE_USABLE(node2) )
	{
		contset = ridx < 0 ? NULL : node2->ContainerSets[ridx];
		v2 = contset != NULL ? contset->Available.MemoryMB : 0;
    }

	/* Expect the maximum one is at the left most. */
	return v2>v1 ? 1 : ( v1==v2 ? 0 : -1);
}

/*
 * The balanced BST index comparing function. The segment containing most
 * allocated resource is ordered at the left most, the segment not in
 * available status is treated always the minimum.
 */
int __DRM_NODERESPOOL_comp_ratioAlloc(void *arg, void *val1, void *val2)
{
	SegResource 	node1 	= (SegResource) val1;
	SegResource 	node2 	= (SegResource) val2;
	GRMContainerSet contset	= NULL;

	uint32_t 		ratio  	= TYPCONVERT(uint32_t,arg);
	int32_t  		v1 		= INT32_MIN;
	int32_t  		v2 		= INT32_MIN;
	int      		ridx   	= getResourceQueueRatioIndex(ratio);

	if ( IS_SEGRESOURCE_USABLE(node1) )
	{
		contset = ridx < 0 ? NULL : node1->ContainerSets[ridx];
		v1 = contset != NULL ? contset->Allocated.MemoryMB : 0;
	}

	if ( IS_SEGRESOURCE_USABLE(node2) )
	{
		contset = ridx < 0 ? NULL : node2->ContainerSets[ridx];
		v2 = contset != NULL ? contset->Allocated.MemoryMB : 0;
    }

	/* Expect the maximum one is at the left most. */
	return v2>v1 ? 1 : ( v1==v2 ? 0 : -1);
}

/*
 * The balanced BST index comparing function. The segment containing fewest
 * io bytes workload is ordered at the left most, the segment not in available
 * status is treated always the minimum.
 */
int __DRM_NODERESPOOL_comp_iobytes(void *arg, void *val1, void *val2)
{
	SegResource 	node1 	= (SegResource) val1;
	SegResource 	node2 	= (SegResource) val2;

	int64_t v1 = IS_SEGRESOURCE_USABLE(node1) ? node1->IOBytesWorkload : INT32_MIN;
	int64_t v2 = IS_SEGRESOURCE_USABLE(node2) ? node2->IOBytesWorkload : INT32_MIN;

	/* Expect the minimum one is at the left most. */
	return v1>v2 ? 1 : ( v1==v2 ? 0 : -1);
}

int  getSegInfoHostAddrStr (SegInfo seginfo, int addrindex, AddressString *addr)
{
	Assert(seginfo != NULL);
	Assert(addrindex >= 0 && addrindex < seginfo->HostAddrCount);
	*addr = (AddressString)((char *)seginfo +
						 	GET_SEGINFO_ADDR_OFFSET_AT(seginfo, addrindex));
	return FUNC_RETURN_OK;
}

int  findSegInfoHostAddrStr(SegInfo seginfo, AddressString addr, int *addrindex)
{
	Assert( seginfo != NULL);
	for ( int i = 0 ; i < seginfo->HostAddrCount ; ++i )
	{
		AddressString oldaddr = NULL;
		getSegInfoHostAddrStr(seginfo, i, &oldaddr);
		Assert( addr != NULL );
		if ( AddressStringComp(oldaddr, addr) )
		{
			*addrindex = i;
			return FUNC_RETURN_OK;
		}
	}
	return FUNC_RETURN_FAIL;
}

void getBufferedHostName(char *hostname, char **buffhostname)
{
	int				 length		= -1;
	char			*newstring	= NULL;
	PAIR 			 pair 		= NULL;
	SimpString		 hostnamestr;

	setSimpleStringRefNoLen(&hostnamestr, (char *)hostname);
	pair = getHASHTABLENode(&(PRESPOOL->BufferedHostNames), &hostnamestr);
	if ( pair != NULL )
	{
		*buffhostname = (char *)(pair->Value);
		elog(DEBUG3, "Resource manager gets hostname %s from hostname buffer as %s.",
					 hostname,
					 *buffhostname);
	}
	else
	{
		/* create new buffered host name string and return */
		length = strlen(hostname);
		newstring = (char *)rm_palloc0(PCONTEXT, length+1);
		strcpy(newstring, hostname);
		void *oldval = setHASHTABLENode(&(PRESPOOL->BufferedHostNames),
						 	 	 	 	(void *)&hostnamestr,
										(void *)newstring,
										false /* No need to free old value. */);
		if( oldval != NULL )
		{
			Assert(false);
		}
		*buffhostname = newstring;
		elog(DEBUG3, "Resource manager adds new hostname %s to hostname buffer. "
					 "Current hostname buffer size %d",
					 hostname,
					 PRESPOOL->BufferedHostNames.NodeCount);
	}
}

GRMContainer createGRMContainer(uint32_t	 id,
                                int32_t    	 memory,
								double	 	 core,
								char      	*hostname,
								SegResource  segres)
{
	GRMContainer container = (GRMContainer)
							 rm_palloc0(PCONTEXT, sizeof(GRMContainerData));
	Assert(container != NULL);
	container->ID       	  = id;
	container->Core 		  = core;
	container->MemoryMB 	  = memory;
	container->Life     	  = 0;
	container->CalcDecPending = false;
	container->Resource       = segres;
	getBufferedHostName(hostname, &(container->HostName));
	Assert( container->HostName != NULL );
	return container;
}

void freeGRMContainer(GRMContainer ctn)
{
	rm_pfree(PCONTEXT, ctn);
}

/*
 *------------------------------------------------------------------------------
 * Resource pool APIs.
 *------------------------------------------------------------------------------
 */

/* Initialize the node resource manager. */
void initializeResourcePoolManager(void)
{
	PRESPOOL->SegmentIDCounter		= 0;

    initializeHASHTABLE(&(PRESPOOL->Segments),
						PCONTEXT,
						HASHTABLE_SLOT_VOLUME_DEFAULT,
						HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
						HASHTABLE_KEYTYPE_UINT32,
						NULL); /* Never free the data in it. */

	initializeHASHTABLE(&(PRESPOOL->SegmentHostNameIndexed),
						PCONTEXT,
                        HASHTABLE_SLOT_VOLUME_DEFAULT,
                        HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
                        HASHTABLE_KEYTYPE_SIMPSTR,
                        NULL); /* Never free the data in it. */

	initializeHASHTABLE(&(PRESPOOL->SegmentHostAddrIndexed),
						PCONTEXT,
                        HASHTABLE_SLOT_VOLUME_DEFAULT,
                        HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
                        HASHTABLE_KEYTYPE_CHARARRAY,
                        NULL); /* Never free the data in it. */

	initializeHASHTABLE(&(PRESPOOL->BufferedHostNames),
						PCONTEXT,
                        HASHTABLE_SLOT_VOLUME_DEFAULT,
                        HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
                        HASHTABLE_KEYTYPE_SIMPSTR,
                        NULL); /* Never free the data in it. */

	PRESPOOL->AvailNodeCount		= 0;

	resetResourceBundleData(&(PRESPOOL->FTSTotal), 0, 0, 0);
	resetResourceBundleData(&(PRESPOOL->GRMTotal), 0, 0, 0);

    PRESPOOL->LastUpdateTime        = 0;
    PRESPOOL->LastRequestTime       = 0;
    PRESPOOL->LastCheckTime   		= 0;
    PRESPOOL->LastResAcqTime		= 0;

    PRESPOOL->LastCheckContainerTime   = 0;
    PRESPOOL->LastRequestContainerTime = 0;

    for ( int i = 0 ; i < RESOURCE_QUEUE_RATIO_SIZE ; ++i )
    {
    	PRESPOOL->OrderedSegResAvailByRatio[i] = NULL;
    	PRESPOOL->OrderedSegResAllocByRatio[i] = NULL;
    }

	initializeBBST(&(PRESPOOL->OrderedIOBytesWorkload),
				   PCONTEXT,
				   NULL,
				   __DRM_NODERESPOOL_comp_iobytes);

	initializeHASHTABLE(&(PRESPOOL->HDFSHostNameIndexed),
						PCONTEXT,
						HASHTABLE_SLOT_VOLUME_DEFAULT,
						HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
						HASHTABLE_KEYTYPE_SIMPSTR,
						NULL);

	initializeHASHTABLE(&(PRESPOOL->GRMHostNameIndexed),
						PCONTEXT,
						HASHTABLE_SLOT_VOLUME_DEFAULT,
						HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
						HASHTABLE_KEYTYPE_SIMPSTR,
						NULL);

	PRESPOOL->MemCoreRatio 				  = 0;
	PRESPOOL->MemCoreRatioMajorityCounter = 0;


	initializeHASHTABLE(&(PRESPOOL->ToAcceptContainers),
						PCONTEXT,
						HASHTABLE_SLOT_VOLUME_DEFAULT,
						HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
						HASHTABLE_KEYTYPE_SIMPSTR,
						NULL);

	initializeHASHTABLE(&(PRESPOOL->ToKickContainers),
						PCONTEXT,
						HASHTABLE_SLOT_VOLUME_DEFAULT,
						HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
						HASHTABLE_KEYTYPE_SIMPSTR,
						NULL);

	PRESPOOL->AcceptedContainers = NULL;
	PRESPOOL->KickedContainers   = NULL;

	PRESPOOL->AddPendingContainerCount = 0;
	PRESPOOL->RetPendingContainerCount = 0;

	/*
	 * initialize allocation policy function table.
	 */
	for ( int i = 0 ; i < RESOURCEPOOL_MAX_ALLOC_POLICY_SIZE ; ++i )
	{
		PRESPOOL->allocateResFuncs[i] = NULL;
	}
	PRESPOOL->allocateResFuncs[0] = allocateResourceFromResourcePoolIOBytes;
}

#define CONNECT_TIMEOUT 60
/*
 * Clean up gp_segment_configuration data including master and segment,
 * but won't delete standby, since standby is added by tool or manually.
 */
void cleanup_segment_config()
{
	int	libpqres = CONNECTION_OK;
	PGconn *conn = NULL;
	char conninfo[512];
	PQExpBuffer sql = NULL;
	PGresult* result = NULL;

	sprintf(conninfo, "options='-c gp_session_role=UTILITY -c allow_system_table_mods=dml' "
			"dbname=template1 port=%d connect_timeout=%d", master_addr_port, CONNECT_TIMEOUT);
	conn = PQconnectdb(conninfo);
	if ((libpqres = PQstatus(conn)) != CONNECTION_OK)
	{
		elog(WARNING, "Fail to connect database when cleanup "
				      "segment configuration catalog table, error code: %d, %s",
				      libpqres,
				      PQerrorMessage(conn));
		PQfinish(conn);
		return;
	}

	result = PQexec(conn, "BEGIN");
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Fail to run SQL: %s when cleanup "
				      "segment configuration catalog table, reason : %s",
				      "BEGIN",
				      PQresultErrorMessage(result));
		goto cleanup;
	}
	PQclear(result);

	sql = createPQExpBuffer();
	appendPQExpBuffer(sql,"DELETE FROM gp_segment_configuration WHERE role = 'p' or role = 'm'");
	result = PQexec(conn, sql->data);
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Fail to run SQL: %s when cleanup "
				      "segment configuration catalog table, reason : %s",
				      sql->data,
				      PQresultErrorMessage(result));
		goto cleanup;
	}
	PQclear(result);

	result = PQexec(conn, "COMMIT");
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Fail to run SQL: %s when cleanup "
				      "segment configuration catalog table, reason : %s",
				      "COMMIT",
				      PQresultErrorMessage(result));
		goto cleanup;
	}

	elog(LOG, "Cleanup segment configuration catalog table successfully!");

cleanup:
	if(sql)
		destroyPQExpBuffer(sql);
	if(result)
		PQclear(result);
	PQfinish(conn);
}
/*
 *  update a segment's status in gp_segment_configuration table.
 *  id : registration order of this segment
 *  status : new status of this segment
 */
void update_segment_status(int32_t id, char status)
{
	int	libpqres = CONNECTION_OK;
	PGconn *conn = NULL;
	char conninfo[512];
	PQExpBuffer sql = NULL;
	PGresult* result = NULL;

	sprintf(conninfo, "options='-c gp_session_role=UTILITY -c allow_system_table_mods=dml' "
			"dbname=template1 port=%d connect_timeout=%d", master_addr_port, CONNECT_TIMEOUT);
	conn = PQconnectdb(conninfo);
	if ((libpqres = PQstatus(conn)) != CONNECTION_OK)
	{
		elog(WARNING, "Fail to connect database when update segment's status "
				      "in segment configuration catalog table, error code: %d, %s",
				      libpqres,
				      PQerrorMessage(conn));
		PQfinish(conn);
		return;
	}

	result = PQexec(conn, "BEGIN");
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Fail to run SQL: %s when update segment's status "
				      "in segment configuration catalog table, reason : %s",
				      "BEGIN",
				      PQresultErrorMessage(result));
		goto cleanup;
	}
	PQclear(result);

	sql = createPQExpBuffer();
	appendPQExpBuffer(sql,"UPDATE gp_segment_configuration SET status='%c' WHERE registration_order=%d",
						   status, id);
	result = PQexec(conn, sql->data);
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Fail to run SQL: %s when update segment's status "
				      "in segment configuration catalog table, reason : %s",
				      sql->data,
				      PQresultErrorMessage(result));
		goto cleanup;
	}
	PQclear(result);

	result = PQexec(conn, "COMMIT");
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Fail to run SQL: %s when update segment's status "
					  "in segment configuration catalog table, reason : %s",
					  "COMMIT",
					  PQresultErrorMessage(result));
		goto cleanup;
	}

	elog(LOG, "Update a segment's status to '%c' in segment configuration catalog table,"
			  "registration_order : %d",
			  status, id);

cleanup:
	if(sql)
		destroyPQExpBuffer(sql);
	if(result)
		PQclear(result);
	PQfinish(conn);
}

/*
 * add a row into table gp_segment_configuration using psql
 * id : registration order of this segment
 * hostname : hostname of this segment
 * addreess : IP address of this segment
 * port : port of this segment
 * role : role of this segment
 */
void add_segment_config_row(int32_t id, char* hostname, char* address, uint32_t port, char role)
{
	int	libpqres = CONNECTION_OK;
	PGconn *conn = NULL;
	char conninfo[512];
	PQExpBuffer sql = NULL;
	PGresult* result = NULL;

	sprintf(conninfo, "options='-c gp_session_role=UTILITY -c allow_system_table_mods=dml' "
			"dbname=template1 port=%d connect_timeout=%d", master_addr_port, CONNECT_TIMEOUT);
	conn = PQconnectdb(conninfo);
	if ((libpqres = PQstatus(conn)) != CONNECTION_OK)
	{
		elog(WARNING, "Fail to connect database when add a new row "
				      "into segment configuration catalog table, error code: %d, %s",
				  	  libpqres,
				  	  PQerrorMessage(conn));
		PQfinish(conn);
		return;
	}

	result = PQexec(conn, "BEGIN");
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Fail to run SQL: %s when add a new row "
			      	  "into segment configuration catalog table, reason : %s",
			      	  "BEGIN",
			      	  PQresultErrorMessage(result));
		goto cleanup;
	}
	PQclear(result);

	sql = createPQExpBuffer();
	appendPQExpBuffer(sql,
					  "INSERT INTO gp_segment_configuration(registration_order,role,status,port,hostname,address) "
					  "VALUES "
					  "(%d,'%c','%c',%d,'%s','%s')",
					  id,role,SEGMENT_STATUS_UP,port,hostname,address);
	result = PQexec(conn, sql->data);
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Fail to run SQL: %s when add a new row "
				      "into segment configuration catalog table, reason : %s",
					  sql->data,
					  PQresultErrorMessage(result));
		goto cleanup;
	}
	PQclear(result);

	result = PQexec(conn, "COMMIT");
	if (!result || PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		elog(WARNING, "Fail to run SQL: %s when add a new row "
				      "into segment configuration catalog table, reason : %s",
					  "COMMIT",
					  PQresultErrorMessage(result));
		goto cleanup;
	}

	elog(LOG, "Add a new row into segment configuration catalog table,"
			  "registration order:%d, role:%c, port:%d, hostname:%s, address:%s",
			  id, role, port, hostname, address);

cleanup:
	if(sql)
		destroyPQExpBuffer(sql);
	if(result)
		PQclear(result);
	PQfinish(conn);
}

/*
 * Add HAWQ segment based on segment stat information. This function registers
 * one segment in resource pool. If the host exists, update based on the latest
 * information.
 */
int addHAWQSegWithSegStat(SegStat segstat)
{
	Assert(segstat != NULL);

	int				 res			 = FUNC_RETURN_OK;
	char			*hostname		 = NULL;
	int				 hostnamelen 	 = -1;
	SegResource	 	 segresource	 = NULL;
	int32_t		 	 segid			 = SEGSTAT_ID_INVALID;
	SimpString		 hostnamekey;
	SimpArray 		 hostaddrkey;
	bool			 segcapchanged  = false;

	/*
	 * Check if the host information exists in the resource pool. Fetch old
	 * machine and check if necessary to update.
	 */
	hostname 	= GET_SEGINFO_HOSTNAME(&(segstat->Info));
	hostnamelen = segstat->Info.HostNameLen;
    res = getSegIDByHostName(hostname, hostnamelen, &segid);
    if ( res != FUNC_RETURN_OK )
    {
    	/* Try addresses of this machine. */
    	for ( int i = 0 ; i < segstat->Info.HostAddrCount ; ++i )
    	{

    		elog(DEBUG5, "Resource manager checks host ip (%d)th to get segment.", i);
    		AddressString addr = NULL;
    		getSegInfoHostAddrStr(&(segstat->Info), i, &addr);
    		if ( strcmp(addr->Address, IPV4_DOT_ADDR_LO) == 0 &&
    			 segstat->Info.HostAddrCount > 1 )
    		{
    			/*
    			 * If the host has only one address as 127.0.0.1, we have to
    			 * save it to track the only one segment, otherwise, we skip
    			 * 127.0.0.1 address in resource pool. Because, each node has
    			 * one entry of this address.
    			 */
    			continue;
    		}

    		res = getSegIDByHostAddr((uint8_t *)(addr->Address), addr->Length, &segid);
    		if ( res == FUNC_RETURN_OK )
    		{
    			break;
    		}
    	}
    }

    /* CASE 1. It is a new host. */
	if ( res != FUNC_RETURN_OK )
	{
		/* Create machine information and corresponding resource information. */
		segresource = createSegResource(segstat);

		/* Update machine internal ID. */
		segresource->Stat->ID = PRESPOOL->SegmentIDCounter;
		PRESPOOL->SegmentIDCounter++;
		segid = segresource->Stat->ID;

		/* Add HAWQ node into resource pool indexed by machine id. */
		void *oldval = setHASHTABLENode(&(PRESPOOL->Segments),
						  	  	  	    TYPCONVERT(void *, segid),
										TYPCONVERT(void *, segresource),
										false /* Should be no old value. */);
		if ( oldval != NULL )
		{
			Assert(false);
		}

		/* Set HAWQ node indices to help find machine id. */
		setSimpleStringRef(&hostnamekey, hostname, hostnamelen);

		oldval = setHASHTABLENode(&(PRESPOOL->SegmentHostNameIndexed),
						  	  	  TYPCONVERT(void *, &hostnamekey),
								  TYPCONVERT(void *, segid),
								  false /* There should be no old value. */);
		if ( oldval != NULL )
		{
			Assert(false);
		}

		elog(LOG, "Resource manager tracked segment %d of host %s.", segid, hostname);

		/* Index all HAWQ node's ip addresses.
		 *
		 * NOTE: However, we may have some address duplicated in each machine,
		 * 		 for example, 127.0.0.1. Here, we just only add new address.
		 * 		 When there is only one segment identified, 127.0.0.1, this node
		 * 		 will be tracked by 127.0.0.1, otherwise, 127.0.0.1 is skipped.
		 */
		for ( int i = 0 ; i < segresource->Stat->Info.HostAddrCount ; ++i )
		{

			AddressString addr = NULL;
			getSegInfoHostAddrStr(&(segresource->Stat->Info), i, &addr);

			/* Probe if the key exists. */
			setSimpleArrayRef(&hostaddrkey, (char *)(addr->Address), addr->Length);
			if ( getHASHTABLENode( &(PRESPOOL->SegmentHostAddrIndexed),
					  	  	       TYPCONVERT(void *, &hostaddrkey)) == NULL )
			{
				setHASHTABLENode( &(PRESPOOL->SegmentHostAddrIndexed),
							  	  TYPCONVERT(void *, &hostaddrkey),
								  TYPCONVERT(void *, segid),
								  false /* There should be no old value. */);

				elog(LOG, "Resource manager tracked ip address '%.*s' for host '%s'",
						  hostaddrkey.Len, hostaddrkey.Array,
					      hostname);
			}
		}

		/*
		 * Set this node HAWQ available. This is a new host, it is always set
		 * HAWQ available because this is from FTS heart-beat of one segment.
		 */
		setSegResHAWQAvailability(segresource, RESOURCE_SEG_STATUS_AVAILABLE);

		/* Add this node into the table gp_segment_configuration */
		AddressString straddr = NULL;
		getSegInfoHostAddrStr(&(segresource->Stat->Info), 0, &straddr);
		Assert(straddr->Address != NULL);

		add_segment_config_row(segid+REGISTRATION_ORDER_OFFSET,
							   hostname,
				               straddr->Address,
							   segresource->Stat->Info.port,
							   SEGMENT_ROLE_PRIMARY);

		/* Add this node into the io bytes workload BBST structure. */
		addSegResourceIOBytesWorkloadIndex(segresource);
		/* Add this node into the alloc/avail resource ordered indices. */
		addSegResourceAvailIndex(segresource);
		addSegResourceAllocIndex(segresource);
		segcapchanged = true;
		res = FUNC_RETURN_OK;
	}
	/*
	 * Case 2. Existing host. Check if should update host information.
	 *
	 * NOTE: We update the capacity and availability now.
	 */
	else {
		segresource = getSegResource(segid);
		Assert(segresource != NULL);

		if ( !IS_SEGSTAT_FTSAVAILABLE(segresource->Stat) )
		{
			setSegResHAWQAvailability(segresource, RESOURCE_SEG_STATUS_AVAILABLE);
			update_segment_status(segresource->Stat->ID + REGISTRATION_ORDER_OFFSET, SEGMENT_STATUS_UP);

			elog(LOG, "Resource manager sets segment %s(%d) up from down.",
					  GET_SEGRESOURCE_HOSTNAME(segresource),
					  segid);
		}

		/* The machine should be up. Update port number. */
		segresource->Stat->Info.port = segstat->Info.port;

		/* Update node capacity. */
		if (((segstat->FTSTotalCore > 0)  &&
			 segresource->Stat->FTSTotalCore != segstat->FTSTotalCore) ||
			((segstat->FTSTotalMemoryMB > 0) &&
			 segresource->Stat->FTSTotalMemoryMB != segstat->FTSTotalMemoryMB))
		{

			uint32_t oldftsmem  = segresource->Stat->FTSTotalMemoryMB;
			uint32_t oldftscore = segresource->Stat->FTSTotalCore;

			if ( segstat->FTSTotalMemoryMB > 0 && segstat->FTSTotalCore > 0 )
			{
				/* Update machine resource capacity. */
				segresource->Stat->FTSTotalMemoryMB = segstat->FTSTotalMemoryMB;
				segresource->Stat->FTSTotalCore     = segstat->FTSTotalCore;

				/* Update overall cluster resource capacity. */
				minusResourceBundleData(&(PRESPOOL->FTSTotal), oldftsmem, oldftscore);
				addResourceBundleData(&(PRESPOOL->FTSTotal),
								      segresource->Stat->FTSTotalMemoryMB,
								      segresource->Stat->FTSTotalCore);
			}

			elog(LOG, "Resource manager sets physical host '%s' capacity change "
					  "from FTS (%d MB,%d CORE) to FTS (%d MB,%d CORE)",
					  GET_SEGRESOURCE_HOSTNAME(segresource),
					  oldftsmem,
					  oldftscore,
					  segresource->Stat->FTSTotalMemoryMB,
					  segresource->Stat->FTSTotalCore);

			segcapchanged =
				oldftsmem  != segresource->Stat->FTSTotalMemoryMB ||
				oldftscore != segresource->Stat->FTSTotalCore;
		}

		/* update the status of this node */
		segresource->LastUpdateTime = gettime_microsec();
		res = RESOURCEPOOL_DUPLICATE_HOST;
	}

	/*
	 * If host capacity is changed, update the cluster level memory/core ratio.
	 * The expectation is that more than 50% cluster nodes has the same memory/
	 * core ratio which is selected as the cluster memory/core ratio.
	 */
	if ( segcapchanged ) {
		uint32_t curratio = 0;
		if ( DRMGlobalInstance->ImpType == NONE_HAWQ2 )
		{
			curratio = trunc(segresource->Stat->FTSTotalMemoryMB /
							 segresource->Stat->FTSTotalCore);

			if ( curratio != PRESPOOL->MemCoreRatio )
			{
				PRESPOOL->MemCoreRatioMajorityCounter--;
				if ( PRESPOOL->MemCoreRatioMajorityCounter == -1 )
				{
					PRESPOOL->MemCoreRatioMajorityCounter = 1;
					PRESPOOL->MemCoreRatio = curratio;
					elog(LOG, "Resource manager changes cluster memory/core ratio "
							  "to %d MBPCORE.",
							  curratio);


				}
			}
			else
			{
				PRESPOOL->MemCoreRatioMajorityCounter++;
			}
		}
	}

	validateResourcePoolStatus(true);
	return res;
}

int updateHAWQSegWithGRMSegStat( SegStat segstat)
{
	Assert(segstat != NULL);

	int				 res			 = FUNC_RETURN_OK;
	char			*hostname		 = NULL;
	int				 hostnamelen 	 = -1;
	SegResource	 	 segres	 		 = NULL;
	SegStat	 	 	 newSegStat	     = NULL;
	int32_t		 	 segid			 = SEGSTAT_ID_INVALID;

	/*
	 * Check if the host information exists in the resource pool. Fetch old
	 * machine and check if necessary to update.
	 */
	hostname 	= GET_SEGINFO_GRMHOSTNAME(&(segstat->Info));
	hostnamelen = segstat->Info.GRMHostNameLen;
    res = getSegIDByHostName(hostname, hostnamelen, &segid);
    if ( res != FUNC_RETURN_OK )
    {
    	/* Try addresses of this machine. */
    	for ( int i = 0 ; i < segstat->Info.HostAddrCount ; ++i )
    	{
    		elog(DEBUG5, "Resource manager checks host ip (%d)th to get segment.", i);
    		AddressString addr = NULL;
    		getSegInfoHostAddrStr(&(segstat->Info), i, &addr);
    		if ( strcmp(addr->Address, IPV4_DOT_ADDR_LO) == 0 &&
    			 segstat->Info.HostAddrCount > 1 )
    		{
    			/*
    			 * If the host has only one address as 127.0.0.1, we have to
    			 * save it to track the only one segment, otherwise, we skip
    			 * 127.0.0.1 address in resource pool. Because, each node has
    			 * one entry of this address.
    			 */
    			continue;
    		}

    		res = getSegIDByHostAddr((uint8_t *)(addr->Address), addr->Length, &segid);
    		if ( res == FUNC_RETURN_OK )
    		{
    			break;
    		}
    	}
    }

	/* Update only the grm capacity. */
    if ( res != FUNC_RETURN_OK )
    {
    	elog(LOG, "Resource manager can not find resource broker reported host %s "
    			  "in the registered segment list. Skip it.",
				  hostname);
    	return res;
    }

	segres = getSegResource(segid);

	/*
	 * update HAWQ RM's SegResource info with GRM info.
	 */
	int ghostlen = __SIZE_ALIGN64(segstat->Info.GRMHostNameLen+1);
	int gracklen = __SIZE_ALIGN64(segstat->Info.GRMRackNameLen+1);
	int oldghostlen = segres->Stat->Info.GRMHostNameLen == 0 ?
					  0 :
					  __SIZE_ALIGN64(segres->Stat->Info.GRMHostNameLen+1);
	int oldgracklen = segres->Stat->Info.GRMRackNameLen == 0 ?
					  0 :
					  __SIZE_ALIGN64(segres->Stat->Info.GRMRackNameLen+1);
	int change = ghostlen + gracklen - oldghostlen - oldgracklen;
	if (change > 0)
	{
		newSegStat = rm_repalloc(PCONTEXT,
								 segres->Stat,
								 offsetof(SegStatData, Info) +
								 	 segres->Stat->Info.Size + change);
		segres->Stat = newSegStat;
	}
	else
		newSegStat = segres->Stat;

	Assert(newSegStat != NULL);
	/* Reset the memory area for GRM host and rack name zero filled. */
	memset((char*)newSegStat +
		   offsetof(SegStatData, Info) + segres->Stat->Info.Size -
		   (oldghostlen + oldgracklen),
		   '\0',
		   ghostlen + gracklen);

	Assert(newSegStat != NULL);

	newSegStat->Info.GRMHostNameLen    = segstat->Info.GRMHostNameLen;
	newSegStat->Info.GRMHostNameOffset = newSegStat->Info.HostNameOffset +
										 __SIZE_ALIGN64(newSegStat->Info.HostNameLen + 1);
	newSegStat->Info.GRMRackNameLen    = segstat->Info.GRMRackNameLen;
	newSegStat->Info.GRMRackNameOffset = newSegStat->Info.GRMHostNameOffset +
										 __SIZE_ALIGN64(newSegStat->Info.GRMHostNameLen+1);
	newSegStat->Info.Size = newSegStat->Info.GRMRackNameOffset +
						    __SIZE_ALIGN64(newSegStat->Info.GRMRackNameLen+1);

	strcpy(GET_SEGINFO_GRMHOSTNAME(&(newSegStat->Info)),
		   GET_SEGINFO_GRMHOSTNAME(&(segstat->Info)));
	strcpy(GET_SEGINFO_GRMRACKNAME(&(newSegStat->Info)),
		   GET_SEGINFO_GRMRACKNAME(&(segstat->Info)));

	segres->Stat = newSegStat;

	elog(LOG, "Resource manager update segment info, hostname:%s, "
			  "with GRM hostname:%s, GRM rackname:%s",
			  GET_SEGINFO_HOSTNAME(&(newSegStat->Info)),
			  GET_SEGINFO_GRMHOSTNAME(&(newSegStat->Info)),
			  GET_SEGINFO_GRMRACKNAME(&(newSegStat->Info)));

	/* Always set segment global resource manager available. */
	setSegResGLOBAvailability(segres, RESOURCE_SEG_STATUS_AVAILABLE);

	if ( segres->Stat->GRMTotalMemoryMB != segstat->GRMTotalMemoryMB ||
		 segres->Stat->GRMTotalCore     != segstat->GRMTotalCore )
	{
		uint32_t oldgrmmem  = segres->Stat->GRMTotalMemoryMB;
		uint32_t oldgrmcore = segres->Stat->GRMTotalCore;

		/* Update machine resource capacity. */
		segres->Stat->GRMTotalMemoryMB = segstat->GRMTotalMemoryMB;
		segres->Stat->GRMTotalCore     = segstat->GRMTotalCore;

		/* Update overall cluster resource capacity. */
		minusResourceBundleData(&(PRESPOOL->GRMTotal), oldgrmmem, oldgrmcore);
		addResourceBundleData(&(PRESPOOL->GRMTotal),
						  	  segres->Stat->GRMTotalMemoryMB,
							  segres->Stat->GRMTotalCore);

		elog(LOG, "Resource manager finds host %s capacity changed from "
				    "GRM (%d MB, %d CORE) to GRM (%d MB, %d CORE)",
					GET_SEGRESOURCE_HOSTNAME(segres),
					oldgrmmem,
					oldgrmcore,
					segres->Stat->GRMTotalMemoryMB,
					segres->Stat->GRMTotalCore);
	}

	int32_t curratio = 0;
	if (DRMGlobalInstance->ImpType == YARN_LIBYARN &&
		segres->Stat->GRMTotalMemoryMB > 0 &&
		segres->Stat->GRMTotalCore > 0)
	{
		curratio = trunc(segres->Stat->GRMTotalMemoryMB /
						 segres->Stat->GRMTotalCore);
	}

	if ( curratio != PRESPOOL->MemCoreRatio )
	{
		PRESPOOL->MemCoreRatioMajorityCounter--;
		if ( PRESPOOL->MemCoreRatioMajorityCounter == -1 )
		{
			PRESPOOL->MemCoreRatioMajorityCounter = 1;
			PRESPOOL->MemCoreRatio = curratio;
			elog(LOG, "Resource manager changes cluster memory/core ratio to "
						"%d MB Per core.",
						curratio);
		}
	}
	else
	{
		PRESPOOL->MemCoreRatioMajorityCounter++;
	}

	return FUNC_RETURN_OK;
}

void setAllSegResourceGRMUnavailable(void)
{
	List 	 *allsegres = NULL;
	ListCell *cell		= NULL;
	getAllPAIRRefIntoList(&(PRESPOOL->Segments), &allsegres);

	foreach(cell, allsegres)
	{
		SegResource segres = (SegResource)(((PAIR)lfirst(cell))->Value);
		setSegResGLOBAvailability(segres, RESOURCE_SEG_STATUS_UNAVAILABLE);
	}
	freePAIRRefList(&(PRESPOOL->Segments), &allsegres);
}

/*
 * Check index to get host id based on host name string.
 */
int getSegIDByHostName(const char *hostname, int hostnamelen, int32_t *id)
{
	PAIR 		pair = NULL;
	SimpString  key;

	*id = SEGSTAT_ID_INVALID;

	setSimpleStringRef( &key,
			            (char *)hostname,
						hostnamelen > 0 ? hostnamelen : strlen(hostname));

	pair = getHASHTABLENode(&(PRESPOOL->SegmentHostNameIndexed), (void *)&key);
	if ( pair != NULL )
	{
		*id = TYPCONVERT(uint32_t, (pair->Value));
		return FUNC_RETURN_OK;
	}
	return FUNC_RETURN_FAIL;
}

int getSegIDByHostAddr(uint8_t *hostaddr, int32_t hostaddrlen, int32_t *id)
{
	PAIR 		pair = NULL;
	SimpArray   addrkey;
	addrkey.Array = (char *)hostaddr;
	addrkey.Len   = hostaddrlen;

	*id = SEGSTAT_ID_INVALID;

	pair = getHASHTABLENode(&(PRESPOOL->SegmentHostAddrIndexed), (void *)&addrkey);
	if ( pair != NULL )
	{
		*id = TYPCONVERT(uint32_t, (pair->Value));
		return FUNC_RETURN_OK;
	}
	return FUNC_RETURN_FAIL;
}

/* Create new ResourceInfo instance with basic attributes initialized. */
SegResource createSegResource(SegStat segstat)
{
	SegResource res = NULL;
	/* Create machine information and corresponding resource information. */
	res = (SegResource)rm_palloc0(PCONTEXT, sizeof(SegResourceData));
	resetResourceBundleData(&(res->Allocated), 0, 0.0, 0);
	resetResourceBundleData(&(res->Available), 0, 0.0, 0);

	res->IOBytesWorkload = 0;
	res->SliceWorkload   = 0;
	res->Stat     		 = segstat;
	res->LastUpdateTime  = gettime_microsec();
	res->Stat->FTSAvailable = RESOURCE_SEG_STATUS_UNSET;
	res->Stat->GRMAvailable = RESOURCE_SEG_STATUS_UNSET;
	res->RUAlivePending  = false;

	for ( int i = 0 ; i < RESOURCE_QUEUE_RATIO_SIZE ; ++i )
	{
		res->ContainerSets[i] = NULL;
	}

	resetResourceBundleData(&(res->IncPending), 0, 0.0, 0);
	resetResourceBundleData(&(res->DecPending), 0, 0.0, 0);
	resetResourceBundleData(&(res->OldInuse)  , 0, 0.0, 0);

	res->GRMContainerCount = 0;

	return res;
}

int setSegStatHAWQAvailability( SegStat segstat, uint8_t newstatus)
{
	int res = segstat->FTSAvailable;
	segstat->FTSAvailable = newstatus;
	return res;
}

int setSegStatGLOBAvailability( SegStat segstat, uint8_t newstatus)
{
	int res = segstat->GRMAvailable;
	segstat->GRMAvailable = newstatus;
	return res;
}

/* Set hawq status of host, return the old status */
int setSegResHAWQAvailability( SegResource segres, uint8_t newstatus)
{
	int res = setSegStatHAWQAvailability(segres->Stat, newstatus);

	/* If the status is not changed, no need to refresh the cluster capacity. */
	if ( res == newstatus )
	{
		return res;
	}

	if ( newstatus == RESOURCE_SEG_STATUS_UNAVAILABLE )
	{
		minusResourceBundleData(&(PRESPOOL->FTSTotal),
								segres->Stat->FTSTotalMemoryMB,
								segres->Stat->FTSTotalCore);
		minusResourceBundleData(&(PRESPOOL->GRMTotal),
								segres->Stat->GRMTotalMemoryMB,
								segres->Stat->GRMTotalCore);

		minusResourceFromResourceManagerByBundle(&(segres->Allocated));

		PRESPOOL->AvailNodeCount--;
		Assert(PRESPOOL->AvailNodeCount >= 0);
		setSegResRUAlivePending(segres, false);
	}
	else
	{
		addResourceBundleData(&(PRESPOOL->FTSTotal),
							  segres->Stat->FTSTotalMemoryMB,
							  segres->Stat->FTSTotalCore);
		addResourceBundleData(&(PRESPOOL->GRMTotal),
							  segres->Stat->GRMTotalMemoryMB,
							  segres->Stat->GRMTotalCore);

		addNewResourceToResourceManagerByBundle(&(segres->Allocated));
		PRESPOOL->AvailNodeCount++;
	}

	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i )
	{
		uint32_t ratio = PQUEMGR->RatioReverseIndex[i];
		reorderSegResourceAvailIndex(segres, ratio);
		reorderSegResourceAllocIndex(segres, ratio);
	}

	elog(LOG, "Host %s is set availability %d. Cluster currently has %d available nodes.",
			  GET_SEGRESOURCE_HOSTNAME(segres),
			  segres->Stat->FTSAvailable,
			  PRESPOOL->AvailNodeCount);

	return res;
}

int setSegResGLOBAvailability( SegResource segres, uint8_t newstatus)
{
	return setSegStatGLOBAvailability(segres->Stat, newstatus);
}

/* Generate HAWQ host report. */
void generateSegResourceReport(int32_t segid, SelfMaintainBuffer buff)
{
	SegResource seg = getSegResource(segid);

	if ( seg == NULL )
	{
		static char messagenull[] = "NULL NODE.";
		appendSelfMaintainBuffer(buff, messagenull, sizeof(messagenull));
	}
	else
	{
		static char reporthead[256];

		int headsize = sprintf(reporthead,
                			   "SEGMENT:ID=%d, "
                			   "HAWQAVAIL=%d,GLOBAVAIL=%d. "
                			   "FTS( %d MB, %d CORE). "
							   "GRM( %d MB, %d CORE). "
							   "MEM=%d(%d) MB. "
							   "CORE=%lf(%lf).\n",
							   seg->Stat->ID,
							   seg->Stat->FTSAvailable,
							   seg->Stat->GRMAvailable,
							   seg->Stat->FTSTotalMemoryMB,
							   seg->Stat->FTSTotalCore,
							   seg->Stat->GRMTotalMemoryMB,
							   seg->Stat->GRMTotalCore,
							   seg->Allocated.MemoryMB,
							   seg->Available.MemoryMB,
							   seg->Allocated.Core,
                			   seg->Available.Core);
		appendSelfMaintainBuffer(buff, reporthead, headsize);
		generateSegInfoReport(&(seg->Stat->Info), buff);
	}
}

/* Generate machine id instance data into a string as report. */
void  generateSegInfoReport(SegInfo seginfo, SelfMaintainBuffer buff)
{
	static char zeropad = '\0';
	static char reporthead[256];
	int reportheadlen = 0;
	char *host = NULL;
	if (seginfo->HostNameLen != 0)
		host = GET_SEGINFO_HOSTNAME(seginfo);
	else if (seginfo->GRMHostNameLen != 0)
		host = GET_SEGINFO_GRMHOSTNAME(seginfo);
	else
		host = "UNKNOWN host";
	reportheadlen =
			sprintf(reporthead,
            "NODE:HOST=%s:%d,Master:%d,Standby:%d,Alive:%d.",
			host,
            seginfo->port,
            seginfo->master,
			seginfo->standby,
			seginfo->alive);

	appendSelfMaintainBuffer(buff, reporthead, reportheadlen);
	appendSelfMaintainBuffer(buff, "Addresses:", sizeof("Addresses:")-1);

	for ( int i = 0 ; i < seginfo->HostAddrCount ; ++i )
	{
		if ( i > 0 )
		{
			appendSelfMaintainBuffer(buff, ",", sizeof(",") - 1);
		}
		generateSegInfoAddrStr(seginfo, i, buff);
	}
	appendSMBVar(buff, zeropad);
}

/* Generate string version address. */
void generateSegInfoAddrStr(SegInfo seginfo, int addrindex, SelfMaintainBuffer buff)
{
	Assert(buff != NULL);
	Assert(seginfo != NULL);
	Assert(addrindex >= 0  && addrindex < seginfo->HostAddrCount);

	/* Get address attribute and offset value. */
	uint16_t attr = GET_SEGINFO_ADDR_ATTR_AT(seginfo, addrindex);

	if ( IS_SEGINFO_ADDR_STR(attr) )
	{
		AddressString straddr = NULL;
		getSegInfoHostAddrStr(seginfo, addrindex, &straddr);
		appendSelfMaintainBuffer(buff, straddr->Address, straddr->Length);
	}
	else
	{
		Assert(false);
	}
}

/* Get segment resource instance based on segment id. */
SegResource getSegResource(int32_t id)
{
	PAIR pair = getHASHTABLENode(&(PRESPOOL->Segments),
								 TYPCONVERT(void *, id));
	return pair == NULL ? 
		   NULL :
		   (SegResource)(pair->Value);
}

/* Generate machine id instance data into a string as report. */
void  generateSegStatReport(SegStat segstat, SelfMaintainBuffer buff)
{
	static char reporthead[256];
	int reportheadlen = 0;
	reportheadlen = sprintf(reporthead,
            "NODE:ID=%d,HAWQ %s, GRM %s, "
            "HAWQ CAP (%d MB, %lf CORE), "
			"GRM CAP(%d MB, %lf CORE),",
            segstat->ID,
			segstat->FTSAvailable ? "AVAIL" : "UNAVAIL",
			segstat->GRMAvailable ? "AVAIL" : "UNAVAIL",
            segstat->FTSTotalMemoryMB,
			segstat->FTSTotalCore * 1.0,
			segstat->GRMTotalMemoryMB,
			segstat->GRMTotalCore * 1.0);


	appendSelfMaintainBuffer(buff, reporthead, reportheadlen);
	generateSegInfoReport(&(segstat->Info), buff);
}
/*
 * Add container to the hash-table of the containers to be accepted. The container
 * capacity should be notified to the corresponding segments.
 */
int  addGRMContainerToToBeAccepted(GRMContainer ctn)
{
	int		 res		 = FUNC_RETURN_OK;
	int32_t  segid		 = SEGSTAT_ID_INVALID;
	uint32_t hostnamelen = strlen(ctn->HostName);

	/*
	 * If the host does not exist, this container can not be added. Check the
	 * host name directly, and try the host address again.
	 */
	if ( ctn->Resource == NULL )
	{

		res = getSegIDByGRMHostName(ctn->HostName, hostnamelen, &segid);
		if ( res != FUNC_RETURN_OK )
		{
			addGRMContainerToKicked(ctn);
			elog(LOG, "Resource manager can not find registered host %s. "
					  "To return this host's resource container at once.",
					  ctn->HostName);
			return res;
		}
		ctn->Resource = getSegResource(segid);
		elog(LOG, "Resource manager recognized resource container on host %s",
				  ctn->HostName);
	}

	/* The host must has registered resource information linked. */
	Assert( ctn->Resource != NULL);

	/* Set the resource as increasing pending. */
	addResourceBundleData(&(ctn->Resource->IncPending), ctn->MemoryMB, ctn->Core);

	/* Add the container to the hash-table to be processed consequently. */
	SimpString key;
	setSimpleStringRef(&key, ctn->HostName, hostnamelen);
	PAIR pair = getHASHTABLENode(&(PRESPOOL->ToAcceptContainers), &key);
	GRMContainerSet ctns = NULL;
	if ( pair == NULL )
	{
		ctns = createGRMContainerSet();
		setHASHTABLENode(&(PRESPOOL->ToAcceptContainers), &key, ctns, false);
	}
	else
	{
		ctns = (GRMContainerSet)(pair->Value);
	}

	appendGRMContainerSetContainer(ctns, ctn);
	PRESPOOL->AddPendingContainerCount++;
	elog(LOG, "AddPendingContainerCount added 1, current value %d",
			  PRESPOOL->AddPendingContainerCount);
	return FUNC_RETURN_OK;
}

void moveGRMContainerSetToAccepted(GRMContainerSet ctns)
{
	if ( ctns == NULL )
	{
		return;
	}
	PRESPOOL->AcceptedContainers = list_concat(PRESPOOL->AcceptedContainers,
											   ctns->Containers);
	ctns->Containers = NULL;
	resetResourceBundleData(&(ctns->Allocated), 0, 0.0, 0);
	resetResourceBundleData(&(ctns->Available), 0, 0.0, 0);
}

void moveGRMContainerSetToKicked(GRMContainerSet ctns)
{
	if ( ctns == NULL )
	{
		return;
	}
	PRESPOOL->KickedContainers = list_concat(PRESPOOL->KickedContainers,
											   ctns->Containers);
	ctns->Containers = NULL;
	resetResourceBundleData(&(ctns->Allocated), 0, 0.0, 0);
	resetResourceBundleData(&(ctns->Available), 0, 0.0, 0);
}

/**
 * Add resource container into the resource pool.
 *
 * container[in]			The resource container to add.
 *
 * Return					FUNC_RETURN_OK
 * 								Succeed.
 * 							RESOURCEPOOL_NO_HOSTID
 * 								Fail to add and move the container to the list
 * 								for returning to GRM.
 **/
void addGRMContainerToResPool(GRMContainer container)
{
	SegResource			segresource	= NULL;
	uint32_t			ratio		= 0;
	GRMContainerSet 	ctns		= NULL;
	bool				newratio	= false;

	Assert(container->Resource != NULL);
	segresource = container->Resource;

	/* Add resource container to the target host resource info instance. */
	ratio = container->MemoryMB / container->Core;
	createAndGetGRMContainerSet(segresource, ratio, &ctns);
	Assert(ctns != NULL);

	newratio = ctns->Allocated.MemoryMB == 0;

	appendGRMContainerSetContainer(ctns, container);

	addResourceBundleData(&(segresource->Allocated), container->MemoryMB, container->Core);
	addResourceBundleData(&(segresource->Available), container->MemoryMB, container->Core);

	minusResourceBundleData(&(segresource->IncPending), container->MemoryMB, container->Core);
	segresource->GRMContainerCount++;

	Assert( segresource->IncPending.Core >= 0 );
	Assert( segresource->IncPending.MemoryMB >= 0 );

	reorderSegResourceAvailIndex(segresource, ratio);
	reorderSegResourceAllocIndex(segresource, ratio);

	elog(LOG, "Resource manager added resource container into resource pool "
			  "(%d MB, %d CORE) at %s (%d:%.*s)",
			  container->MemoryMB,
			  container->Core,
			  container->HostName,
			  segresource->Stat->ID,
			  segresource->Stat->Info.HostNameLen,
			  GET_SEGRESOURCE_HOSTNAME(segresource));

	validateResourcePoolStatus(false);
}

void dropGRMContainerFromResPool(GRMContainer ctn)
{
	minusResourceBundleData(&(ctn->Resource->Allocated), ctn->MemoryMB, ctn->Core);
	minusResourceBundleData(&(ctn->Resource->Available), ctn->MemoryMB, ctn->Core);

	Assert( ctn->Resource->Allocated.MemoryMB >= 0 );
	Assert( ctn->Resource->Allocated.Core >= 0     );
	Assert( ctn->Resource->Available.MemoryMB >= 0 );
	Assert( ctn->Resource->Available.Core >= 0     );

	Assert( ctn->Resource->Allocated.MemoryMB >= 0 );
	Assert( ctn->Resource->Allocated.Core >= 0     );
	Assert( ctn->Resource->Available.MemoryMB >= 0 );
	Assert( ctn->Resource->Available.Core >= 0     );

	uint32_t ratio = trunc(ctn->MemoryMB / ctn->Core);
	reorderSegResourceAllocIndex(ctn->Resource, ratio);
	reorderSegResourceAvailIndex(ctn->Resource, ratio);

	elog(LOG, "Resource manager dropped resource container from resource pool "
			  "(%d MB, %d CORE) at %s (%d:%.*s)",
			  ctn->MemoryMB,
			  ctn->Core,
			  ctn->HostName,
			  ctn->Resource->Stat->ID,
			  ctn->Resource->Stat->Info.HostNameLen,
			  GET_SEGRESOURCE_HOSTNAME(ctn->Resource));
}

void addGRMContainerToToBeKicked(GRMContainer ctn)
{
	uint32_t				hostnamelen		= 0;

	hostnamelen = strlen(ctn->HostName);

	/* Add the container to the hash-table to be processed longer. */
	SimpString key;
	setSimpleStringRef(&key, ctn->HostName, hostnamelen);
	PAIR pair = getHASHTABLENode(&(PRESPOOL->ToKickContainers), &key);
	GRMContainerSet ctns = NULL;
	if ( pair == NULL )
	{
		ctns = createGRMContainerSet();
		setHASHTABLENode(&(PRESPOOL->ToKickContainers), &key, ctns, false);
	}
	else
	{
		ctns = (GRMContainerSet)(pair->Value);
	}

	appendGRMContainerSetContainer(ctns, ctn);

	/* Mark the resource info has resource pending for decreasing. */
	ctn->CalcDecPending = true;
	addResourceBundleData(&(ctn->Resource->DecPending), ctn->MemoryMB, ctn->Core);

	PRESPOOL->RetPendingContainerCount++;
}

void addGRMContainerToKicked(GRMContainer ctn)
{
	Assert(ctn != NULL);
	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	PRESPOOL->KickedContainers = lappend(PRESPOOL->KickedContainers, ctn);
	MEMORY_CONTEXT_SWITCH_BACK
	PRESPOOL->RetPendingContainerCount++;
}

int getOrderedResourceAvailTreeIndexByRatio(uint32_t ratio, BBST *tree)
{
	*tree = NULL;
	int32_t rindex = getResourceQueueRatioIndex(ratio);
	if ( rindex < 0 )
	{
		return RESOURCEPOOL_NO_RATIO;
	}
	else if ( rindex < RESOURCE_QUEUE_RATIO_SIZE )
	{
		*tree = PRESPOOL->OrderedSegResAvailByRatio[rindex];
	}
	else {
		Assert(false);
	}
	return FUNC_RETURN_OK;
}

int getOrderedResourceAllocTreeIndexByRatio(uint32_t ratio, BBST *tree)
{
	*tree = NULL;
	int32_t rindex = getResourceQueueRatioIndex(ratio);
	if ( rindex < 0 )
	{
		return RESOURCEPOOL_NO_RATIO;
	}
	else if ( rindex < RESOURCE_QUEUE_RATIO_SIZE )
	{
		*tree = PRESPOOL->OrderedSegResAllocByRatio[rindex];
	}
	else
	{
		Assert(false);
	}
	return FUNC_RETURN_OK;
}

int  addOrderedResourceAvailTreeIndexByRatio(uint32_t ratio, BBST *tree)
{
	*tree = NULL;
	int32_t rindex = getResourceQueueRatioIndex(ratio);
	if ( rindex < 0 )
	{
		return RESOURCEPOOL_NO_RATIO;
	}
	else if ( rindex < RESOURCE_QUEUE_RATIO_SIZE )
	{
		if ( PRESPOOL->OrderedSegResAvailByRatio[rindex] != NULL )
		{
			return RESOURCEPOOL_DUPLICATE_RATIO;
		}

		/* Create new BBST tree here. */
		*tree = createBBST(PCONTEXT,
						   TYPCONVERT(void *, ratio), __DRM_NODERESPOOL_comp_ratioFree);

		PRESPOOL->OrderedSegResAvailByRatio[rindex] = *tree;

		/* Add all current SegResource instances in to the tree index. */
		List 	 *allnodes = NULL;
		ListCell *cell	   = NULL;
		getAllPAIRRefIntoList(&(PRESPOOL->Segments), &allnodes);
		foreach(cell, allnodes)
		{
			SegResource curres = (SegResource)((PAIR)lfirst(cell))->Value;
			insertBBSTNode(*tree, createBBSTNode(*tree, curres));
		}
		freePAIRRefList(&(PRESPOOL->Segments), &allnodes);
	}
	else
	{
		Assert(false);
	}
	return FUNC_RETURN_OK;
}

int  addOrderedResourceAllocTreeIndexByRatio(uint32_t ratio, BBST *tree)
{
	*tree = NULL;
	int32_t rindex = getResourceQueueRatioIndex(ratio);
	if ( rindex < 0 )
	{
		return RESOURCEPOOL_NO_RATIO;
	}
	else if ( rindex < RESOURCE_QUEUE_RATIO_SIZE )
	{
		 if ( PRESPOOL->OrderedSegResAllocByRatio[rindex] != NULL )
		 {
			 return RESOURCEPOOL_DUPLICATE_RATIO;
		 }

		/* Create new BBST tree here. */
		*tree = createBBST(PCONTEXT,
						   TYPCONVERT(void *, ratio),
						   __DRM_NODERESPOOL_comp_ratioAlloc);

		PRESPOOL->OrderedSegResAllocByRatio[rindex] = *tree;

		/* Add all current SegResource instances in to the tree index. */
		List 	 *allnodes = NULL;
		ListCell *cell	   = NULL;
		getAllPAIRRefIntoList(&(PRESPOOL->Segments), &allnodes);
		foreach(cell, allnodes)
		{
			SegResource curres = (SegResource)((PAIR)lfirst(cell))->Value;
			insertBBSTNode(*tree, createBBSTNode(*tree, curres));
		}
		freePAIRRefList(&(PRESPOOL->Segments), &allnodes);
	}
	else
	{
		Assert(false);
	}
	return FUNC_RETURN_OK;
}

int getGRMContainerSet(SegResource segres, uint32_t ratio, GRMContainerSet *ctns)
{
	Assert(segres != NULL);
	*ctns = NULL;
	int32_t rindex = getResourceQueueRatioIndex(ratio);
	if ( rindex < 0 )
	{
		return RESOURCEPOOL_NO_RATIO;
	}

	if ( rindex < RESOURCE_QUEUE_RATIO_SIZE )
	{
		*ctns = segres->ContainerSets[rindex];
	}
	else
	{
		Assert(false);
	}
	return FUNC_RETURN_OK;
}

/*
 * Get container set instance of segment resource instance.
 */
int createAndGetGRMContainerSet(SegResource segres, uint32_t ratio, GRMContainerSet *ctns)
{
	int32_t rindex = getResourceQueueRatioIndex(ratio);
	if ( rindex < 0 )
	{
		return RESOURCEPOOL_NO_RATIO;
	}

	if ( rindex < RESOURCE_QUEUE_RATIO_SIZE )
	{
		if ( segres->ContainerSets[rindex] == NULL )
		{
			*ctns = createGRMContainerSet();
			segres->ContainerSets[rindex] = *ctns;
		}
		else
		{
			*ctns = segres->ContainerSets[rindex];
		}
	}
	else
	{
		Assert(false);
	}
	return FUNC_RETURN_OK;
}

GRMContainerSet createGRMContainerSet(void)
{
	GRMContainerSet res = (GRMContainerSet)rm_palloc(PCONTEXT,
													 sizeof(GRMContainerSetData));
	resetResourceBundleData(&(res->Allocated), 0, 0.0, 0);
	resetResourceBundleData(&(res->Available), 0, 0.0, 0);
	res->Containers = NULL;
	return res;
}

GRMContainer popGRMContainerSetContainerList(GRMContainerSet ctns)
{
	if ( ctns->Containers == NULL )
	{
		return NULL;
	}

	GRMContainer res = (GRMContainer)lfirst(list_head(ctns->Containers));
	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	ctns->Containers = list_delete_first(ctns->Containers);
	MEMORY_CONTEXT_SWITCH_BACK

	minusResourceBundleData(&(ctns->Allocated), res->MemoryMB, res->Core);
	minusResourceBundleData(&(ctns->Available), res->MemoryMB, res->Core);

	return res;
}

GRMContainer getGRMContainerSetContainerFirst(GRMContainerSet ctns)
{
	if ( ctns->Containers == NULL )
	{
		return NULL;
	}
	return (GRMContainer)lfirst(list_head(ctns->Containers));
}

void appendGRMContainerSetContainer(GRMContainerSet ctns, GRMContainer ctn)
{
	Assert( ctn != NULL );
	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	ctns->Containers = lappend(ctns->Containers, ctn);
	MEMORY_CONTEXT_SWITCH_BACK
	addResourceBundleData(&(ctns->Allocated), ctn->MemoryMB, ctn->Core);
	addResourceBundleData(&(ctns->Available), ctn->MemoryMB, ctn->Core);
}

void moveGRMContainerSetContainerList(GRMContainerSet tctns, GRMContainerSet sctns)
{
	addResourceBundleDataByBundle(&(tctns->Allocated), &(sctns->Allocated));
	addResourceBundleDataByBundle(&(tctns->Available), &(sctns->Available));

	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT);
	tctns->Containers = list_concat(tctns->Containers, sctns->Containers);
	sctns->Containers = NULL;
	MEMORY_CONTEXT_SWITCH_BACK

	resetResourceBundleData(&(sctns->Allocated), 0, 0.0, 0);
	resetResourceBundleData(&(sctns->Available), 0, 0.0, 0);
}

void freeGRMContainerSet(GRMContainerSet ctns)
{
	Assert( ctns->Containers == NULL );
	rm_pfree(PCONTEXT, ctns);
}
/**
 * THE MAIN FUNCTION for acquiring resource from resource pool.
 */

int allocateResourceFromResourcePool(int32_t 	nodecount,
									 int32_t	minnodecount,
		 	 	 	 	 	 	 	 uint32_t 	memory,
									 double 	core,
									 int64_t	iobytes,
									 int32_t    slicesize,
									 int32_t	vseglimitpseg,
									 int 		preferredcount,
									 char 	  **preferredhostname,
									 int64_t   *preferredscansize,
									 bool		fixnodecount,
									 List 	  **vsegcounters,
									 int32_t   *totalvsegcount,
									 int64_t   *vsegiobytes)
{
	return (*PRESPOOL->allocateResFuncs[rm_allocation_policy])(nodecount,
			 	 	 	 	 	 	 	 	 	 	 	 	   minnodecount,
															   memory,
															   core,
															   iobytes,
															   slicesize,
															   vseglimitpseg,
															   preferredcount,
															   preferredhostname,
															   preferredscansize,
															   fixnodecount,
															   vsegcounters,
															   totalvsegcount,
															   vsegiobytes);
}

int allocateResourceFromResourcePoolIOBytes(int32_t 	nodecount,
										    int32_t		minnodecount,
										    uint32_t 	memory,
										    double 		core,
										    int64_t		iobytes,
										    int32_t   	slicesize,
											int32_t		vseglimitpseg,
										    int 		preferredcount,
										    char 	  **preferredhostname,
										    int64_t    *preferredscansize,
										    bool		fixnodecount,
										    List 	  **vsegcounters,
										    int32_t    *totalvsegcount,
										    int64_t    *vsegiobytes)
{
	int 			res			  		= FUNC_RETURN_OK;
	uint32_t 		ratio 		  		= memory/core;
	BBST			nodetree	  		= &(PRESPOOL->OrderedIOBytesWorkload);
	BBSTNode		leftnode	  		= NULL;
	SegResource		segresource   		= NULL;
	List 		   *tmplist				= NULL;
	int32_t			segid		  		= SEGSTAT_ID_INVALID;
	GRMContainerSet containerset  		= NULL;
	int				nodecountleft 		= nodecount;
	int				impossiblecount   	= 0;
	bool			skipchosenmachine 	= true;
	int 			fullcount 			= nodetree->NodeIndex->NodeCount;

	/* This hash saves all selected hosts containing at least one segment.    */
	HASHTABLEData	vsegcnttbl;

	initializeHASHTABLE(&vsegcnttbl,
						PCONTEXT,
						HASHTABLE_SLOT_VOLUME_DEFAULT,
						HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
						HASHTABLE_KEYTYPE_UINT32,
						NULL);
	/*
	 *--------------------------------------------------------------------------
	 * stage 1 allocate based on locality, only 1 segment allocated in one host.
	 *--------------------------------------------------------------------------
	 */
	int clustersize = PRESPOOL->AvailNodeCount;
	if ( nodecount < clustersize )
	{
		elog(DEBUG5, "Resource manager tries to find host based on locality data.");

		for ( uint32_t i = 0 ; i < preferredcount ; ++i )
		{
			/*
			 * Get machine identified by HDFS host name. The HDFS host names does
			 * not have to be a YARN or HAWQ FTS recognized host name. Therefore,
			 * getNodeIDByHDFSHostName() is responsible to find one mapped HAWQ
			 * FTS unified host.
			 */
			res = getSegIDByHDFSHostName(preferredhostname[i],
										 strlen(preferredhostname[i]),
										 &segid);
			if ( res != FUNC_RETURN_OK )
			{
				/* Can not find the machine, skip this machine. */
				elog(LOG, "Resource manager failed to resolve HDFS host identified "
						  "by %s. This host is skipped temporarily.",
						  preferredhostname[i]);
				continue;
			}

			/* Get the resource counter of this host. */
			segresource = getSegResource(segid);

			if (!IS_SEGRESOURCE_USABLE(segresource))
			{
				elog(DEBUG3, "Segment %s has unavailable status:"
						  "RUAlivePending: %d, Available :%d.",
						  preferredhostname[i],
						  segresource->RUAlivePending,
						  segresource->Stat->FTSAvailable);
				continue;
			}

			/* Get the allocated resource of this host with specified ratio */
			res = getGRMContainerSet(segresource, ratio, &containerset);
			if ( res != FUNC_RETURN_OK )
			{
				/* This machine does not have the resource with matching ratio.*/
				elog(DEBUG3, "Segment %s does not contain expected "
						  "resource of %d MB per core. This host is skipped.",
						  preferredhostname[i],
						  ratio);
				continue;
			}

			/* Decide how many segments can be allocated based on locality data.*/
			int segcountact = containerset == NULL ?
							  0 :
							  containerset->Available.MemoryMB / memory;
			if ( segcountact == 0 )
			{
				elog(DEBUG3, "Segment %s does not have more resource to allocate. "
							 "This segment is skipped.",
							 preferredhostname[i]);
				continue;
			}

			/*
			 * We expect only 1 segment working in this preferred host. Therefore,
			 * we check one virtual segment containing slicesize slices.
			 *
			 * NOTE: In this loop we always try to find segments not breaking
			 * 		 slice limit. Because we will gothrough all segments later
			 * 		 if not enough segments are found in this loop.
			 */
			if ( segresource->SliceWorkload + slicesize > rm_slice_num_per_seg_limit )
			{
				elog(DEBUG3, "Segment %s contains %d slices working now, it can "
							 "not afford %d more slices.",
							 preferredhostname[i],
							 segresource->SliceWorkload,
							 slicesize);
				continue;
			}

			elog(DEBUG3, "Resource manager chooses segment %s to allocate vseg.",
						 GET_SEGRESOURCE_HOSTNAME(segresource));

			/* Allocate resource from selected host. */
			allocateResourceFromSegment(segresource,
									 	containerset,
										memory,
										core,
										slicesize);

			/* Reorder the changed host. */
			reorderSegResourceAvailIndex(segresource, ratio);

			/* Track the mapping from host information to hdfs host name index.*/
			VSegmentCounterInternal vsegcnt = createVSegmentCounter(i, segresource);

			setHASHTABLENode(&vsegcnttbl,
							 TYPCONVERT(void *, segresource->Stat->ID),
							 TYPCONVERT(void *, vsegcnt),
							 false);

			/* Check if we have gotten expected number of segments. */
			nodecountleft--;
			if ( nodecountleft == 0 )
			{
				break;
			}
		}
	}

	elog(DEBUG3, "After choosing vseg based on locality, %d vsegs allocated, "
				 "expect %d vsegs.",
				 nodecount-nodecountleft,
				 nodecount);

	/*
	 *--------------------------------------------------------------------------
	 * stage 2 allocate based on io workload.
	 *--------------------------------------------------------------------------
	 */
	while( nodecountleft > 0 &&
		   PRESPOOL->OrderedIOBytesWorkload.Root != NULL &&
		   impossiblecount < fullcount )
	{
		VSegmentCounterInternal curhost     = NULL;
		bool 				    skipcurrent = false;

		/* Get and remove the host having largest available resource. */
		leftnode = getLeftMostNode(nodetree);
		removeBBSTNode(nodetree, &leftnode);
		MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
		tmplist= lappend(tmplist, leftnode);
		MEMORY_CONTEXT_SWITCH_BACK

		/* If in current loop we should skip chosen machines, check and skip. */
		SegResource currresinfo = (SegResource)(leftnode->Data);

		elog(DEBUG5, "Try segment %s to allocate resource by round-robin.",
					 GET_SEGRESOURCE_HOSTNAME(currresinfo));

		if ( !IS_SEGRESOURCE_USABLE(currresinfo) )
		{
			impossiblecount++;
			skipcurrent = true;
			elog(DEBUG5, "Segment %s is not resource usable, status %d pending %d",
						 GET_SEGRESOURCE_HOSTNAME(currresinfo),
						 currresinfo->Stat->FTSAvailable,
						 currresinfo->RUAlivePending?1:0);
		}
		else
		{
			PAIR pair = getHASHTABLENode(&vsegcnttbl,
										 TYPCONVERT(void *,
													currresinfo->Stat->ID));
			if ( pair != NULL )
			{
				Assert(!currresinfo->RUAlivePending);
				Assert(IS_SEGSTAT_FTSAVAILABLE(currresinfo->Stat));

				curhost = (VSegmentCounterInternal)(pair->Value);
				/* Host should not break vseg num limit. */
				if ( !fixnodecount && curhost->VSegmentCount >= vseglimitpseg )
				{
					impossiblecount++;
					skipcurrent = true;
					elog(DEBUG5, "Segment %s can not container more vsegs for "
								 "current statement, allocated %d vsegs.",
								 GET_SEGRESOURCE_HOSTNAME(curhost->Resource),
								 curhost->VSegmentCount);
				}

				if ( !skipcurrent && skipchosenmachine )
				{
					impossiblecount++;
					skipcurrent = true;
					elog(DEBUG5, "Segment %s is skipped temporarily.",
								 GET_SEGRESOURCE_HOSTNAME(curhost->Resource));
				}
			}
		}

		if ( !skipcurrent )
		{
			/* Try to allocate resource in the selected host. */
			SegResource curres = (SegResource)(leftnode->Data);

			res = getGRMContainerSet(curres, ratio, &containerset);

			if ( res != FUNC_RETURN_OK )
			{
				/* This machine does not have the resource with matching ratio.
				 * In fact should never occur. */
				impossiblecount++;
				elog(DEBUG5, "Segment %s does not contain resource of %d MBPCORE",
							 GET_SEGRESOURCE_HOSTNAME(curres),
							 ratio);
			}
			else
			{

				if ( !fixnodecount &&
					 curres->SliceWorkload + slicesize > rm_slice_num_per_seg_limit )
				{
					elog(LOG, "Segment %s contains %d slices working now, "
							  "it can not afford %d more slices.",
							  GET_SEGRESOURCE_HOSTNAME(curres),
							  curres->SliceWorkload,
							  slicesize);
					impossiblecount++;
				}

				else if ( containerset != NULL &&
						  containerset->Available.MemoryMB >= memory &&
					      containerset->Available.Core     >= core )
				{
					elog(DEBUG3, "Resource manager chooses host %s to allocate vseg.",
								 GET_SEGRESOURCE_HOSTNAME(curres));

					/* Allocate resource. */
					allocateResourceFromSegment(curres,
											 	containerset,
												memory,
												core,
												slicesize);
					/* Reorder the changed host. */
					reorderSegResourceAvailIndex(curres, ratio);

					/*
					 * Check if the selected host has hdfs host name passed in.
					 * If true we just simply add the counter, otherwise, we
					 * create a new segment counter instance.
					 */
					if ( curhost != NULL )
					{
						curhost->VSegmentCount++;
					}
					else
					{
						uint32_t hdfsnameindex = preferredcount;
						int32_t  syncid		   = SEGSTAT_ID_INVALID;

						for ( uint32_t k = 0 ; k < preferredcount ; ++k )
						{
							res=getSegIDByHDFSHostName(preferredhostname[k],
									  	  	  	  	   strlen(preferredhostname[k]),
													   &syncid);
							if(syncid == curres->Stat->ID)
							{
								hdfsnameindex = k;
								break;
							}
						}

						VSegmentCounterInternal vsegcnt =
							createVSegmentCounter(hdfsnameindex, curres);

						if (hdfsnameindex == preferredcount)
						{
							if (debug_print_split_alloc_result)
							{
								elog(LOG, "Segment %s mismatched HDFS host name.",
										  GET_SEGRESOURCE_HOSTNAME(vsegcnt->Resource));
							}
						}

						setHASHTABLENode(&vsegcnttbl,
										 TYPCONVERT(void *, curres->Stat->ID),
										 TYPCONVERT(void *, vsegcnt),
										 false);
					}
					nodecountleft--;
					impossiblecount = 0;
				}
				else
				{
					elog(DEBUG5, "Segment %s does not contain enough resource of "
								 "%d MBPCORE",
								 GET_SEGRESOURCE_HOSTNAME(curres),
								 ratio);
					impossiblecount++;
				}
			}
		}

		if ( impossiblecount >= fullcount )
		{
			if ( skipchosenmachine )
			{
				impossiblecount = 0;
			}
			skipchosenmachine = false;
		}

 		/*
 		 * If the tree goes to 0 nodes, we have to insert all nodes saved in
 		 * tmplist back to the tree to make them ordered naturally again.
 		 */
		if ( nodetree->Root == NULL )
		{
			while( list_length(tmplist) > 0 )
			{
				MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
				insertBBSTNode(nodetree, (BBSTNode)(lfirst(list_head(tmplist))));
				tmplist = list_delete_first(tmplist);
				MEMORY_CONTEXT_SWITCH_BACK
			}
		}
	}

	/*
	 * Insert all nodes saved in tmplist back to the tree to restore the resource
	 * tree for the next time.
	 */
	while( list_length(tmplist) > 0 )
	{
		MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
		insertBBSTNode(nodetree, (BBSTNode)(lfirst(list_head(tmplist))));
		tmplist = list_delete_first(tmplist);
		MEMORY_CONTEXT_SWITCH_BACK
	}

	/* STEP 3. Refresh io bytes workload. */
	*vsegiobytes = (nodecount - nodecountleft) > 0 ?
					iobytes / (nodecount - nodecountleft) :
					0;

	List 	 *vsegcntlist = NULL;
	ListCell *cell		  = NULL;
	getAllPAIRRefIntoList(&vsegcnttbl, &vsegcntlist);
	foreach(cell, vsegcntlist)
	{
		VSegmentCounterInternal vsegcounter = (VSegmentCounterInternal)
											  ((PAIR)(lfirst(cell)))->Value;
		vsegcounter->Resource->IOBytesWorkload +=
									(*vsegiobytes) * vsegcounter->VSegmentCount;
		reorderSegResourceIOBytesWorkloadIndex(vsegcounter->Resource);
	}

    /* STEP 4. Build result. */
	foreach(cell, vsegcntlist)
	{
		MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
			(*vsegcounters) = lappend((*vsegcounters),
									  ((PAIR)(lfirst(cell)))->Value);
		MEMORY_CONTEXT_SWITCH_BACK
	}
	freePAIRRefList(&vsegcnttbl, &vsegcntlist);
	cleanHASHTABLE(&vsegcnttbl);
	*totalvsegcount = nodecount - nodecountleft;

	validateResourcePoolStatus(false);
	return FUNC_RETURN_OK;
}
/**
 * Return the resource to each original hosts.
 */
int returnResourceToResourcePool(int 		memory,
								 double 	core,
								 int64_t 	vsegiobytes,
								 int32_t 	slicesize,
								 List 	  **hosts,
								 bool 		isold)
{
	int				 res	= FUNC_RETURN_OK;
	uint32_t 		 ratio	= 0;
	SegResource		 segres = NULL;
	GRMContainerSet	 ctns	= NULL;
	ListCell		*cell	= NULL;

	Assert( list_length(*hosts) > 0 );

	ratio = memory/core;

	foreach(cell, *hosts)
	{
		VSegmentCounterInternal vsegcnt = (VSegmentCounterInternal)lfirst(cell);
		segres = vsegcnt->Resource;

		if ( !isold )
		{
			/* Get the resource container set. */
			res = getGRMContainerSet(segres, ratio, &ctns);

			/* Recycle the resource to the container set of target ratio */
			res = recycleResourceToSegment(segres,
										   ctns,
										   memory      * vsegcnt->VSegmentCount,
										   core        * vsegcnt->VSegmentCount,
										   vsegiobytes * vsegcnt->VSegmentCount,
										   slicesize   * vsegcnt->VSegmentCount);

			res = reorderSegResourceAvailIndex(segres, ratio);
			Assert(res == FUNC_RETURN_OK);
			res = reorderSegResourceIOBytesWorkloadIndex(segres);
			Assert(res == FUNC_RETURN_OK);
		}
		else
		{
			minusResourceBundleData(&(segres->OldInuse), memory, core);
			Assert( segres->OldInuse.MemoryMB >= 0 );
			Assert( segres->OldInuse.Core  >= 0 );
			elog(LOG, "Resource manager minus (%d MB, %lf CORE) from old in-use "
					  "resource of host %s. (%d MB, %lf CORE) old in-use resource "
					  "remains.",
					  memory,
					  core,
					  GET_SEGRESOURCE_HOSTNAME(segres),
					  segres->OldInuse.MemoryMB,
					  segres->OldInuse.Core);
		}

		/* Free counter instance */
		rm_pfree(PCONTEXT, vsegcnt);

	}

	/* Clean connection track information. */
	list_free(*hosts);
	*hosts = NULL;

	validateResourcePoolStatus(false);
	return FUNC_RETURN_OK;
}

VSegmentCounterInternal createVSegmentCounter(uint32_t 		hdfsnameindex,
											  SegResource	segres)
{
	VSegmentCounterInternal result = rm_palloc0(PCONTEXT,
												sizeof(VSegmentCounterInternalData));
	result->HDFSNameIndex = hdfsnameindex;
	result->Resource	  = segres;
	result->VSegmentCount = 1;
	result->SegId		  = segres->Stat->ID;
	return result;
}

int allocateResourceFromSegment(SegResource 	segres,
							 	GRMContainerSet ctns,
								int32_t			memory,
								double 			core,
								int32_t			slicesize)
{
	Assert( ctns->Available.Core >= core );
	Assert( ctns->Available.MemoryMB >= memory);
	Assert( segres->Available.Core >= core );
	Assert( segres->Available.MemoryMB >= memory);

	minusResourceBundleData(&(ctns->Available), memory, core);
	minusResourceBundleData(&(segres->Available), memory, core);

	segres->SliceWorkload	+= slicesize;

	elog(DEBUG3, "HAWQ RM :: allocated resource from machine %s by "
				 "(%d MB, %lf CORE) for %d slices. "
				 "(%d MB, %lf CORE) Left. "
				 "Workload "INT64_FORMAT" bytes, total %d slices.",
				 GET_SEGRESOURCE_HOSTNAME(segres),
				 memory,
				 core,
				 slicesize,
				 segres->Available.MemoryMB,
				 segres->Available.Core,
				 segres->IOBytesWorkload,
				 segres->SliceWorkload);

	return FUNC_RETURN_OK;
}

int recycleResourceToSegment(SegResource	 segres,
						  	 GRMContainerSet ctns,
							 int32_t		 memory,
							 double			 core,
							 int64_t		 iobytes,
							 int32_t		 slicesize)
{
	segres->IOBytesWorkload -= iobytes;
	segres->SliceWorkload   -= slicesize;

	if ( ctns != NULL )
	{
		addResourceBundleData(&(ctns->Available), memory, core);
		addResourceBundleData(&(segres->Available), memory, core);
		elog(DEBUG3, "HAWQ RM :: returned resource to machine %s by "
					 "(%d MB, %lf CORE) for "INT64_FORMAT" bytes %d slices. "
					 "(%d MB, %lf CORE) Left. "
					 "Workload "INT64_FORMAT" bytes, total %d slices.",
					 GET_SEGRESOURCE_HOSTNAME(segres),
					 memory,
					 core,
					 iobytes,
					 slicesize,
					 segres->Available.MemoryMB,
					 segres->Available.Core,
					 segres->IOBytesWorkload,
					 segres->SliceWorkload);
	}
	else
	{
		elog(DEBUG3, "HAWQ RM :: returned resource to machine %s no resource left. "
					 "Workload "INT64_FORMAT" bytes, total %d slices.",
					 GET_SEGRESOURCE_HOSTNAME(segres),
					 segres->IOBytesWorkload,
					 segres->SliceWorkload);
	}

	return FUNC_RETURN_OK;
}



void addSegResourceAvailIndex(SegResource segres)
{
	int 	 res 	= FUNC_RETURN_OK;
	BBST	 tree	= NULL;
	uint32_t ratio  = 0;
	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i )
	{
		/* Get the BBST for this ratio. */
		ratio = PQUEMGR->RatioReverseIndex[i];
		res = getOrderedResourceAvailTreeIndexByRatio(ratio, &tree);
		if ( res != FUNC_RETURN_OK )
		{
			Assert(false);
		}

		/* Add the node */
		res = insertBBSTNode(tree, createBBSTNode(tree, segres));
		if (res != FUNC_RETURN_OK)
		{
			Assert(false);
		}

		elog(LOG, "Resource manager tracked host %s in available resource "
				  "ordered  index for mem/core ratio %d MBPCORE.",
				  GET_SEGRESOURCE_HOSTNAME(segres),
				  ratio);
	}
}

void addSegResourceAllocIndex(SegResource segres)
{
	int 	 res 	= FUNC_RETURN_OK;
	BBST	 tree	= NULL;
	uint32_t ratio  = 0;
	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i )
	{
		/* Get the BBST for this ratio. */
		ratio = PQUEMGR->RatioReverseIndex[i];
		res = getOrderedResourceAllocTreeIndexByRatio(ratio, &tree);
		if ( res != FUNC_RETURN_OK )
		{
			Assert(false);
		}

		/* Add the node */
		res = insertBBSTNode(tree, createBBSTNode(tree, segres));
		if (res != FUNC_RETURN_OK)
		{
			Assert(false);
		}

		elog(LOG, "Resource manager tracked host %s in allocated resource "
				  "ordered  index for mem/core ratio %d MBPCORE.",
				  GET_SEGRESOURCE_HOSTNAME(segres),
				  ratio);
	}
}

void addSegResourceIOBytesWorkloadIndex(SegResource segres)
{
	/* Add the node */
	int res = insertBBSTNode(&(PRESPOOL->OrderedIOBytesWorkload),
						 	 createBBSTNode(&(PRESPOOL->OrderedIOBytesWorkload),
						 			 	 	segres));
	if (res != FUNC_RETURN_OK)
	{
		Assert(false);
	}

	elog(LOG, "Resource manager tracked host %s in io bytes workload.",
			  GET_SEGRESOURCE_HOSTNAME(segres));
}

int reorderSegResourceAvailIndex(SegResource segres, uint32_t ratio)
{
	int 	 res 	= FUNC_RETURN_OK;
	BBST	 tree	= NULL;
	/* Get the BBST for this ratio. */
	res = getOrderedResourceAvailTreeIndexByRatio(ratio, &tree);
	if ( res == RESOURCEPOOL_NO_RATIO )
	{
		return res; /* If it is to reorder a node, the BBST must already exist.*/
	}

	return reorderBBSTNodeData(tree, segres);
}

int reorderSegResourceAllocIndex(SegResource segres, uint32_t ratio)
{
	int 	 res 	= FUNC_RETURN_OK;
	BBST	 tree	= NULL;
	/* Get the BBST for this ratio. */
	res = getOrderedResourceAllocTreeIndexByRatio(ratio, &tree);
	if ( res == RESOURCEPOOL_NO_RATIO )
	{
		return res; /* If it is to reorder a node, the BBST must already exist.*/
	}

	return reorderBBSTNodeData(tree, segres);
}

int reorderSegResourceIOBytesWorkloadIndex(SegResource segres)
{
	int 	 res 	= FUNC_RETURN_OK;
	BBSTNode node	= NULL;

	/* Reorder the node */
	node = getBBSTNode(&(PRESPOOL->OrderedIOBytesWorkload), segres);
	if ( node == NULL )
	{
		return RESOURCEPOOL_INTERNAL_NO_HOST_INDEX;
	}

	res = removeBBSTNode(&(PRESPOOL->OrderedIOBytesWorkload), &node);
	if ( res != FUNC_RETURN_OK )
	{
		return RESOURCEPOOL_INTERNAL_NO_HOST_INDEX;
	}
	res = insertBBSTNode(&(PRESPOOL->OrderedIOBytesWorkload), node);

	if ( res == UTIL_BBST_DUPLICATE_VALUE )
	{
		res = RESOURCEPOOL_INTERNAL_DUPLICATE_HOST;
	}
	else
	{
		Assert ( res == FUNC_RETURN_OK );
	}
	return res;
}

int getSegIDByHDFSHostName(const char *hostname, int hostnamelen, int32_t *id)
{
	return getSegIDByHostNameInternal(&(PRESPOOL->HDFSHostNameIndexed),
									  hostname,
									  hostnamelen,
									  id);
}

int getSegIDByGRMHostName(const char *hostname, int hostnamelen, int32_t *id)
{
	return getSegIDByHostNameInternal(&(PRESPOOL->GRMHostNameIndexed),
									  hostname,
									  hostnamelen,
									  id);
}

int getSegIDByHostNameInternal(HASHTABLE   hashtable,
							   const char *hostname,
							   int 		   hostnamelen,
							   int32_t    *id)
{
	int 		  res 			= FUNC_RETURN_OK;
	List 	   	 *gottenaddr	= NULL;
	ListCell   	 *addrcell		= NULL;
	AddressString addr			= NULL;
	SimpString 	  ohostname;
	SimpString    key;

	Assert( hashtable != NULL );

	*id = SEGSTAT_ID_INVALID;
	initSimpleString(&ohostname, PCONTEXT);

	/* Try if this is already saved. */
	setSimpleStringRef(&key, (char *)hostname, hostnamelen);
	PAIR pair = getHASHTABLENode(hashtable, (void *)&key);
	if ( pair != NULL )
	{
		*id = TYPCONVERT(uint32_t, pair->Value);
		return FUNC_RETURN_OK;
	}

	/* Try to solve this host name. */
	res = getHostIPV4AddressesByHostNameAsString(PCONTEXT,
												 hostname,
												 &ohostname,
												 &gottenaddr);
	if ( res != FUNC_RETURN_OK )
	{
		elog(WARNING, "Resource manager can not resolve host name %s", hostname);
		goto exit;
	}

	/* Try to use official host name to get hawa node/host. */
	res = getSegIDByHostName(ohostname.Str, ohostname.Len, id);
	if ( res == FUNC_RETURN_OK )
	{
		elog(DEBUG3, "Resource manager found host %s as host officially %s.",
					 hostname,
					 ohostname.Str);
		goto exit;
	}

	/* Try each ip address to get hawq node/host. */
	foreach(addrcell, gottenaddr)
	{
		addr = (AddressString)lfirst(addrcell);
		res = getSegIDByHostAddr((uint8_t *)(addr->Address), addr->Length, id);
		if ( res == FUNC_RETURN_OK )
		{
			elog(DEBUG3, "Resource manager found host %s identified by "
						 "address %s.",
						 hostname,
						 addr->Address);
			goto exit;
		}
	}

	res = RESOURCEPOOL_UNRESOLVED_HOST;

exit:
	/* Clean up. */
	freeHostIPV4AddressesAsString(PCONTEXT, &gottenaddr);
	freeSimpleStringContent(&ohostname);

	if ( res == FUNC_RETURN_OK )
	{
		setHASHTABLENode(hashtable,
						 &key,
						 TYPCONVERT(void *, *id),
						 false);
	}
	return res;
}

/*
 * Iterate all containers and return it.
 */
void returnAllGRMResourceFromSegment(SegResource segres)
{
	Assert(segres != NULL);
	GRMContainer 	ctn   = NULL;
	GRMContainerSet ctns  = NULL;
	uint32_t 		count = 0;

	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i )
	{
		ctns = segres->ContainerSets[i];
		if ( ctns == NULL )
		{
			continue;
		}

		while (ctns->Containers != NULL)
		{
			ctn = popGRMContainerSetContainerList(ctns);
			addGRMContainerToKicked(ctn);
			minusResourceBundleData(&(segres->Allocated), ctn->MemoryMB, ctn->Core);
			minusResourceBundleData(&(segres->Available), ctn->MemoryMB, ctn->Core);
			count++;
		}

		reorderSegResourceAllocIndex(segres, PQUEMGR->RatioReverseIndex[i]);
		reorderSegResourceAvailIndex(segres, PQUEMGR->RatioReverseIndex[i]);
	}

	Assert(segres->Allocated.MemoryMB == 0);
	Assert(segres->Allocated.Core == 0.0);
	segres->GRMContainerCount = 0;

	elog(DEBUG3, "HAWQ RM: returnAllResourceForSegment: %u containers have been "
				 "removed for machine internal id:%u",
				 count,
				 segres->Stat->ID);

	validateResourcePoolStatus(false);
}

/*
 * Go through each segment, and return the GRM containers in those segments
 * global resource manager unavailable.
 */
void returnAllGRMResourceFromGRMUnavailableSegments(void)
{
	List 	 *allsegres = NULL;
	ListCell *cell		= NULL;

	getAllPAIRRefIntoList(&(PRESPOOL->Segments), &allsegres);

	foreach(cell, allsegres)
	{
		SegResource segres = (SegResource)(((PAIR)lfirst(cell))->Value);
		if ( IS_SEGSTAT_GRMAVAILABLE(segres->Stat) )
		{
			continue;
		}
		minusResourceFromResourceManagerByBundle(&(segres->Allocated));
		returnAllGRMResourceFromSegment(segres);
	}
	freePAIRRefList(&(PRESPOOL->Segments), &allsegres);
}

void dropAllGRMContainersFromSegment(SegResource segres)
{
	Assert(segres != NULL);
	GRMContainer 	 ctn 	 = NULL;
	GRMContainerSet  ctns 	 = NULL;
	uint32_t 		 count 	 = 0;

	/* Add current in use resource to old in use counter. */
	addResourceBundleDataByBundle(&(segres->OldInuse), &(segres->Allocated));
	minusResourceBundleDataByBundle(&(segres->OldInuse), &(segres->Available));
	Assert(segres->OldInuse.MemoryMB >= 0);
	Assert(segres->OldInuse.Core >= 0);

	elog(LOG, "Resource manager sets host %s old in-used resource (%d MB, %lf CORE).",
			  GET_SEGRESOURCE_HOSTNAME(segres),
			  segres->OldInuse.MemoryMB,
			  segres->OldInuse.Core);

	/* Drop all containers to to kick list. */
	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i )
	{
		ctns = segres->ContainerSets[i];
		if ( ctns == NULL )
		{
			continue;
		}

		while (ctns->Containers != NULL)
		{
			ctn = popGRMContainerSetContainerList(ctns);
			elog(LOG, "Resource manager dropped container (%d MB, %d CORE) in host %s",
					  ctn->MemoryMB,
					  ctn->Core,
					  ctn->HostName);
			addGRMContainerToToBeKicked(ctn);

			count++;

			elog(LOG, "Resource manager decides to return container %d in host %s "
					  "in order to drop all resource pool's GRM containers.",
					  ctn->ID,
					  ctn->HostName);
		}

		resetResourceBundleData(&(ctns->Allocated), 0, 0.0, 0);
		resetResourceBundleData(&(ctns->Available), 0, 0.0, 0);

		reorderSegResourceAllocIndex(segres, PQUEMGR->RatioReverseIndex[i]);
		reorderSegResourceAvailIndex(segres, PQUEMGR->RatioReverseIndex[i]);
	}

	elog(LOG, "Resource manager cleared %u containers, old in-use resource "
				  "is set (%d MB, %lf CORE)",
				  count,
				  segres->OldInuse.MemoryMB,
				  segres->OldInuse.Core);

	resetResourceBundleData(&(segres->Allocated), 0, 0.0, 0);
	resetResourceBundleData(&(segres->Available), 0, 0.0, 0);
	segres->GRMContainerCount = 0;
	validateResourcePoolStatus(false);
}


/*
 * Request RMSEGs to increase memory quota according to resource containers added.
 */
int notifyToBeAcceptedGRMContainersToRMSEG(void)
{
	int		  res   = FUNC_RETURN_OK;
	List	 *ctnss = NULL;
	ListCell *cell  = NULL;

	getAllPAIRRefIntoList(&(PRESPOOL->ToAcceptContainers), &ctnss);

	foreach(cell, ctnss)
	{
		GRMContainerSet ctns = (GRMContainerSet)(((PAIR)lfirst(cell))->Value);

		if ( ctns->Allocated.MemoryMB == 0 && ctns->Allocated.Core == 0 )
		{
			continue;
		}

		GRMContainer firstctn = getGRMContainerSetContainerFirst(ctns);
		Assert(firstctn != NULL);
		char *hostname = firstctn->HostName;

		if (rm_resourcepool_test_filename == NULL ||
			rm_resourcepool_test_filename[0] == '\0')
		{
			res = increaseMemoryQuota(hostname, ctns);
			if ( res != FUNC_RETURN_OK )
			{
				elog(LOG, "Resource manager failed to increase memory quota on "
						  "host %s.", hostname);
			}
		}
		/* Skip memory quota increase in fault injection mode for RM test */
		else
		{
			moveGRMContainerSetToAccepted(ctns);
		}
	}
	freePAIRRefList(&(PRESPOOL->ToAcceptContainers), &ctnss);
	return FUNC_RETURN_OK;
}

/*
 * Request RMSEGs to decrease memory quota according to
 * resource containers added
 */
int notifyToBeKickedGRMContainersToRMSEG(void)
{
	int		  res   = FUNC_RETURN_OK;
	List	 *ctnss = NULL;
	ListCell *cell  = NULL;

	getAllPAIRRefIntoList(&(PRESPOOL->ToKickContainers), &ctnss);

	foreach(cell, ctnss)
	{
		GRMContainerSet ctns = (GRMContainerSet)(((PAIR)lfirst(cell))->Value);

		if (ctns->Allocated.Core == 0 || ctns->Allocated.MemoryMB == 0)
		{
			continue;
		}

		GRMContainer firstctn = getGRMContainerSetContainerFirst(ctns);
		Assert(firstctn != NULL);
		char *hostname = firstctn->HostName;

		if (rm_resourcepool_test_filename == NULL ||
			rm_resourcepool_test_filename[0] == '\0')
		{
			res = decreaseMemoryQuota(hostname, ctns);

			if ( res != FUNC_RETURN_OK )
			{
				elog(LOG, "Resource manager failed to decrease memory quota on "
						  "host %s",
						  hostname);
			}
		}
		/* Skip memory quota increase in fault injection mode for RM test. */
		else
		{
			moveGRMContainerSetToKicked(ctns);
		}
	}

	freePAIRRefList(&(PRESPOOL->ToKickContainers), &ctnss);
	return FUNC_RETURN_OK;
}

void moveAllAcceptedGRMContainersToResPool(void)
{
	while( PRESPOOL->AcceptedContainers != NULL )
	{
		GRMContainer ctn = (GRMContainer)
						   lfirst(list_head(PRESPOOL->AcceptedContainers));
		MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
		PRESPOOL->AcceptedContainers = list_delete_first(PRESPOOL->AcceptedContainers);
		MEMORY_CONTEXT_SWITCH_BACK

		addGRMContainerToResPool(ctn);
		PRESPOOL->AddPendingContainerCount--;
		elog(LOG, "AddPendingContainerCount minused 1, current value %d",
				  PRESPOOL->AddPendingContainerCount);
		addNewResourceToResourceManager(ctn->MemoryMB, ctn->Core);
		removePendingResourceRequestInRootQueue(ctn->MemoryMB, ctn->Core);
	}
	validateResourcePoolStatus(true);
}

void timeoutIdleGRMResourceToRB(void)
{
	/*
	 * No need to return resource when in NONE mode, we HAWQ RM exclusively uses
	 * all resource.
	 */
	if ( DRMGlobalInstance->ImpType == NONE_HAWQ2 )
	{
		return;
	}

	/* Choose host to return resource. The idea of choosing container to return
	 * is to choose the machine containing most resource. */
	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i )
	{

		if ( PQUEMGR->RatioWaterMarks[i].NodeCount == 0 )
		{
			continue;
		}

		/* If there are resource requests, we dont timeout resource. */
		if ( PQUEMGR->RatioTrackers[i]->TotalRequest.MemoryMB > 0 ||
			 PQUEMGR->RatioTrackers[i]->TotalPending.MemoryMB > 0 )
		{
			continue;
		}

		uint32_t ratio = PQUEMGR->RatioTrackers[i]->MemCoreRatio;

		DynMemoryCoreRatioWaterMark mark =
			(DynMemoryCoreRatioWaterMark)
			getDQueueContainerData(
				getDQueueContainerHead(&(PQUEMGR->RatioWaterMarks[i])));

		/* Get quota to return. */
		double   retcore = 0;

		/* We want to leave some resource as minimum reserved quota. */
		int32_t idlereqmem  = 0;
		double  idlereqcore = 0.0;
		getIdleResourceRequest(&idlereqmem, &idlereqcore);

		if ( mark->ClusterVCore > 0 )
		{
			retcore = mark->ClusterVCore < idlereqcore ?
					  PQUEMGR->RatioTrackers[i]->TotalAllocated.Core - idlereqcore :
					  PQUEMGR->RatioTrackers[i]->TotalAllocated.Core - mark->ClusterVCore;
		}
		else
		{
			retcore = PQUEMGR->RatioTrackers[i]->TotalAllocated.Core;
		}

		/* If no need to return the resource. */
		if ( retcore <= 0 )
		{
			continue;
		}

		/* Get integer number of global resource manager container. */
		uint32_t retcontnum     = trunc(retcore);
		uint32_t realretcontnum = 0;

		/* Select containers from resource pool to return.
		 * TODO: We expect each container has 1 core, this maybe false when the
		 * segment resource quota is set larger than 1 core. */
		if ( retcontnum > 0 )
		{

			elog(LOG, "Resource manager decides to timeout %u resource containers "
					  "in cluster including %u healthy nodes.",
					  retcontnum,
					  PRESPOOL->AvailNodeCount);

			timeoutIdleGRMResourceToRBByRatio(i,
										   	  retcontnum,
											  &realretcontnum,
											  mark->ClusterVCore > 0 ? 2 : 0 );
			if ( realretcontnum > 0 )
			{
				/* Notify resource queue manager to minus allocated resource.*/
				minusResourceFromReourceManager(realretcontnum * ratio,
												realretcontnum * 1);

				elog(LOG, "Resource manager chose %d resource containers to "
						  "return actually.",
						  realretcontnum);
			}
		}
	}
	validateResourcePoolStatus(true);
}

void forceReturnGRMResourceToRB(void)
{
	Assert(PQUEMGR->RatioCount == 1);
	uint32_t realretcontnum = 0;
	timeoutIdleGRMResourceToRBByRatio(0,
									  PQUEMGR->ForcedReturnGRMContainerCount,
									  &realretcontnum,
									  0);
	PQUEMGR->ForcedReturnGRMContainerCount -= realretcontnum;
	Assert(PQUEMGR->ForcedReturnGRMContainerCount >= 0);

	if ( realretcontnum > 0 )
	{
		/* Notify resource queue manager to minus allocated resource.*/
		minusResourceFromReourceManager(realretcontnum * PQUEMGR->RatioReverseIndex[0],
										realretcontnum * 1);

		elog(LOG, "Resource manager forced %d resource containers to "
				  "return actually.",
				  realretcontnum);
	}

	elog( LOG, "Resource pool returned %d GRM containers to breathe out resource.",
			   realretcontnum);
}

void timeoutIdleGRMResourceToRBByRatio(int 		 ratioindex,
								       uint32_t	 retcontnum,
									   uint32_t *realretcontnum,
									   int		 segminnum)
{
	int 		res 	= FUNC_RETURN_OK;
	BBST		tree	= NULL;
	BBSTNode	node	= NULL;
	uint32_t   	ratio	= PQUEMGR->RatioTrackers[ratioindex]->MemCoreRatio;
	DQueueData  tempskipnodes;

	initializeDQueue(&tempskipnodes, PCONTEXT);

	*realretcontnum = 0;

	/* Get the BBST for this ratio. */
	res = getOrderedResourceAllocTreeIndexByRatio(ratio, &tree);
	if ( res == RESOURCEPOOL_NO_RATIO )
	{
		elog(LOG, "No resource allocated of %d MB per core in resource pool.",
				  ratio);
		return;
	}

	/* Get the container to return one by one. */
	for ( int i = 0 ; i < retcontnum && tree->Root != NULL ; ++i )
	{
		/* Get the node containing most resource. */
		node = getLeftMostNode(tree);
		Assert( node != NULL );
		SegResource resource = (SegResource)(node->Data);
		Assert( resource != NULL );

		/* Get the container set of expected ratio. */
		GRMContainerSet containerset = NULL;

		res = getGRMContainerSet(resource, ratio, &containerset);
		if ( res != FUNC_RETURN_OK )
		{
			/* This machine does not have the resource with matching ratio.*/
			elog(DEBUG3, "Host %s does not contain expected resource of %d MBPCORE. "
					     "No need to check left hosts.",
						 GET_SEGRESOURCE_HOSTNAME(resource),
						 ratio);
			break;
		}

		/* If too few containers, break the loop. */
		if ( list_length(containerset->Containers) <= segminnum )
		{
			/* This machine does not have enough containers to return.*/
			elog(DEBUG3, "Host %s does not contain at least one resource container "
						 "for returning resource to global resource manager. "
					     "No need to check left hosts.",
						 GET_SEGRESOURCE_HOSTNAME(resource));
			break;
		}

		/* Get the container to return. */
		Assert( containerset->Containers != NULL );
		GRMContainer retcont = getGRMContainerSetContainerFirst(containerset);

		if ( containerset->Available.MemoryMB >= retcont->MemoryMB &&
			 containerset->Available.Core     >= retcont->Core )
		{

			Assert(resource->Available.MemoryMB >= retcont->MemoryMB);
			Assert(resource->Available.Core     >= retcont->Core);

			retcont = popGRMContainerSetContainerList(containerset);

			minusResourceBundleData(&(resource->Allocated),
									retcont->MemoryMB,
									retcont->Core);
			minusResourceBundleData(&(resource->Available),
									retcont->MemoryMB,
									retcont->Core);
			resource->GRMContainerCount--;

			Assert( resource->Allocated.MemoryMB >= 0 );
			Assert( resource->Allocated.Core >= 0  );
			Assert( resource->Available.MemoryMB >= 0 );
			Assert( resource->Available.Core >= 0  );

			Assert( containerset->Allocated.MemoryMB >= 0 );
			Assert( containerset->Allocated.Core >= 0   );
			Assert( containerset->Available.MemoryMB >= 0 );
			Assert( containerset->Available.Core >= 0   );

			reorderSegResourceAllocIndex(resource, ratio);
			reorderSegResourceAvailIndex(resource, ratio);

			addGRMContainerToToBeKicked(retcont);
			(*realretcontnum)++;
			elog(LOG, "Resource manager decides to return container %d in host %s",
					  retcont->ID,
					  retcont->HostName);
			validateResourcePoolStatus(false);
		}
		else
		{
			/*
			 * Remove this node temporarily, we will add this back after timeout
			 * processing.
			 */
			BBSTNode removenode = node;
			removeBBSTNode(tree, &removenode);

			elog(DEBUG3, "Host %s is busy to return resource to global resource "
						 "manager. Skip this host temporarily.",
						 GET_SEGRESOURCE_HOSTNAME(resource));

			insertDQueueTailNode(&tempskipnodes, removenode);
		}
	}

	while( tempskipnodes.NodeCount > 0 )
	{
		insertBBSTNode(tree, (BBSTNode)removeDQueueHeadNode(&tempskipnodes));
	}
}

bool hasSegmentGRMCapacityNotUpdated(void)
{
	if (DRMGlobalInstance->ImpType == NONE_HAWQ2)
	{
		return false;
	}
	bool res = false;

	List 	 *allsegres = NULL;
	ListCell *cell	    = NULL;
	getAllPAIRRefIntoList(&(PRESPOOL->Segments), &allsegres);
	foreach(cell, allsegres)
	{
		SegResource segresource = (SegResource)(((PAIR)lfirst(cell))->Value);
		if ( segresource->Stat->GRMTotalMemoryMB == 0 ||
			 segresource->Stat->GRMTotalCore == 0 )
		{
			res = true;
			break;
		}
	}
	freePAIRRefList(&(PRESPOOL->Segments), &allsegres);
	return res;
}

bool allSegmentHasNoGRMContainersAllocated(void)
{
	bool	  res		= true;
	List 	 *allsegres = NULL;
	ListCell *cell	    = NULL;
	getAllPAIRRefIntoList(&(PRESPOOL->Segments), &allsegres);
	foreach(cell, allsegres)
	{
		SegResource segresource = (SegResource)(((PAIR)lfirst(cell))->Value);
		if (segresource->Allocated.MemoryMB > 0 ||
			segresource->Allocated.Core > 0 )
		{
			elog(DEBUG3, "Segment %s contains allocated resource.",
						 GET_SEGRESOURCE_HOSTNAME(segresource));
			res = false;
			break;
		}
	}
	freePAIRRefList(&(PRESPOOL->Segments), &allsegres);
	return res;
}

bool setSegResRUAlivePending( SegResource segres, bool pending)
{
	Assert( segres != NULL );
	int res = segres->RUAlivePending;
	if ( res == pending )
	{
		return res;
	}
	segres->RUAlivePending = pending;

	if ( PQUEMGR->RatioCount == 1 )
	{
		uint32_t ratio = PQUEMGR->RatioReverseIndex[0];
		reorderSegResourceAllocIndex(segres, ratio);
		reorderSegResourceAvailIndex(segres, ratio);
	}
	return res;
}

uint32_t getSegResourceCapacityMemory( SegResource segres )
{
	switch( DRMGlobalInstance->ImpType )
	{
	case YARN_LIBYARN:
		return segres->Stat->GRMTotalMemoryMB;
	case NONE_HAWQ2:
		return segres->Stat->FTSTotalMemoryMB;
	default:
		Assert(false);
	}
	return 0;
}

uint32_t getSegResourceCapacityCore( SegResource segres )
{
	switch( DRMGlobalInstance->ImpType )
	{
	case YARN_LIBYARN:
		return segres->Stat->GRMTotalCore;
	case NONE_HAWQ2:
		return segres->Stat->FTSTotalCore;
	default:
		Assert(false);
	}
	return 0;
}

void checkGRMContainerStatus(RB_GRMContainerStat ctnstats, int size)
{
	/* Build container status hash table. */
	HASHTABLEData stattbl;
	initializeHASHTABLE(&stattbl,
						PCONTEXT,
						HASHTABLE_SLOT_VOLUME_DEFAULT,
						HASHTABLE_SLOT_VOLUME_DEFAULT_MAX,
						HASHTABLE_KEYTYPE_UINT32,
						NULL);
	for ( int i = 0 ; i < size ; ++i )
	{
		elog(DEBUG3, "Resource manager tracks container %d.",
					 ctnstats[i].ContainerID);
		setHASHTABLENode(&stattbl,
						 TYPCONVERT(void *, ctnstats[i].ContainerID),
						 &ctnstats[i],
						 false);
		ctnstats[i].isFound = false;
	}

	MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
	/* Go through all current accepted GRM containers to check its status. */
	for( int i = 0 ; i < PRESPOOL->SegmentIDCounter ; ++i )
	{
		SegResource segres = getSegResource(i);
		Assert(segres != NULL);

		for( int ridx = 0 ; ridx < PQUEMGR->RatioCount ; ++ridx )
		{
			GRMContainerSet ctnset = segres->ContainerSets[ridx];
			if ( ctnset == NULL )
			{
				continue;
			}

			ListCell *cell = list_head(ctnset->Containers);
			ListCell *prev = NULL;
			while( cell != NULL )
			{
				GRMContainer ctn = (GRMContainer)lfirst(cell);
				PAIR pair = (PAIR)getHASHTABLENode(&(stattbl),
												   TYPCONVERT(void *, ctn->ID));
				RB_GRMContainerStat ctnstat = pair != NULL ?
											  (RB_GRMContainerStat)(pair->Value):
											  NULL;

				if ( ctnstat == NULL || !(ctnstat->isActive) )
				{
					/*
					 * This container is not in report, or the container is not
					 * active. It should be returned.
					 */
					GRMContainer ctn = (GRMContainer)lfirst(cell);
					ctnset->Containers = list_delete_cell(ctnset->Containers,
														  cell,
														  prev);
					cell = prev ? lnext(prev) : list_head(ctnset->Containers);

					/*
					 * Minus this container resource quota from resource pool
					 * and resource queue manager.
					 */
					minusResourceBundleData(&(segres->Allocated), ctn->MemoryMB, ctn->Core);
					minusResourceBundleData(&(segres->Available), ctn->MemoryMB, ctn->Core);

					Assert( segres->Allocated.MemoryMB >= 0 );
					Assert( segres->Allocated.Core     >= 0 );
					Assert( segres->Available.MemoryMB >= 0 );
					Assert( segres->Available.Core     >= 0 );

					minusResourceBundleData(&(ctnset->Allocated), ctn->MemoryMB, ctn->Core);
					minusResourceBundleData(&(ctnset->Available), ctn->MemoryMB, ctn->Core);

					Assert( ctnset->Allocated.MemoryMB >= 0 );
					Assert( ctnset->Allocated.Core     >= 0 );
					Assert( ctnset->Available.MemoryMB >= 0 );
					Assert( ctnset->Available.Core     >= 0 );

					reorderSegResourceAllocIndex(segres, PQUEMGR->RatioReverseIndex[ridx]);
					reorderSegResourceAvailIndex(segres, PQUEMGR->RatioReverseIndex[ridx]);

					addGRMContainerToToBeKicked(ctn);
					elog(LOG, "Resource manager decides to return container %d "
							  "in host %s because %s.",
							  ctn->ID,
							  ctn->HostName,
							  ctnstat == NULL ?
								  "it is not tracked by YARN" :
								  "it is not treated active in YARN");

					minusResourceFromReourceManager(ctn->MemoryMB, ctn->Core);
					validateResourcePoolStatus(true);
				}
				else
				{
					elog(DEBUG3, "Resource manager set container %d found.",
								 ctnstat->ContainerID);
					/*
					 * For all containers not tracked by hawq rm, they are marked
					 * as isFound = false.
					 */
					ctnstat->isFound = true;

					prev = cell;
					cell = lnext(cell);
				}
			}
		}
	}
	MEMORY_CONTEXT_SWITCH_BACK

	/* Cleanup hashtable. */
	cleanHASHTABLE(&stattbl);
}

void freeVSegmentConterList(List **list)
{
	while( list_length(*list) > 0 )
	{
		VSegmentCounterInternal vsegcnt = (VSegmentCounterInternal)lfirst(list_head(*list));
		MEMORY_CONTEXT_SWITCH_TO(PCONTEXT)
		*list = list_delete_first(*list);
		MEMORY_CONTEXT_SWITCH_BACK
		rm_pfree(PCONTEXT, vsegcnt);
	}
}

void dropAllResPoolGRMContainersToToBeKicked(void)
{
	elog(LOG, "Resource manager drops all allocated resource per request from "
			  "resource broker error.");

	/* Mark old resource quota level and clear all containers in all hosts. */
	for(uint32_t idx = 0; idx < PRESPOOL->SegmentIDCounter; idx++)
	{
	    SegResource segres = getSegResource(idx);

	    /* Minus resource from resource queue resource quota. */
	    minusResourceFromResourceManagerByBundle(&(segres->Allocated));
		/* This call makes resource pool remove unused containers. */
		dropAllGRMContainersFromSegment(segres);
	}

	validateResourcePoolStatus(true);

	/* Clear pending resource allocation request to resource broker. */
	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i )
	{
		resetResourceBundleData(&(PQUEMGR->RatioTrackers[i]->TotalPending), 0, 0, -1);
		PQUEMGR->RatioTrackers[i]->TotalPendingStartTime = 0;
	}

	refreshMemoryCoreRatioLevelUsage(gettime_microsec());

	validateResourcePoolStatus(true);
}

void dropAllToAcceptGRMContainersToKicked(void)
{
	List	 *ctnslst 	= NULL;
	ListCell *cell  	= NULL;

	getAllPAIRRefIntoList(&(PRESPOOL->ToAcceptContainers), &ctnslst);
	foreach(cell, ctnslst)
	{
		GRMContainerSet ctns = (GRMContainerSet)(((PAIR)lfirst(cell))->Value);

		if ( ctns->Allocated.MemoryMB == 0 && ctns->Allocated.Core == 0 )
		{
			continue;
		}

		Assert( ctns->Containers != NULL );
		/* Check the first container to get segment resource instance. */
		GRMContainer ctn1st = (GRMContainer)lfirst(list_head(ctns->Containers));
		minusResourceBundleDataByBundle(&(ctn1st->Resource->IncPending),
										&(ctns->Allocated));
		PRESPOOL->AddPendingContainerCount -= list_length(ctns->Containers);
		moveGRMContainerSetToKicked(ctns);
	}
	freePAIRRefList(&(PRESPOOL->ToAcceptContainers), &ctnslst);
}

/*******************************************************************************
 * Code for validating the health of resource pool.
 ******************************************************************************/
void validateResourcePoolStatus(bool refquemgr)
{

	int32_t  totalallocmem  = 0;
	double	 totalalloccore = 0.0;
	int32_t  totalavailmem  = 0;
	double   totalavailcore = 0.0;

	/* Check each host. */
	for ( int i = 0 ; i < PRESPOOL->Segments.SlotVolume ; ++i )
	{
		List 	 *slot = PRESPOOL->Segments.Slots[i];
		ListCell *cell = NULL;
		foreach(cell, slot)
		{
			SegResource segres = (SegResource)(((PAIR)lfirst(cell))->Value);
			/* Validation 1. All resource info has allocated resource counter and
			 * 				 available resource counter correct. */
			int32_t  allocmem  = 0;
			double   alloccore = 0;
			int32_t  availmem  = 0;
			double   availcore = 0;
			getSegResResourceCountersByMemCoreCounters(segres,
													   &allocmem,
													   &alloccore,
													   &availmem,
													   &availcore);
			if ( segres->Allocated.MemoryMB != allocmem ||
				 segres->Allocated.Core   	!= alloccore ||
				 segres->Available.MemoryMB != availmem ||
				 segres->Available.Core   	!= availcore )
			{
				elog(ERROR, "HAWQ RM Validation. Wrong resource counter. "
							"Host %s. "
							"Expect allocated (%d MB, %lf CORE) "
							       "available (%d MB, %lf CORE),"
							"ContainerSet allocated (%d MB, %lf CORE) "
								   	   	 "available (%d MB, %lf CORE)",
							GET_SEGRESOURCE_HOSTNAME(segres),
							segres->Allocated.MemoryMB,
							segres->Allocated.Core,
							segres->Available.MemoryMB,
							segres->Available.Core,
							allocmem,
							alloccore,
							availmem,
							availcore);
			}

			/* Validation 2. The ratio should be correct. */
			if ( (allocmem == 0 && alloccore != 0) ||
				 (allocmem != 0 && alloccore == 0) ||
				 (availmem == 0 && availcore != 0) ||
				 (availmem != 0 && availcore == 0) ||
				 (alloccore != 0 && availcore != 0 &&
				  trunc(allocmem/alloccore) != trunc(availmem/availcore)))
			{
				elog(ERROR, "HAWQ RM Validation. Wrong resource counter ratio. "
							"Host %s. "
						    "Allocated (%d MB, %lf CORE) "
							"available (%d MB, %lf CORE),",
							GET_SEGRESOURCE_HOSTNAME(segres),
							segres->Allocated.MemoryMB,
							segres->Allocated.Core,
							segres->Available.MemoryMB,
							segres->Available.Core);
			}

			/* Validation 3. The ratio now must be unique. */
			if ( PQUEMGR->RatioCount > 1 )
			{
				elog(ERROR, "HAWQ RM Validation. More than 1 mem/core ratio. ");
			}

			totalallocmem  += allocmem;
			totalalloccore += alloccore;
			totalavailmem  += availmem;
			totalavailcore += availcore;

		}
	}

	/*
	 * Validation 4. The total allocated should be no more than the capacity of
	 * whole cluster.
	 */
	int32_t  mem  = 0;
	uint32_t core = 0;

	if ( PQUEMGR->RootTrack != NULL )
	{

		if ( DRMGlobalInstance->ImpType == YARN_LIBYARN )
		{
			mem  = PRESPOOL->GRMTotal.MemoryMB * PQUEMGR->GRMQueueMaxCapacity;
			core = PRESPOOL->GRMTotal.Core     * PQUEMGR->GRMQueueMaxCapacity;
		}
		else if ( DRMGlobalInstance->ImpType == NONE_HAWQ2 )
		{
			mem  = PRESPOOL->FTSTotal.MemoryMB;
			core = PRESPOOL->FTSTotal.Core;
		}
		else
		{
			Assert(false);
		}
	}
	else
	{
		return;
	}

	if ( totalallocmem > mem || totalalloccore > core )
	{
		elog(ERROR, "HAWQ RM Validation. Allocated too much resource in resource "
					"pool. (%d MB, %lf CORE)",
					totalallocmem,
					totalalloccore);
	}

	/*
	 * Validation 5. The total allocated and available resource should match
	 * resource queue capacity in resource queue manager.
	 */

	if ( refquemgr && PQUEMGR->RatioCount == 1 )
	{
		Assert( PQUEMGR->RatioTrackers[0] != NULL );

		if ( PQUEMGR->RatioTrackers[0]->TotalAllocated.MemoryMB != totalallocmem ||
			 PQUEMGR->RatioTrackers[0]->TotalAllocated.Core     != totalalloccore )
		{
			elog(ERROR, "HAWQ RM Validation. Wrong total allocated resource. "
						"In resource pool allocated (%d MB, %lf CORE), "
						"In resource queue manager allocated (%d MB, %lf CORE).",
						totalallocmem,
						totalalloccore,
						PQUEMGR->RatioTrackers[0]->TotalAllocated.MemoryMB,
						PQUEMGR->RatioTrackers[0]->TotalAllocated.Core);
		}

		if ( totalavailmem > totalallocmem || totalavailcore > totalalloccore )
		{
				elog(ERROR, "HAWQ RM Validation. Wrong total allocated resource. "
							"In resource pool available (%d MB, %lf CORE), "
							"In resource pool allocated (%d MB, %lf CORE).",
							totalavailmem,
							totalavailcore,
							totalallocmem,
							totalalloccore);
		}
	}

	if ( PQUEMGR->RatioCount == 1 )
	{
		/* Validation 6. The available resource index should be well organized. */
		BBST 	   availtree = NULL;
		DQueueData line;
		initializeDQueue(&line, PCONTEXT);
		if ( getOrderedResourceAvailTreeIndexByRatio(PQUEMGR->RatioReverseIndex[0],
													 &availtree) == FUNC_RETURN_OK )
		{
			Assert( availtree != NULL );
			traverseBBSTMidOrder(availtree, &line);

			if ( line.NodeCount != PRESPOOL->Segments.NodeCount )
			{
				elog(ERROR, "HAWQ RM Validation. The available resource ordered index "
							"contains %d nodes, expect %d nodes.",
							line.NodeCount,
							PRESPOOL->Segments.NodeCount);
			}

			SegResource prevres = NULL;
			DQUEUE_LOOP_BEGIN( &line, iter, BBSTNode, bbstnode )
				SegResource curres = (SegResource)(bbstnode->Data);
				if ( prevres != NULL ) {
					if ( IS_SEGRESOURCE_USABLE(prevres) &&
						 IS_SEGRESOURCE_USABLE(curres) )
					{
						if ( prevres->Available.MemoryMB < curres->Available.MemoryMB )
						{
							elog(ERROR, "HAWQ RM Validation. The available resource "
										"ordered index is not ordered well. "
										"Current host %s, %d MB, "
										"Previous host %s, %d MB.",
										GET_SEGRESOURCE_HOSTNAME(curres),
										curres->Available.MemoryMB,
										GET_SEGRESOURCE_HOSTNAME(prevres),
										prevres->Available.MemoryMB);
						}
					}
					else if (!IS_SEGRESOURCE_USABLE(prevres) &&
							  IS_SEGRESOURCE_USABLE(curres))
					{
						elog(ERROR, "HAWQ RM Validation. The available resource ordered "
									"index is not ordered well. "
									"Current host %s is available "
									"Previous host %s is not available.",
									GET_SEGRESOURCE_HOSTNAME(curres),
									GET_SEGRESOURCE_HOSTNAME(prevres));
					}
				}
				prevres = curres;
			DQUEUE_LOOP_END
			removeAllDQueueNodes(&line);
		}

		/* Validation 7. The allocated resource index should be well organized. */
		BBST alloctree = NULL;
		if ( getOrderedResourceAllocTreeIndexByRatio(PQUEMGR->RatioReverseIndex[0],
													 &alloctree) != FUNC_RETURN_OK )
		{
		}
		else
		{
			Assert( alloctree != NULL );
			traverseBBSTMidOrder(alloctree, &line);

			if ( line.NodeCount != PRESPOOL->Segments.NodeCount )
			{
				elog(ERROR, "HAWQ RM Validation. The allocated resource ordered index "
							"contains %d nodes, expect %d nodes.",
							line.NodeCount,
							PRESPOOL->Segments.NodeCount);
			}

			SegResource prevres = NULL;
			DQUEUE_LOOP_BEGIN( &line, iter, BBSTNode, bbstnode )
				SegResource curres = (SegResource)(bbstnode->Data);
				if ( prevres != NULL )
				{
					if ( IS_SEGRESOURCE_USABLE(prevres) &&
						 IS_SEGRESOURCE_USABLE(curres) )
					{
						if ( prevres->Allocated.MemoryMB < curres->Allocated.MemoryMB )
						{
							elog(ERROR, "HAWQ RM Validation. The allocated resource ordered "
										"index is not ordered well. "
										"Current host %s, %d MB, "
										"Previous host %s, %d MB.",
										GET_SEGRESOURCE_HOSTNAME(curres),
										curres->Allocated.MemoryMB,
										GET_SEGRESOURCE_HOSTNAME(prevres),
										prevres->Allocated.MemoryMB);
						}
					}
					else if (!IS_SEGRESOURCE_USABLE(prevres) &&
							  IS_SEGRESOURCE_USABLE(curres))
					{
						elog(ERROR, "HAWQ RM Validation. The allocated resource ordered "
									"index is not ordered well. "
									"Current host %s is available "
									"Previous host %s is not available.",
									GET_SEGRESOURCE_HOSTNAME(curres),
									GET_SEGRESOURCE_HOSTNAME(prevres));
					}
				}
				prevres = curres;
			DQUEUE_LOOP_END
			removeAllDQueueNodes(&line);
			cleanDQueue(&line);
		}
	}
}

int getClusterGRMContainerSize(void)
{
	int res = 0;
	for ( int32_t i =  0 ; i < PRESPOOL->SegmentIDCounter ; ++i )
	{
		/* Get segment. */
		SegResource segres = getSegResource(i);
		res += getSegmentGRMContainerSize(segres);
	}
	return res;
}

int getSegmentGRMContainerSize(SegResource segres)
{
	return segres->GRMContainerCount;
}


void getSegResResourceCountersByMemCoreCounters(SegResource  resinfo,
												int32_t		*allocmem,
												double		*alloccore,
												int32_t		*availmem,
												double		*availcore)
{
	*allocmem  = 0;
	*alloccore = 0.0;
	*availmem  = 0;
	*availcore = 0.0;

	for ( int i = 0 ; i < PQUEMGR->RatioCount ; ++i )
	{
		GRMContainerSet ctns = resinfo->ContainerSets[i];

		if ( resinfo->ContainerSets[i] == NULL )
		{
			continue;
		}

		*allocmem  += ctns->Allocated.MemoryMB;
		*alloccore += ctns->Allocated.Core;
		*availmem  += ctns->Available.MemoryMB;
		*availcore += ctns->Available.Core;

		/* Check if all allocated containers are counted. */
		int32_t   mem  = 0;
		double    core = 0;
		ListCell *cell = NULL;
		foreach(cell, ctns->Containers)
		{
			mem  += ((GRMContainer)lfirst(cell))->MemoryMB;
			core += ((GRMContainer)lfirst(cell))->Core;
		}

		if ( mem != ctns->Allocated.MemoryMB || core != ctns->Allocated.Core )
		{
			elog(ERROR, "HAWQ RM Validation. Wrong container set counter. "
						"Host %s.",
						GET_SEGRESOURCE_HOSTNAME(resinfo));
		}
	}
}

void dumpResourcePoolHosts(const char *filename)
{
    if ( filename == NULL )
    {
    	return;
    }

    FILE *fp = fopen(filename, "w");
    if ( fp == NULL )
    {
    	elog(WARNING, "Fail to open file %s to dump resource pool host status",
    				  filename);
    	return;
    }

    HASHTABLE hawq_nodes = &(PRESPOOL->Segments);
    for (int i=0; i<hawq_nodes->SlotVolume;i++) 
    {
    	List     *slot = hawq_nodes->Slots[i];
    	ListCell *cell = NULL;

    	foreach(cell, slot)
    	{
            SegResource segresource = (SegResource)(((PAIR)lfirst(cell))->Value);
            fprintf(fp, "HOST_ID(id=%u:hostname:%s)\n",
            		segresource->Stat->ID,
                    GET_SEGRESOURCE_HOSTNAME(segresource));
            fprintf(fp, "HOST_INFO(FTSTotalMemoryMB=%u:FTSTotalCore=%u:"
            					  "GRMTotalMemoryMB=%u:GRMTotalCore=%u)\n",
                    segresource->Stat->FTSTotalMemoryMB,
                    segresource->Stat->FTSTotalCore,
                    segresource->Stat->GRMTotalMemoryMB,
                    segresource->Stat->GRMTotalCore);
            fprintf(fp, "HOST_AVAILABLITY(HAWQAvailable=%s:GLOBAvailable=%s)\n",
                    segresource->Stat->FTSAvailable == 0 ? "false" : "true",
                    segresource->Stat->GRMAvailable == 0 ? "false" : "true");
            fprintf(fp, "HOST_RESOURCE(AllocatedMemory=%d:AllocatedCores=%f:"
            						  "AvailableMemory=%d:AvailableCores=%f:"
            						  "IOBytesWorkload="INT64_FORMAT":"
            						  "SliceWorkload=%d:"
            						  "LastUpdateTime="INT64_FORMAT":"
            						  "RUAlivePending=%s)\n",
                    segresource->Allocated.MemoryMB,
					segresource->Allocated.Core,
					segresource->Available.MemoryMB,
					segresource->Available.Core,
					segresource->IOBytesWorkload,
					segresource->SliceWorkload,
					segresource->LastUpdateTime,
					segresource->RUAlivePending ? "true" : "false");

            for (int j=0; j<PQUEMGR->RatioCount;j++)
            {
            	GRMContainerSet ctns = segresource->ContainerSets[j];

                if (ctns == NULL )
                { 
                    continue; 
                }
            
                fprintf(fp, "HOST_RESOURCE_CONTAINERSET("
                			"ratio=%u:"
                			"AllocatedMemory=%d:AvailableMemory=%d:"
                			"AllocatedCore=%f:AvailableCore:%f)\n",
                            PQUEMGR->RatioReverseIndex[j],
                            ctns->Allocated.MemoryMB,
                            ctns->Available.MemoryMB,
                            ctns->Allocated.Core,
                            ctns->Available.Core);

                ListCell *cell = NULL;
                foreach(cell, ctns->Containers)
                {
                	GRMContainer ctn = (GRMContainer)lfirst(cell);
					fprintf(fp, "\tRESOURCE_CONTAINER("
								"ID=%d:"
								"MemoryMB=%d:Core=%d:"
								"Life=%d:"
								"HostName=%s)\n",
								ctn->ID,
								ctn->MemoryMB,
								ctn->Core,
								ctn->Life,
								ctn->HostName);
                }
            }
    	}
    }

    fclose(fp);
}
