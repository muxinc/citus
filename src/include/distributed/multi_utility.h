/*-------------------------------------------------------------------------
 *
 * multi_utility.h
 *	  Citus utility hook and related functionality.
 *
 * Copyright (c) 2012-2016, Citus Data, Inc.
 *-------------------------------------------------------------------------
 */

#ifndef MULTI_UTILITY_H
#define MULTI_UTILITY_H

#include "tcop/utility.h"


typedef struct NodeAddress
{
	char *nodeName;
	int32 nodePort;
} NodeAddress;

extern void multi_ProcessUtility(Node *parsetree, const char *queryString,
								 ProcessUtilityContext context, ParamListInfo params,
								 DestReceiver *dest, char *completionTag);

extern bool WorkerCopy(CopyStmt *copyStatement);
extern NodeAddress * MasterNodeAddress(CopyStmt *copyStatement);

#endif /* MULTI_UTILITY_H */
