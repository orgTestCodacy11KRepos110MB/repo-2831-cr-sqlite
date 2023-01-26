use sqlite_nostd::{context, sqlite3, Connection, Context, ResultCode, Value};
extern crate alloc;
use alloc::format;
use alloc::vec::Vec;

use crate::{
    fractindex_view::create_fract_view_and_triggers,
    util::{collection_max_select, collection_min_select},
};

pub fn as_ordered(
    context: *mut context,
    db: *mut sqlite3,
    table: &str,
    order_by_column: *mut sqlite_nostd::value,
    collection_columns: &[*mut sqlite_nostd::value],
) {
    // 0. we should drop all triggers and views if they already exist
    // or be fancy and track schema versions to know if this is needed.

    // 1. ensure that all columns exist in the target table
    let mut collection_columns = collection_columns.to_vec();
    collection_columns.push(order_by_column);
    let rc = table_has_all_columns(db, table, &collection_columns);

    if rc.is_err() {
        context.result_error("Failed determining if all columns are present in the base table");
        return;
    }
    if let Ok(false) = rc {
        context.result_error("all columns are not present in the base table");
        return;
    }

    // 2. write into our __crsql_master table the information about the index
    if let Err(_) = db.exec_safe("SAVEPOINT record_schema_information;") {
        return;
    }
    let rc = record_schema_information(db, table, order_by_column, &collection_columns);
    if rc.is_err() {
        let _ = db.exec_safe("ROLLBACK;");
        context.result_error("Failed recording schema information for the base table");
        return;
    }

    // 3. set up triggers to allow for append and pre-pend insertions
    if let Err(_) = create_pend_trigger(db, table, order_by_column, &collection_columns) {
        let _ = db.exec_safe("ROLLBACK;");
        context.result_error("Failed creating triggers for the base table");
        return;
    }

    // 4. create fract view for insert after and move operations
    if let Err(_) = create_fract_view_and_triggers(db, table, order_by_column, &collection_columns)
    {
        let _ = db.exec_safe("ROLLBACK;");
        context.result_error("Failed creating view for the base table");
        return;
    }

    let _ = db.exec_safe("RELEASE;");
}

fn record_schema_information(
    db: *mut sqlite3,
    table: &str,
    order_by_column: *mut sqlite_nostd::value,
    collection_columns: &[*mut sqlite_nostd::value],
) -> Result<ResultCode, ResultCode> {
    // TODO: start a savepoint
    let sql = "INSERT OR REPLACE INTO __crsql_master (type, name, augments) VALUES (?, ?, ?, ?, ?) RETURNING id";
    let stmt = db.prepare_v2(sql)?;
    stmt.bind_text(1, "fract_index")?;
    stmt.bind_text(2, table)?;
    stmt.bind_text(3, table)?;
    stmt.step()?;

    let id = stmt.column_int64(0)?;

    let sql =
        "INSERT OR REPLACE INTO __crsql_master_prop (master_id, key, ord, value) VALUES (?, ?, ?, ?)";
    let stmt = db.prepare_v2(sql)?;
    stmt.bind_int64(1, id)?;
    stmt.bind_text(2, "order_by")?;
    stmt.bind_int(3, 0)?;
    stmt.bind_value(4, order_by_column)?;
    stmt.step()?;
    stmt.reset()?;

    for (i, col) in collection_columns.iter().enumerate() {
        stmt.bind_int64(1, id)?;
        stmt.bind_text(2, "collection")?;
        stmt.bind_int(3, i as i32)?;
        stmt.bind_value(4, *col)?;
        stmt.step()?;
        stmt.reset()?;
    }

    Ok(ResultCode::OK)
}

fn table_has_all_columns(
    db: *mut sqlite3,
    table: &str,
    columns: &Vec<*mut sqlite_nostd::value>,
) -> Result<bool, ResultCode> {
    let bindings = columns.iter().map(|_| "?").collect::<Vec<_>>().join(", ");
    let sql = format!(
        "SELECT count(*) FROM pragma_table_info(?) WHERE \"name\" in ({})",
        bindings
    );
    let stmt = db.prepare_v2(&sql)?;
    stmt.bind_text(1, table)?;
    for (i, col) in columns.iter().enumerate() {
        stmt.bind_value((i + 2) as i32, *col)?;
    }

    let step_code = stmt.step()?;
    if step_code == ResultCode::ROW {
        let count = stmt.column_int(0)?;
        if count != columns.len() as i32 {
            return Ok(false);
        }
    }

    Ok(true)
}

fn create_pend_trigger(
    db: *mut sqlite3,
    table: &str,
    order_by_column: *mut sqlite_nostd::value,
    collection_columns: &[*mut sqlite_nostd::value],
) -> Result<ResultCode, ResultCode> {
    let trigger = format!(
        "CREATE TRIGGER IF NOT EXISTS __crsql_fractindex_pend_trig AFTER INSERT ON {table}
        WHEN NEW.{order_by_column} = 0 OR NEW.{order_by_column} = 1 BEGIN
            UPDATE {table} SET {order_by_column} = CASE NEW.{order_by_column}
            WHEN
                0 THEN crsql_fract_key_between(NULL, ({min_select}))
                1 THEN crsql_fract_key_between(({max_select}), NULL)
            END
            WHERE _rowid_ = NEW._rowid_;
        END;",
        table = table,
        order_by_column = order_by_column.text(),
        min_select = collection_min_select(table, order_by_column, collection_columns)?,
        max_select = collection_max_select(table, order_by_column, collection_columns)?
    );
    db.exec_safe(&trigger)
}
