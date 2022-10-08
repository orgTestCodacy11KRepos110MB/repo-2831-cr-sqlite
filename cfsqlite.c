#include "cfsqlite.h"
#include "cfsqlite-util.h"
#include "cfsqlite-tableinfo.h"
#include "cfsqlite-consts.h"
#include "cfsqlite-triggers.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

/**
 * Global variables to hold the site id and db version.
 * This prevents us from having to query the tables that store
 * these values every time we do a read or write.
 *
 * The db version must be correctly updated on every write transaction.
 * All writes within the same transaction must use the same db version.
 * The reason for this is so we can replicate all rows changed by
 * a given transaction together and commit them together on the
 * other end.
 *
 * DB version is incremented on trnsaction commit via a
 * commit hook.
 */
static char siteIdBlob[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
// so we don't re-initialize site id on new connections
// TODO: mutex to guard initialization of cfsqlite.
// cfsqlite initialize should not need to be re-entrant
// as now calls within initialization should depend on cfsqlite itself.
/**
 * TODO: Add a mutex to guard initialization of cfsqlite.
 * This will prevent connections from initializing
 * the shared memory concurrently.
 *
 * The initialization mutex does not need to be re-entrant
 * as cfsqlite initialization makes no calls to itself.
 */
static int siteIdSet = 0;
static const size_t siteIdBlobSize = sizeof(siteIdBlob);

/**
 * Cached representation of the version of the database.
 *
 * This is not an unsigned int since sqlite does not support unsigned ints
 * as a data type and we do eventually write db version(s) to the db.
 *
 * TODO: Changing the db version needs to be done in a thread safe manner.
 */
static int64_t dbVersion = -9223372036854775807L;
static int dbVersionSet = 0;

/**
 * The site id table is used to persist the site id and
 * populate `siteIdBlob` on initialization of a connection.
 */
static int createSiteIdTable(sqlite3 *db)
{
  int rc = SQLITE_OK;
  char *zSql = 0;

  zSql = sqlite3_mprintf(
      "CREATE TABLE \"%s\" (site_id)",
      TBL_SITE_ID);
  rc = sqlite3_exec(db, zSql, 0, 0, 0);
  sqlite3_free(zSql);

  if (rc != SQLITE_OK)
  {
    return rc;
  }

  zSql = sqlite3_mprintf("INSERT INTO \"%s\" VALUES(uuid_blob(uuid()))", TBL_SITE_ID);
  rc = sqlite3_exec(db, zSql, 0, 0, 0);
  sqlite3_free(zSql);

  return rc;
}

/**
 * Loads the siteId into memory. If a site id
 * cannot be found for the given database one is created
 * and saved to the site id table.
 */
static int initSiteId(sqlite3 *db)
{
  char *zSql = 0;
  sqlite3_stmt *pStmt = 0;
  int rc = SQLITE_OK;
  int tableExists = 0;
  const void *siteIdFromTable = 0;

  // We were already initialized by another connection
  if (siteIdSet != 0)
  {
    return rc;
  }

  // look for site id tablesql
  tableExists = cfsql_doesTableExist(db, TBL_SITE_ID);

  if (tableExists == 0)
  {
    // create the table
    // generate the site id
    // insert it
    rc = createSiteIdTable(db);
    if (rc != SQLITE_OK)
    {
      return rc;
    }
  }

  // read site id from the table and save to global
  zSql = sqlite3_mprintf("SELECT site_id FROM %Q", TBL_SITE_ID);
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);

  if (rc != SQLITE_OK)
  {
    return rc;
  }

  rc = sqlite3_step(pStmt);
  if (rc != SQLITE_ROW)
  {
    sqlite3_finalize(pStmt);
    return rc;
  }

  siteIdFromTable = sqlite3_column_blob(pStmt, 0);
  // the blob mem returned to us will be freed so copy it.
  // https://www.sqlite.org/c3ref/column_blob.html
  memcpy(siteIdBlob, siteIdFromTable, siteIdBlobSize);

  sqlite3_finalize(pStmt);
  return SQLITE_OK;
}

/**
 * Computes the current version of the database
 * and saves it in the global variable.
 * The version is incremented on every transaction commit.
 * The version is used on every write to update clock values for the
 * rows written.
 *
 * INIT DB VERSION MUST BE CALLED AFTER SITE ID INITIALIZATION
 */
static int initDbVersion(sqlite3 *db)
{
  char *zSql;
  sqlite3_stmt *pStmt = 0;
  int rc = SQLITE_OK;
  char **rClockTableNames = 0;
  int rNumRows = 0;
  int rNumCols = 0;
  int i = 0;

  // already initialized?
  if (dbVersionSet != 0)
  {
    return rc;
  }

  // find all `clock` tables
  rc = sqlite3_get_table(
      db,
      "SELECT tbl_name FROM sqlite_master WHERE type='table' AND tbl_name LIKE '%__cfsql_clock'",
      &rClockTableNames,
      &rNumRows,
      &rNumCols,
      0);

  if (rc != SQLITE_OK)
  {
    sqlite3_free_table(rClockTableNames);
    return rc;
  }

  if (rNumRows == 0)
  {
    sqlite3_free_table(rClockTableNames);
    return rc;
  }

  // builds the query string
  zSql = cfsql_getDbVersionUnionQuery(rNumRows, rClockTableNames);
  sqlite3_free_table(rClockTableNames);
  // now prepare the statement
  // and bind site id param
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);

  if (rc != SQLITE_OK)
  {
    return rc;
  }

  for (i = 0; i < rNumRows; ++i)
  {
    // SQLITE_STATIC since the site id never changes.
    sqlite3_bind_blob(pStmt, i, siteIdBlob, siteIdBlobSize, SQLITE_STATIC);
  }

  rc = sqlite3_step(pStmt);
  // No rows? Then we're a fresh DB with the min starting version
  if (rc != SQLITE_ROW)
  {
    dbVersionSet = 1;
    sqlite3_finalize(pStmt);
    return rc;
  }

  // had a row? grab the version returned to us
  dbVersion = sqlite3_column_int64(pStmt, 0);
  dbVersionSet = 1;
  sqlite3_finalize(pStmt);

  return SQLITE_OK;
}

/**
 * return the uuid which uniquely identifies this database.
 *
 * `select cfsql_siteid()`
 *
 * @param context
 * @param argc
 * @param argv
 */
static void siteIdFunc(sqlite3_context *context, int argc, sqlite3_value **argv)
{
  sqlite3_result_blob(context, &siteIdBlob, siteIdBlobSize, SQLITE_STATIC);
}

/**
 * Return the current version of the database.
 *
 * `select cfsql_dbversion()`
 */
static void dbVersionFunc(sqlite3_context *context, int argc, sqlite3_value **argv)
{
  sqlite3_result_int64(context, dbVersion);
}

/**
 * Given a query passed to cfsqlite, determine what kind of schema modification
 * query it is.
 *
 * We need to know given each schema modification type
 * requires unique handling in the crr layer.
 * 
 * The provided query must be a normalized query.
 */
static int determineQueryType(sqlite3 *db, sqlite3_context *context, const char *query)
{
  int rc = SQLITE_OK;
  char *formattedError = 0;

  if (strncmp("CREATE TABLE", query, CREATE_TABLE_LEN) == 0)
  {;
    return CREATE_TABLE;
  }
  if (strncmp("ALTER TABLE", query, ALTER_TABLE_LEN) == 0)
  {
    return ALTER_TABLE;
  }
  if (strncmp("CREATE INDEX", query, CREATE_INDEX_LEN) == 0)
  {
    return CREATE_INDEX;
  }
  if (strncmp("CREATE UNIQUE INDEX", query, CREATE_UNIQUE_INDEX_LEN) == 0)
  {
    return CREATE_INDEX;
  }
  if (strncmp("DROP INDEX", query, DROP_INDEX_LEN) == 0)
  {
    return DROP_INDEX;
  }
  if (strncmp("DROP TABLE", query, DROP_TABLE_LEN) == 0)
  {
    return DROP_TABLE;
  }

  formattedError = sqlite3_mprintf("Unknown schema modification statement provided: %s", query);
  sqlite3_result_error(context, formattedError, -1);
  sqlite3_free(formattedError);

  return SQLITE_MISUSE;
}

/**
 * The clock table holds the snapshot
 * of the vector clock at the time there was
 * a mutation for a given row.
 *
 * The clock table is structured as a junction table.
 * | row_id | site_id | version
 * +--------+---------+--------
 *   1          a         1
 *   1          b         2
 * ----------------------------
 * @param tableInfo
 */
int cfsql_createClockTable(
    sqlite3 *db,
    cfsql_TableInfo *tableInfo,
    char **err)
{
  char *zSql = 0;
  char *pkList = 0;
  int rc = SQLITE_OK;

  if (tableInfo->pksLen == 0)
  {
    // We never select the clock for a single row by itself
    // hence that row_id is second in the pk def.
    zSql = sqlite3_mprintf("CREATE TABLE \"%s__cfsql_clock\" (\
      \"row_id\" NOT NULL,\
      \"__cfsql_site_id\" NOT NULL,\
      \"__cfsql_version\" NOT NULL,\
      PRIMARY KEY (\"__cfsql_site_id\", \"row_id\")\
    )",
                           tableInfo->tblName);
  }
  else
  {
    pkList = cfsql_asIdentifierList(
        tableInfo->pks,
        tableInfo->pksLen,
        0);
    zSql = sqlite3_mprintf("CREATE TABLE \"%s__cfsql_clock\" (\
      %s,\
      \"__cfsql_site_id\" NOT NULL,\
      \"__cfsql_version\" NOT NULL,\
      PRIMARY KEY (\"__cfsql_site_id\", %s)\
    )",
                           tableInfo->tblName, pkList, pkList);
    sqlite3_free(pkList);
  }

  rc = sqlite3_exec(db, zSql, 0, 0, err);
  sqlite3_free(zSql);
  if (rc != SQLITE_OK)
  {
    return rc;
  }

  return rc;
}

int cfsql_addIndicesToCrrBaseTable(
    sqlite3 *db,
    cfsql_TableInfo *tableInfo,
    char **err)
{
  int rc = SQLITE_OK;
  cfsql_IndexInfo *indexInfo = tableInfo->indexInfo;
  int indexInfoLen = tableInfo->indexInfoLen;
  char *identifierList;
  char *zSql;

  if (indexInfoLen == 0)
  {
    return rc;
  }

  for (int i = 0; i < indexInfoLen; ++i)
  {
    int isPk = strcmp(indexInfo[i].origin, "pk") == 0;
    if (isPk)
    {
      // we create primary keys in the table creation statement
      continue;
    }

    // TODO: we don't yet handle indices created with where clauses
    identifierList = cfsql_asIdentifierListStr(indexInfo[i].indexedCols, indexInfo[i].indexedColsLen, ',');
    zSql = sqlite3_mprintf(
        "CREATE INDEX \"%s\" ON \"%s__cfsql_crr\" (%s)",
        indexInfo[i].name,
        tableInfo->tblName,
        identifierList);
    sqlite3_free(identifierList);

    rc = sqlite3_exec(db, zSql, 0, 0, err);
    sqlite3_free(zSql);
    if (rc != SQLITE_OK)
    {
      return rc;
    }
  }

  return SQLITE_OK;
}

int cfsql_createCrrBaseTable(
    sqlite3 *db,
    cfsql_TableInfo *tableInfo,
    char **err)
{
  int rc = SQLITE_OK;
  char *zSql = 0;
  sqlite3_stmt *pStmt = 0;

  char *columnDefs = cfsql_asColumnDefinitions(
      tableInfo->withVersionCols,
      tableInfo->withVersionColsLen);
  char *pkList = cfsql_asIdentifierList(
      tableInfo->pks,
      tableInfo->pksLen,
      0);
  zSql = sqlite3_mprintf("CREATE TABLE \"%s__cfsql_crr\" (\
    %s,\
    __cfsql_cl INT DEFAULT 1,\
    __cfsql_src INT DEFAULT 0%s\
    %s %s %s %s\
  )",
                         tableInfo->tblName,
                         columnDefs,
                         pkList != 0 ? "," : 0,
                         pkList != 0 ? "PRIMARY KEY" : 0,
                         pkList != 0 ? "(" : 0,
                         pkList,
                         pkList != 0 ? ")" : 0);

  sqlite3_free(pkList);
  sqlite3_free(columnDefs);

  rc = sqlite3_exec(db, zSql, 0, 0, err);
  sqlite3_free(zSql);
  
  if (rc != SQLITE_OK)
  {
    return rc;
  }

  // We actually never need to do this.
  // Unless we're migrating existing tables.
  // rc = cfsql_addIndicesToCrrBaseTable(
  //     db,
  //     tableInfo,
  //     err);
  // if (rc != SQLITE_OK)
  // {
  //   return rc;
  // }

  return SQLITE_OK;
}

/**
 * A view that matches the table as defined by the user
 * and represents the crr to their application.
 */
int cfsql_createViewOfCrr(
    sqlite3 *db,
    cfsql_TableInfo *tableInfo,
    char **err)
{
  int rc = SQLITE_OK;
  char *zSql = 0;
  char *columns = cfsql_asIdentifierList(tableInfo->baseCols, tableInfo->baseColsLen, 0);

  zSql = sqlite3_mprintf("CREATE VIEW \"%s\" \
    AS SELECT %s \
    FROM \
    \"%s__cfsql_crr\" \
    WHERE \
    \"%s__cfsql_crr\".\"__cfsql_cl\" % 2 = 1",
                         tableInfo->tblName,
                         columns,
                         tableInfo->tblName,
                         tableInfo->tblName);
  sqlite3_free(columns);

  rc = sqlite3_exec(db, zSql, 0, 0, err);
  sqlite3_free(zSql);
  return rc;
}

void cfsql_insertConflictResolution() {

}

/**
 * The patch view provides an interface for applying patches
 * to a crr.
 *
 * I.e., inserts can be made
 * against the patch view to sync data from
 * a peer.
 */
int cfsql_createPatchView(
    sqlite3 *db,
    cfsql_TableInfo *tableInfo,
    char **err)
{
  char *zSql = 0;
  int rc = SQLITE_OK;

  zSql = sqlite3_mprintf("CREATE VIEW \"%s__cfsql_patch\" AS SELECT\
    \"%s__cfsql_crr\".*,\
    '{\"fake\": 1}' as __cfsql_clock\
  FROM \"%s__cfsql_crr\"",
                         tableInfo->tblName,
                         tableInfo->tblName,
                         tableInfo->tblName);

  rc = sqlite3_exec(db, zSql, 0, 0, err);
  sqlite3_free(zSql);
  return rc;
}

/**
 * Create a new crr --
 * all triggers, views, tables
 */
static void createCrr(
    sqlite3_context *context,
    sqlite3 *db,
    const char *query)
{
  int rc = SQLITE_OK;
  char *zSql = 0;
  char *err = 0;
  char *tblName = 0;
  cfsql_TableInfo *tableInfo = 0;

  // convert statement to create temp table prefixed with `cfsql_tmp__`
  zSql = sqlite3_mprintf("CREATE TEMP TABLE cfsql_tmp__%s", query + CREATE_TABLE_LEN + 1);
  rc = sqlite3_exec(db, zSql, 0, 0, &err);

  if (rc != SQLITE_OK)
  {
    sqlite3_free(zSql);
    sqlite3_result_error(context, err, -1);
    sqlite3_free(err);
    return;
  }

  // extract the word after CREATE TEMP TABLE cfsql_tmp__
  tblName = cfsql_extractWord(CREATE_TEMP_TABLE_CFSQL_LEN, zSql);
  sqlite3_free(zSql);

  // TODO: we should get table info from the temp table
  // but use the proper table base name.
  char *tmpTblName = sqlite3_mprintf("cfsql_tmp__%s", tblName);
  rc = cfsql_getTableInfo(
      db,
      USER_SPACE,
      tmpTblName,
      &tableInfo,
      &err);

  if (rc != SQLITE_OK)
  {
    sqlite3_free(tblName);
    sqlite3_free(tmpTblName);
    sqlite3_result_error(context, err, -1);
    sqlite3_free(err);
    cfsql_freeTableInfo(tableInfo);
    return;
  }

  // We only needed the temp table to extract pragma info
  zSql = sqlite3_mprintf("DROP TABLE temp.cfsql_tmp__%s", tblName);
  rc = sqlite3_exec(db, zSql, 0, 0, &err);
  sqlite3_free(zSql);
  tableInfo->tblName = strdup(tblName);
  sqlite3_free(tblName);
  sqlite3_free(tmpTblName);
  if (rc != SQLITE_OK)
  {
    sqlite3_result_error(context, err, -1);
    sqlite3_free(err);
    cfsql_freeTableInfo(tableInfo);
    return;
  }

  rc = cfsql_createClockTable(db, tableInfo, &err);
  if (rc == SQLITE_OK)
  {
    rc = cfsql_createCrrBaseTable(db, tableInfo, &err);
  }
  if (rc == SQLITE_OK)
  {
    rc = cfsql_createViewOfCrr(db, tableInfo, &err);
  }
  if (rc == SQLITE_OK)
  {
    rc = cfsql_createPatchView(db, tableInfo, &err);
  }
  if (rc == SQLITE_OK)
  {
    rc = cfsql_createCrrViewTriggers(db, tableInfo, &err);
  }
  if (rc == SQLITE_OK)
  {
    rc = cfsql_createPatchTrigger(db, tableInfo, &err);
  }
  if (rc == SQLITE_OK)
  {
    cfsql_freeTableInfo(tableInfo);
    return;
  }

  sqlite3_result_error(context, err, -1);
  sqlite3_free(err);
}

static void dropCrr()
{
  // drop base table
  // drop clocks table
  // views and triggers should auto-drop
}

static void createCrrIndex()
{
  // just replace the table name with the crr table name. done.
}

static void dropCrrIndex()
{
  // just replace the table name with the crr table name. done.
}

static void alterCrr()
{
  // create crr in tmp table
  // run alter againt tmp crr
  // diff pragma of orig crr and tmp crr
  // determine:
  // - col add
  // - col drop
  // - col rename
  // add: +1
  // rm: -1
  // rename: delta on one
  //
  // rename:
  // drop triggers and views
  // rename col on base crr (and version col.. if need be)
  // recreate triggers and views based on new crr pragma
  //
  // add:
  // same as above but add col on step 2
  //
  // remove:
  // remove col on step 2
}

/**
 * Interface to create and modify crrs.
 *
 * `select cfsql('CREATE TABLE foo (bar)')`
 *
 * Handles:
 * - create table
 * - drop table
 * - create index
 * - drop index
 * - alter table
 */
static void cfsqlFunc(sqlite3_context *context, int argc, sqlite3_value **argv)
{
  char *query = 0;
  int rc = SQLITE_OK;
  char *found = 0;
  int queryType = -1;
  sqlite3 *db = sqlite3_context_db_handle(context);
  char *errmsg = 0;
  sqlite3_stmt *pStmt = 0;
  char *normalized = 0;

  query = (char *)sqlite3_value_text(argv[0]);
  found = strstr(query, ";");

  if (found != NULL)
  {
    sqlite3_result_error(context, "You may not pass multiple statements to cfsql yet. Run one statement at a time.", -1);
    return;
  }

  // Prepare a statement so we can normalize the query.
  // Normalizing the query strips comments, extra whitespace
  // and just puts it in a format that we can easily parse.
  rc = sqlite3_prepare_v2(db, query, -1, &pStmt, 0);
  if (rc != SQLITE_OK)
  {
    sqlite3_result_error(context, sqlite3_errmsg(db), -1);
    return;
  }
  query = strdup(sqlite3_normalized_sql(pStmt));
  sqlite3_finalize(pStmt);

  queryType = determineQueryType(db, context, query);
  // TODO: likely need this to be a sub-transaction
  rc = sqlite3_exec(db, "BEGIN", 0, 0, &errmsg);
  if (rc != SQLITE_OK)
  {
    sqlite3_result_error(context, errmsg, -1);
    sqlite3_free(errmsg);
    return;
  }

  switch (queryType)
  {
  case CREATE_TABLE:
    createCrr(context, db, query);
    break;
  case DROP_TABLE:
    dropCrr();
    break;
  case CREATE_INDEX:
    createCrrIndex();
    break;
  case DROP_INDEX:
    dropCrrIndex();
    break;
  case ALTER_TABLE:
    alterCrr();
    break;
  default:
    break;
  }

  sqlite3_exec(db, "COMMIT", 0, 0, 0);
}

// todo: install a commit_hook to advance the dbversion on every tx commit

// get_changes_since function

// vector_short -- centralized resolver(s)

#ifdef _WIN32
__declspec(dllexport)
#endif
    int sqlite3_cfsqlite_init(sqlite3 *db, char **pzErrMsg,
                              const sqlite3_api_routines *pApi)
{
  int rc = SQLITE_OK;

  SQLITE_EXTENSION_INIT2(pApi);

  /**
   * Initialization creates a number of tables.
   * We should ensure we do these in a tx
   * so we cannot have partial initialization.
   */
  rc = sqlite3_exec(db, "BEGIN", 0, 0, 0);
  if (rc != SQLITE_OK)
  {
    return rc;
  }

  // TODO: we need to guard cfsql initialization with a mutex
  // since we set shared variables for site id and db version.
  // TODO: we'll need to CAS on writes to db version in the commit hook.
  // __sync_val_compare_and_swap
  // https://gcc.gnu.org/onlinedocs/gcc-4.1.0/gcc/Atomic-Builtins.html
  rc = initSiteId(db);
  if (rc != SQLITE_OK)
  {
    sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    return rc;
  }
  // once site id is initialize, we are able to init db version.
  // db version uses site id in its queries hence why it comes after site id init.
  rc = initDbVersion(db);
  if (rc != SQLITE_OK)
  {
    sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    return rc;
  }

  rc = sqlite3_exec(db, "COMMIT", 0, 0, 0);
  if (rc != SQLITE_OK)
  {
    return rc;
  }

  if (rc == SQLITE_OK)
  {
    rc = sqlite3_create_function(db, "cfsql_siteid", 0,
                                 // siteid never changes -- deterministic and innnocuous
                                 SQLITE_UTF8 | SQLITE_INNOCUOUS |
                                     SQLITE_DETERMINISTIC,
                                 0, siteIdFunc, 0, 0);
  }
  if (rc == SQLITE_OK)
  {
    rc = sqlite3_create_function(db, "cfsql_dbversion", 0,
                                 // dbversion can change on each invocation.
                                 SQLITE_UTF8 | SQLITE_INNOCUOUS,
                                 0, dbVersionFunc, 0, 0);
  }
  if (rc == SQLITE_OK)
  {
    rc = sqlite3_create_function(db, "cfsql", 1,
                                 // cfsql should only ever be used at the top level
                                 // and does a great deal to modify
                                 // existing database state. directonly.
                                 SQLITE_UTF8 | SQLITE_DIRECTONLY,
                                 0, cfsqlFunc, 0, 0);
  }

  return rc;
}
