/*-------------------------------------------------------------------------
 * multi_utility.c
 *	  Citus utility hook and related functionality.
 *
 * Copyright (c) 2012-2016, Citus Data, Inc.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include "access/htup_details.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "distributed/master_protocol.h"
#include "distributed/metadata_cache.h"
#include "distributed/multi_copy.h"
#include "distributed/multi_utility.h"
#include "distributed/multi_join_order.h"
#include "distributed/transmit.h"
#include "distributed/worker_manager.h"
#include "distributed/worker_protocol.h"
#include "parser/parser.h"
#include "parser/parse_utilcmd.h"
#include "storage/lmgr.h"
#include "tcop/pquery.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * This struct defines the state for the callback for drop statements.
 * It is copied as it is from commands/tablecmds.c in Postgres source.
 */
struct DropRelationCallbackState
{
	char relkind;
	Oid heapOid;
	bool concurrent;
};


/* Local functions forward declarations for Transmit statement */
static bool IsTransmitStmt(Node *parsetree);
static void VerifyTransmitStmt(CopyStmt *copyStatement);

/* Local functions forward declarations for processing distributed table commands */
static Node * ProcessCopyStmt(CopyStmt *copyStatement, char *completionTag);
static Node * ProcessIndexStmt(IndexStmt *createIndexStatement,
							   const char *createIndexCommand);
static Node * ProcessDropIndexStmt(DropStmt *dropIndexStatement,
								   const char *dropIndexCommand);
static Node * ProcessAlterTableStmt(AlterTableStmt *alterTableStatement,
									const char *alterTableCommand);

/* Local functions forward declarations for unsupported command checks */
static void ErrorIfUnsupportedIndexStmt(IndexStmt *createIndexStatement);
static void ErrorIfUnsupportedDropIndexStmt(DropStmt *dropIndexStatement);
static void ErrorIfUnsupportedAlterTableStmt(AlterTableStmt *alterTableStatement);
static void ErrorIfDistributedRenameStmt(RenameStmt *renameStatement);

/* Local functions forward declarations for helper functions */
static bool IsAlterTableRenameStmt(RenameStmt *renameStatement);
static void ExecuteDistributedDDLCommand(Oid relationId, const char *ddlCommandString);
static bool ExecuteCommandOnWorkerShards(Oid relationId, const char *commandString,
										 List **failedPlacementList);
static bool AllFinalizedPlacementsAccessible(Oid relationId);
static void RangeVarCallbackForDropIndex(const RangeVar *rel, Oid relOid, Oid oldRelOid,
										 void *arg);


/*
 * Utility for handling citus specific concerns around utility statements.
 *
 * There's two basic types of concerns here:
 * 1) Intercept utility statements that run after distributed query
 *    execution. At this stage, the Create Table command for the master node's
 *    temporary table has been executed, and this table's relationId is
 *    visible to us. We can therefore update the relationId in master node's
 *    select query.
 * 2) Handle utility statements on distributed tables that the core code can't
 *    handle.
 */
void
multi_ProcessUtility(Node *parsetree,
					 const char *queryString,
					 ProcessUtilityContext context,
					 ParamListInfo params,
					 DestReceiver *dest,
					 char *completionTag)
{
	/*
	 * TRANSMIT used to be separate command, but to avoid patching the grammar
	 * it's no overlaid onto COPY, but with FORMAT = 'transmit' instead of the
	 * normal FORMAT options.
	 */
	if (IsTransmitStmt(parsetree))
	{
		CopyStmt *copyStatement = (CopyStmt *) parsetree;

		VerifyTransmitStmt(copyStatement);

		/* ->relation->relname is the target file in our overloaded COPY */
		if (copyStatement->is_from)
		{
			ReceiveRegularFile(copyStatement->relation->relname);
		}
		else
		{
			SendRegularFile(copyStatement->relation->relname);
		}

		/* Don't execute the faux copy statement */
		return;
	}

	if (IsA(parsetree, CopyStmt))
	{
		parsetree = ProcessCopyStmt((CopyStmt *) parsetree, completionTag);

		if (parsetree == NULL)
		{
			return;
		}
	}

	if (IsA(parsetree, IndexStmt))
	{
		parsetree = ProcessIndexStmt((IndexStmt *) parsetree, queryString);
	}

	if (IsA(parsetree, DropStmt))
	{
		DropStmt *dropStatement = (DropStmt *) parsetree;
		if (dropStatement->removeType == OBJECT_INDEX)
		{
			parsetree = ProcessDropIndexStmt(dropStatement, queryString);
		}
	}

	if (IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *alterTableStmt = (AlterTableStmt *) parsetree;
		if (alterTableStmt->relkind == OBJECT_TABLE)
		{
			parsetree = ProcessAlterTableStmt(alterTableStmt, queryString);
		}
	}

	/*
	 * ALTER TABLE ... RENAME statements have their node type as RenameStmt and
	 * not AlterTableStmt. So, we intercept RenameStmt to tackle these commands.
	 */
	if (IsA(parsetree, RenameStmt))
	{
		RenameStmt *renameStmt = (RenameStmt *) parsetree;
		if (IsAlterTableRenameStmt(renameStmt))
		{
			ErrorIfDistributedRenameStmt(renameStmt);
		}
	}

	/*
	 * Inform the user about potential caveats.
	 *
	 * To prevent failures in aborted transactions, CitusHasBeenLoaded() needs
	 * to be the second condition. See RelationIdGetRelation() which is called
	 * by CitusHasBeenLoaded().
	 */
	if (IsA(parsetree, CreatedbStmt) && CitusHasBeenLoaded())
	{
		ereport(NOTICE, (errmsg("Citus partially supports CREATE DATABASE for "
								"distributed databases"),
						 errdetail("Citus does not propagate CREATE DATABASE "
								   "command to workers"),
						 errhint("You can manually create a database and its "
								 "extensions on workers.")));
	}
	else if (IsA(parsetree, CreateSchemaStmt) && CitusHasBeenLoaded())
	{
		ereport(NOTICE, (errmsg("Citus partially supports CREATE SCHEMA "
								"for distributed databases"),
						 errdetail("schema usage in joins and in some UDFs "
								   "provided by Citus are not supported yet")));
	}
	else if (IsA(parsetree, CreateRoleStmt) && CitusHasBeenLoaded())
	{
		ereport(NOTICE, (errmsg("Citus does not support CREATE ROLE/USER "
								"for distributed databases"),
						 errdetail("Multiple roles are currently supported "
								   "only for local tables")));
	}

	/* now drop into standard process utility */
	standard_ProcessUtility(parsetree, queryString, context,
							params, dest, completionTag);
}


/* Is the passed in statement a transmit statement? */
static bool
IsTransmitStmt(Node *parsetree)
{
	if (IsA(parsetree, CopyStmt))
	{
		CopyStmt *copyStatement = (CopyStmt *) parsetree;
		ListCell *optionCell = NULL;

		/* Extract options from the statement node tree */
		foreach(optionCell, copyStatement->options)
		{
			DefElem *defel = (DefElem *) lfirst(optionCell);

			if (strncmp(defel->defname, "format", NAMEDATALEN) == 0 &&
				strncmp(defGetString(defel), "transmit", NAMEDATALEN) == 0)
			{
				return true;
			}
		}
	}

	return false;
}


/*
 * VerifyTransmitStmt checks that the passed in command is a valid transmit
 * statement. Raise ERROR if not.
 *
 * Note that only 'toplevel' options in the CopyStmt struct are checked, and
 * that verification of the target files existance is not done here.
 */
static void
VerifyTransmitStmt(CopyStmt *copyStatement)
{
	/* do some minimal option verification */
	if (copyStatement->relation == NULL ||
		copyStatement->relation->relname == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("FORMAT 'transmit' requires a target file")));
	}

	if (copyStatement->filename != NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("FORMAT 'transmit' only accepts STDIN/STDOUT"
							   " as input/output")));
	}

	if (copyStatement->query != NULL ||
		copyStatement->attlist != NULL ||
		copyStatement->is_program)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("FORMAT 'transmit' does not accept query, attribute list"
							   " or PROGRAM parameters ")));
	}
}


/*
 * ProcessCopyStmt handles Citus specific concerns for COPY like supporting
 * COPYing from distributed tables and preventing unsupported actions. The
 * function returns a modified COPY statement to be executed, or NULL if no
 * further processing is needed.
 */
static Node *
ProcessCopyStmt(CopyStmt *copyStatement, char *completionTag)
{
	/*
	 * We first check if we have a "COPY (query) TO filename". If we do, copy doesn't
	 * accept relative file paths. However, SQL tasks that get assigned to worker nodes
	 * have relative paths. We therefore convert relative paths to absolute ones here.
	 */
	if (copyStatement->relation == NULL &&
		!copyStatement->is_from &&
		!copyStatement->is_program &&
		copyStatement->filename != NULL)
	{
		const char *filename = copyStatement->filename;

		if (!is_absolute_path(filename) && JobDirectoryElement(filename))
		{
			copyStatement->filename = make_absolute_path(filename);
		}
	}

	/*
	 * We check whether a distributed relation is affected. For that, we need to open the
	 * relation. To prevent race conditions with later lookups, lock the table, and modify
	 * the rangevar to include the schema.
	 */
	if (copyStatement->relation != NULL)
	{
		Relation copiedRelation = NULL;
		bool isDistributedRelation = false;

		copiedRelation = heap_openrv(copyStatement->relation, AccessShareLock);

		isDistributedRelation = IsDistributedTable(RelationGetRelid(copiedRelation));

		/* ensure future lookups hit the same relation */
		copyStatement->relation->schemaname = get_namespace_name(
			RelationGetNamespace(copiedRelation));

		heap_close(copiedRelation, NoLock);

		if (isDistributedRelation)
		{
			if (copyStatement->is_from)
			{
				CitusCopyFrom(copyStatement, completionTag);
				return NULL;
			}
			else if (!copyStatement->is_from)
			{
				/*
				 * The copy code only handles SELECTs in COPY ... TO on master tables,
				 * as that can be done non-invasively. To handle COPY master_rel TO
				 * the copy statement is replaced by a generated select statement.
				 */
				ColumnRef *allColumns = makeNode(ColumnRef);
				SelectStmt *selectStmt = makeNode(SelectStmt);
				ResTarget *selectTarget = makeNode(ResTarget);

				allColumns->fields = list_make1(makeNode(A_Star));
				allColumns->location = -1;

				selectTarget->name = NULL;
				selectTarget->indirection = NIL;
				selectTarget->val = (Node *) allColumns;
				selectTarget->location = -1;

				selectStmt->targetList = list_make1(selectTarget);
				selectStmt->fromClause = list_make1(copyObject(copyStatement->relation));

				/* replace original statement */
				copyStatement = copyObject(copyStatement);
				copyStatement->relation = NULL;
				copyStatement->query = (Node *) selectStmt;
			}
		}
	}

	return (Node *) copyStatement;
}


/*
 * ProcessIndexStmt processes create index statements for distributed tables.
 * The function first checks if the statement belongs to a distributed table
 * or not. If it does, then it executes distributed logic for the command.
 *
 * The function returns the IndexStmt node for the command to be executed on the
 * master node table.
 */
static Node *
ProcessIndexStmt(IndexStmt *createIndexStatement, const char *createIndexCommand)
{
	/*
	 * We first check whether a distributed relation is affected. For that, we need to
	 * open the relation. To prevent race conditions with later lookups, lock the table,
	 * and modify the rangevar to include the schema.
	 */
	if (createIndexStatement->relation != NULL)
	{
		Relation relation = NULL;
		Oid relationId = InvalidOid;
		bool isDistributedRelation = false;
		char *namespaceName = NULL;
		LOCKMODE lockmode = ShareLock;

		/*
		 * We don't support concurrently creating indexes for distributed
		 * tables, but till this point, we don't know if it is a regular or a
		 * distributed table.
		 */
		if (createIndexStatement->concurrent)
		{
			lockmode = ShareUpdateExclusiveLock;
		}

		relation = heap_openrv(createIndexStatement->relation, lockmode);
		relationId = RelationGetRelid(relation);

		isDistributedRelation = IsDistributedTable(relationId);

		/* ensure future lookups hit the same relation */
		namespaceName = get_namespace_name(RelationGetNamespace(relation));
		createIndexStatement->relation->schemaname = namespaceName;

		heap_close(relation, NoLock);

		if (isDistributedRelation)
		{
			ErrorIfUnsupportedIndexStmt(createIndexStatement);

			/* if it is supported, go ahead and execute the command */
			ExecuteDistributedDDLCommand(relationId, createIndexCommand);
		}
	}

	return (Node *) createIndexStatement;
}


/*
 * ProcessDropIndexStmt processes drop index statements for distributed tables.
 * The function first checks if the statement belongs to a distributed table
 * or not. If it does, then it executes distributed logic for the command.
 *
 * The function returns the DropStmt node for the command to be executed on the
 * master node table.
 */
static Node *
ProcessDropIndexStmt(DropStmt *dropIndexStatement, const char *dropIndexCommand)
{
	ListCell *dropObjectCell = NULL;
	Oid distributedIndexId = InvalidOid;
	Oid distributedRelationId = InvalidOid;

	Assert(dropIndexStatement->removeType == OBJECT_INDEX);

	/* check if any of the indexes being dropped belong to a distributed table */
	foreach(dropObjectCell, dropIndexStatement->objects)
	{
		Oid indexId = InvalidOid;
		Oid relationId = InvalidOid;
		bool isDistributedRelation = false;
		struct DropRelationCallbackState state;
		bool missingOK = true;
		bool noWait = false;
		LOCKMODE lockmode = AccessExclusiveLock;

		List *objectNameList = (List *) lfirst(dropObjectCell);
		RangeVar *rangeVar = makeRangeVarFromNameList(objectNameList);

		/*
		 * We don't support concurrently dropping indexes for distributed
		 * tables, but till this point, we don't know if it is a regular or a
		 * distributed table.
		 */
		if (dropIndexStatement->concurrent)
		{
			lockmode = ShareUpdateExclusiveLock;
		}

		/*
		 * The next few statements are based on RemoveRelations() in
		 * commands/tablecmds.c in Postgres source.
		 */
		AcceptInvalidationMessages();

		state.relkind = RELKIND_INDEX;
		state.heapOid = InvalidOid;
		state.concurrent = dropIndexStatement->concurrent;
		indexId = RangeVarGetRelidExtended(rangeVar, lockmode, missingOK,
										   noWait, RangeVarCallbackForDropIndex,
										   (void *) &state);

		/*
		 * If the index does not exist, we don't do anything here, and allow
		 * postgres to throw appropriate error or notice message later.
		 */
		if (!OidIsValid(indexId))
		{
			continue;
		}

		relationId = IndexGetRelation(indexId, false);
		isDistributedRelation = IsDistributedTable(relationId);
		if (isDistributedRelation)
		{
			distributedIndexId = indexId;
			distributedRelationId = relationId;
			break;
		}
	}

	if (OidIsValid(distributedIndexId))
	{
		ErrorIfUnsupportedDropIndexStmt(dropIndexStatement);

		/* if it is supported, go ahead and execute the command */
		ExecuteDistributedDDLCommand(distributedRelationId, dropIndexCommand);
	}

	return (Node *) dropIndexStatement;
}


/*
 * ProcessAlterTableStmt processes alter table statements for distributed tables.
 * The function first checks if the statement belongs to a distributed table
 * or not. If it does, then it executes distributed logic for the command.
 *
 * The function returns the AlterTableStmt node for the command to be executed on the
 * master node table.
 */
static Node *
ProcessAlterTableStmt(AlterTableStmt *alterTableStatement, const char *alterTableCommand)
{
	/* first check whether a distributed relation is affected */
	if (alterTableStatement->relation != NULL)
	{
		LOCKMODE lockmode = AlterTableGetLockLevel(alterTableStatement->cmds);
		Oid relationId = AlterTableLookupRelation(alterTableStatement, lockmode);
		if (OidIsValid(relationId))
		{
			bool isDistributedRelation = IsDistributedTable(relationId);
			if (isDistributedRelation)
			{
				ErrorIfUnsupportedAlterTableStmt(alterTableStatement);

				/* if it is supported, go ahead and execute the command */
				ExecuteDistributedDDLCommand(relationId, alterTableCommand);
			}
		}
	}

	return (Node *) alterTableStatement;
}


/*
 * ErrorIfUnsupportedIndexStmt checks if the corresponding index statement is
 * supported for distributed tables and errors out if it is not.
 */
static void
ErrorIfUnsupportedIndexStmt(IndexStmt *createIndexStatement)
{
	Oid namespaceId;
	Oid indexRelationId;
	char *indexRelationName = createIndexStatement->idxname;

	if (indexRelationName == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("creating index without a name on a distributed table is "
							   "currently unsupported")));
	}

	namespaceId = get_namespace_oid(createIndexStatement->relation->schemaname, false);
	indexRelationId = get_relname_relid(indexRelationName, namespaceId);
	if (indexRelationId != InvalidOid)
	{
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE),
						errmsg("relation \"%s\" already exists", indexRelationName)));
	}

	if (createIndexStatement->tableSpace != NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("specifying tablespaces with CREATE INDEX statements is "
							   "currently unsupported")));
	}

	if (createIndexStatement->concurrent)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("creating indexes concurrently on distributed tables is "
							   "currently unsupported")));
	}

	if (createIndexStatement->unique)
	{
		RangeVar *relation = createIndexStatement->relation;
		bool missingOk = false;

		/* caller uses ShareLock for non-concurrent indexes, use the same lock here */
		LOCKMODE lockMode = ShareLock;
		Oid relationId = RangeVarGetRelid(relation, lockMode, missingOk);
		Var *partitionKey = PartitionKey(relationId);
		char partitionMethod = PartitionMethod(relationId);
		List *indexParameterList = NIL;
		ListCell *indexParameterCell = NULL;
		bool indexContainsPartitionColumn = false;

		if (partitionMethod == DISTRIBUTE_BY_APPEND)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("creating unique indexes on append-partitioned tables "
								   "is currently unsupported")));
		}

		indexParameterList = createIndexStatement->indexParams;
		foreach(indexParameterCell, indexParameterList)
		{
			IndexElem *indexElement = (IndexElem *) lfirst(indexParameterCell);
			char *columnName = indexElement->name;
			AttrNumber attributeNumber = InvalidAttrNumber;

			/* column name is null for index expressions, skip it */
			if (columnName == NULL)
			{
				continue;
			}

			attributeNumber = get_attnum(relationId, columnName);
			if (attributeNumber == partitionKey->varattno)
			{
				indexContainsPartitionColumn = true;
			}
		}

		if (!indexContainsPartitionColumn)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("creating unique indexes on non-partition "
								   "columns is currently unsupported")));
		}
	}
}


/*
 * ErrorIfUnsupportedDropIndexStmt checks if the corresponding drop index statement is
 * supported for distributed tables and errors out if it is not.
 */
static void
ErrorIfUnsupportedDropIndexStmt(DropStmt *dropIndexStatement)
{
	Assert(dropIndexStatement->removeType == OBJECT_INDEX);

	if (list_length(dropIndexStatement->objects) > 1)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot drop multiple distributed objects in a "
							   "single command"),
						errhint("Try dropping each object in a separate DROP "
								"command.")));
	}

	if (dropIndexStatement->concurrent)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("dropping indexes concurrently on distributed tables is "
							   "currently unsupported")));
	}
}


/*
 * ErrorIfUnsupportedAlterTableStmt checks if the corresponding alter table statement
 * is supported for distributed tables and errors out if it is not. Currently,
 * only the following commands are supported.
 *
 * ALTER TABLE ADD|DROP COLUMN
 * ALTER TABLE ALTER COLUMN SET DATA TYPE
 * ALTER TABLE SET|DROP NOT NULL
 * ALTER TABLE SET|DROP DEFAULT
 */
static void
ErrorIfUnsupportedAlterTableStmt(AlterTableStmt *alterTableStatement)
{
	List *commandList = alterTableStatement->cmds;
	ListCell *commandCell = NULL;

	/* error out if any of the subcommands are unsupported */
	foreach(commandCell, commandList)
	{
		AlterTableCmd *command = (AlterTableCmd *) lfirst(commandCell);
		AlterTableType alterTableType = command->subtype;

		switch (alterTableType)
		{
			case AT_AddColumn:
			{
				break;
			}

			case AT_DropColumn:
			case AT_ColumnDefault:
			case AT_AlterColumnType:
			case AT_SetNotNull:
			case AT_DropNotNull:
			{
				/* error out if the alter table command is on the partition column */

				Var *partitionColumn = NULL;
				HeapTuple tuple = NULL;
				char *alterColumnName = command->name;

				LOCKMODE lockmode = AlterTableGetLockLevel(alterTableStatement->cmds);
				Oid relationId = AlterTableLookupRelation(alterTableStatement, lockmode);
				if (!OidIsValid(relationId))
				{
					continue;
				}

				partitionColumn = PartitionKey(relationId);

				tuple = SearchSysCacheAttName(relationId, alterColumnName);
				if (HeapTupleIsValid(tuple))
				{
					Form_pg_attribute targetAttr = (Form_pg_attribute) GETSTRUCT(tuple);
					if (targetAttr->attnum == partitionColumn->varattno)
					{
						ereport(ERROR, (errmsg("cannot execute ALTER TABLE command "
											   "involving partition column")));
					}

					ReleaseSysCache(tuple);
				}

				break;
			}

			default:
			{
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("alter table command is currently supported"),
								errdetail("Only ADD|DROP COLUMN, SET|DROP NOT NULL,"
										  " SET|DROP DEFAULT and TYPE subcommands are"
										  " supported.")));
			}
		}
	}
}


/*
 * ErrorIfDistributedRenameStmt errors out if the corresponding rename statement
 * operates on a distributed table or its objects.
 *
 * Note: This function handles only those rename statements which operate on tables.
 */
static void
ErrorIfDistributedRenameStmt(RenameStmt *renameStatement)
{
	Oid relationId = InvalidOid;
	bool isDistributedRelation = false;

	Assert(IsAlterTableRenameStmt(renameStatement));

	/*
	 * The lock levels here should be same as the ones taken in
	 * RenameRelation(), renameatt() and RenameConstraint(). However, since all
	 * three statements have identical lock levels, we just use a single statement.
	 */
	relationId = RangeVarGetRelid(renameStatement->relation, AccessExclusiveLock,
								  renameStatement->missing_ok);

	/*
	 * If the table does not exist, we don't do anything here, and allow postgres to
	 * throw the appropriate error or notice message later.
	 */
	if (!OidIsValid(relationId))
	{
		return;
	}

	isDistributedRelation = IsDistributedTable(relationId);
	if (isDistributedRelation)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("renaming distributed tables or their objects is "
							   "currently unsupported")));
	}
}


/*
 * IsAlterTableRenameStmt returns true if the passed in RenameStmt operates on a
 * distributed table or its objects. This includes:
 * ALTER TABLE RENAME
 * ALTER TABLE RENAME COLUMN
 * ALTER TABLE RENAME CONSTRAINT
 */
static bool
IsAlterTableRenameStmt(RenameStmt *renameStmt)
{
	bool isAlterTableRenameStmt = false;

	if (renameStmt->renameType == OBJECT_TABLE)
	{
		isAlterTableRenameStmt = true;
	}
	else if (renameStmt->renameType == OBJECT_COLUMN &&
			 renameStmt->relationType == OBJECT_TABLE)
	{
		isAlterTableRenameStmt = true;
	}

#if (PG_VERSION_NUM >= 90500)
	else if (renameStmt->renameType == OBJECT_TABCONSTRAINT)
	{
		isAlterTableRenameStmt = true;
	}
#else
	else if (renameStmt->renameType == OBJECT_CONSTRAINT &&
			 renameStmt->relationType == OBJECT_TABLE)
	{
		isAlterTableRenameStmt = true;
	}
#endif

	return isAlterTableRenameStmt;
}


/*
 * ExecuteDistributedDDLCommand applies a given DDL command to the given
 * distributed table. If the function is unable to access all the finalized
 * shard placements, then it fails early and errors out. If the command
 * successfully executed on any finalized shard placement, and failed on
 * others, then it marks the placements on which execution failed as invalid.
 */
static void
ExecuteDistributedDDLCommand(Oid relationId, const char *ddlCommandString)
{
	List *failedPlacementList = NIL;
	bool executionOK = false;

	bool allPlacementsAccessible = AllFinalizedPlacementsAccessible(relationId);
	if (!allPlacementsAccessible)
	{
		ereport(ERROR, (errmsg("cannot execute command: %s", ddlCommandString),
						errdetail("All finalized shard placements need to be accessible "
								  "to execute DDL commands on distributed tables.")));
	}

	/* make sure we don't process cancel signals */
	HOLD_INTERRUPTS();

	executionOK = ExecuteCommandOnWorkerShards(relationId, ddlCommandString,
											   &failedPlacementList);

	/* if command could not be executed on any finalized shard placement, error out */
	if (!executionOK)
	{
		ereport(ERROR, (errmsg("could not execute DDL command on worker node shards")));
	}
	else
	{
		/* else, mark failed placements as inactive */
		ListCell *failedPlacementCell = NULL;
		foreach(failedPlacementCell, failedPlacementList)
		{
			ShardPlacement *placement = (ShardPlacement *) lfirst(failedPlacementCell);
			uint64 shardId = placement->shardId;
			char *workerName = placement->nodeName;
			uint32 workerPort = placement->nodePort;
			uint64 oldShardLength = placement->shardLength;

			DeleteShardPlacementRow(shardId, workerName, workerPort);
			InsertShardPlacementRow(shardId, FILE_INACTIVE, oldShardLength,
									workerName, workerPort);
		}
	}

	if (QueryCancelPending)
	{
		ereport(WARNING, (errmsg("cancel requests are ignored during DDL commands")));
		QueryCancelPending = false;
	}

	RESUME_INTERRUPTS();
}


/*
 * ExecuteCommandOnWorkerShards executes a given command on all the finalized
 * shard placements of the given table. If the remote command errors out on the
 * first attempted placement, the function returns false. Otherwise, it returns
 * true.
 *
 * If the remote query errors out on the first attempted placement, it is very
 * likely that the command is going to fail on other placements too. This is
 * because most errors here will be PostgreSQL errors. Hence, the function fails
 * fast to avoid marking a high number of placements as failed. If the command
 * succeeds on at least one placement before failing on others, then the list of
 * failed placements is returned in failedPlacementList.
 *
 * Note: There are certain errors which would occur on few nodes and not on the
 * others. For example, adding a column with a type which exists on some nodes
 * and not on the others. In that case, this function might still end up returning
 * a large number of placements as failed.
 */
static bool
ExecuteCommandOnWorkerShards(Oid relationId, const char *commandString,
							 List **failedPlacementList)
{
	bool isFirstPlacement = true;
	ListCell *shardCell = NULL;
	List *shardList = NIL;

	shardList = LoadShardList(relationId);
	foreach(shardCell, shardList)
	{
		List *shardPlacementList = NIL;
		ListCell *shardPlacementCell = NULL;
		uint64 *shardIdPointer = (uint64 *) lfirst(shardCell);
		uint64 shardId = (*shardIdPointer);

		/* build the shard ddl command */
		char *escapedCommandString = quote_literal_cstr(commandString);
		StringInfo applyCommand = makeStringInfo();
		appendStringInfo(applyCommand, WORKER_APPLY_SHARD_DDL_COMMAND, shardId,
						 escapedCommandString);

		shardPlacementList = FinalizedShardPlacementList(shardId);
		foreach(shardPlacementCell, shardPlacementList)
		{
			ShardPlacement *placement = (ShardPlacement *) lfirst(shardPlacementCell);
			char *workerName = placement->nodeName;
			uint32 workerPort = placement->nodePort;

			List *queryResultList = ExecuteRemoteQuery(workerName, workerPort,
													   applyCommand);
			if (queryResultList == NIL)
			{
				/*
				 * If we failed on the first placement, return false. We return
				 * here instead of exiting at the end to avoid breaking through
				 * multiple loops.
				 */
				if (isFirstPlacement)
				{
					return false;
				}

				ereport(WARNING, (errmsg("could not apply command on shard "
										 UINT64_FORMAT " on node %s:%d", shardId,
										 workerName, workerPort),
								  errdetail("Shard placement will be marked as "
											"inactive.")));

				*failedPlacementList = lappend(*failedPlacementList, placement);
			}
			else
			{
				ereport(DEBUG2, (errmsg("applied command on shard " UINT64_FORMAT
										" on node %s:%d", shardId, workerName,
										workerPort)));
			}

			isFirstPlacement = false;
		}

		FreeStringInfo(applyCommand);
	}

	return true;
}


/*
 * AllFinalizedPlacementsAccessible returns true if all the finalized shard
 * placements for a given relation are accessible. Otherwise, the function
 * returns false. To do so, the function first gets a list of responsive
 * worker nodes and then checks if all the finalized shard placements lie
 * on those worker nodes.
 */
static bool
AllFinalizedPlacementsAccessible(Oid relationId)
{
	bool allPlacementsAccessible = true;
	ListCell *shardCell = NULL;
	List *responsiveNodeList = ResponsiveWorkerNodeList();

	List *shardList = LoadShardList(relationId);
	foreach(shardCell, shardList)
	{
		List *shardPlacementList = NIL;
		ListCell *shardPlacementCell = NULL;
		uint64 *shardIdPointer = (uint64 *) lfirst(shardCell);
		uint64 shardId = (*shardIdPointer);

		shardPlacementList = FinalizedShardPlacementList(shardId);
		foreach(shardPlacementCell, shardPlacementList)
		{
			ListCell *responsiveNodeCell = NULL;
			bool placementAccessible = false;
			ShardPlacement *placement = (ShardPlacement *) lfirst(shardPlacementCell);

			/* verify that the placement lies on one of the responsive worker nodes */
			foreach(responsiveNodeCell, responsiveNodeList)
			{
				WorkerNode *node = (WorkerNode *) lfirst(responsiveNodeCell);
				if (strncmp(node->workerName, placement->nodeName, WORKER_LENGTH) == 0 &&
					node->workerPort == placement->nodePort)
				{
					placementAccessible = true;
					break;
				}
			}

			if (!placementAccessible)
			{
				allPlacementsAccessible = false;
				break;
			}
		}

		if (!allPlacementsAccessible)
		{
			break;
		}
	}

	return allPlacementsAccessible;
}


/*
 * Before acquiring a table lock, check whether we have sufficient rights.
 * In the case of DROP INDEX, also try to lock the table before the index.
 *
 * This code is heavily borrowed from RangeVarCallbackForDropRelation() in
 * commands/tablecmds.c in Postgres source. We need this to ensure the right
 * order of locking while dealing with DROP INDEX statments.
 */
static void
RangeVarCallbackForDropIndex(const RangeVar *rel, Oid relOid, Oid oldRelOid, void *arg)
{
	/* *INDENT-OFF* */
	HeapTuple	tuple;
	struct DropRelationCallbackState *state;
	char		relkind;
	Form_pg_class classform;
	LOCKMODE	heap_lockmode;

	state = (struct DropRelationCallbackState *) arg;
	relkind = state->relkind;
	heap_lockmode = state->concurrent ?
		ShareUpdateExclusiveLock : AccessExclusiveLock;

	Assert(relkind == RELKIND_INDEX);

	/*
	 * If we previously locked some other index's heap, and the name we're
	 * looking up no longer refers to that relation, release the now-useless
	 * lock.
	 */
	if (relOid != oldRelOid && OidIsValid(state->heapOid))
	{
		UnlockRelationOid(state->heapOid, heap_lockmode);
		state->heapOid = InvalidOid;
	}

	/* Didn't find a relation, so no need for locking or permission checks. */
	if (!OidIsValid(relOid))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped, so nothing to do */
	classform = (Form_pg_class) GETSTRUCT(tuple);

	if (classform->relkind != relkind)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
						errmsg("\"%s\" is not an index", rel->relname)));

	/* Allow DROP to either table owner or schema owner */
	if (!pg_class_ownercheck(relOid, GetUserId()) &&
		!pg_namespace_ownercheck(classform->relnamespace, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   rel->relname);

	if (!allowSystemTableMods && IsSystemClass(relOid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rel->relname)));

	ReleaseSysCache(tuple);

	/*
	 * In DROP INDEX, attempt to acquire lock on the parent table before
	 * locking the index.  index_drop() will need this anyway, and since
	 * regular queries lock tables before their indexes, we risk deadlock if
	 * we do it the other way around.  No error if we don't find a pg_index
	 * entry, though --- the relation may have been dropped.
	 */
	if (relkind == RELKIND_INDEX && relOid != oldRelOid)
	{
		state->heapOid = IndexGetRelation(relOid, true);
		if (OidIsValid(state->heapOid))
			LockRelationOid(state->heapOid, heap_lockmode);
	}
	/* *INDENT-ON* */
}
