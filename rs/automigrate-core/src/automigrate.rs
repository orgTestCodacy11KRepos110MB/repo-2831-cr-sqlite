use sqlite_nostd::ResultCode;

extern crate alloc;

use sqlite_nostd as sqlite;

pub fn automigrate(db: &mut sqlite::Connection, schema: &str) -> Result<ResultCode, ResultCode> {
    // extension might not be loaded in a runtime loadable extension environment
    // so we'd need to drop all `crsql_as_crr` statements. Re-write them to no-ops?
    let tempdb = sqlite::open(sqlite::strlit!(":memory:"))?;

    /*
     * The automigrate algorithm:
     * 1. Pull the supplied schema version of the input string
     * 2. Ensure it is greater than db's current schema version
     * 3. open a new in-memory db (w crsqlite loaded in the mem db -- detect via pragma query)
     * 4. apply supplied schema against the memory db
     * 5. find dropped tables
     * 6. find new tables
     * 7. find modified tables
     *
     * Modified tables:
     * 1. find new columns
     * 2. find dropped columns
     * 3. find modified columns -- we can't do this given we don't have a stable identifier for columns
     *   -- well we could if only type information on the columns changed or primary key participation changed
     *   -- need to also figure out index changes
     */
}
