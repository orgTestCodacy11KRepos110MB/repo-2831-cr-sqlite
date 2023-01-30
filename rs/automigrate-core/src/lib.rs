#![no_std]
mod automigrate;
extern crate alloc;
use alloc::string::String;
use core::ffi::c_char;
use core::slice;
use sqlite_nostd as sqlite;

use sqlite::Connection;
use sqlite::Context;
use sqlite::Value;

pub use automigrate::*;

pub extern "C" fn crsql_automigrate(
    ctx: *mut sqlite::context,
    argc: i32,
    argv: *mut *mut sqlite::value,
) {
    let args = sqlite::args!(argc, argv);
    if argc != 1 {
        ctx.result_error("expected 1 argument");
        return;
    }

    // todo: start save pont
    let result = automigrate::automigrate(db, args[0].text());
    if let Err(err) = result {
        // todo: rollback savepoint
        ctx.result_error("failed to migrate");
        return;
    }
    // todo: commit savepoint

    ctx.result_text_owned(String::from("migrated"));
}

// fn pull_schema_version(schema: &str) {
//     // pull first line
// }

#[no_mangle]
pub extern "C" fn sqlite3_crsqlautomigrate_init(
    db: *mut sqlite::sqlite3,
    _err_msg: *mut *mut c_char,
    api: *mut sqlite::api_routines,
) -> u32 {
    sqlite::EXTENSION_INIT2(api);

    db.create_function_v2(
        "crsql_automigrate",
        1,
        sqlite::UTF8,
        None,
        Some(crsql_automigrate),
        None,
        None,
        None,
    )
    .unwrap_or(sqlite::ResultCode::ERROR) as u32
}
