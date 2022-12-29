import * as React from "react";
import { createRoot } from "react-dom/client";

import sqliteWasm from "@vlcn.io/wa-crsqlite";
import startSync from "@vlcn.io/sync-client";
import tblrx from "@vlcn.io/rx-tbl";
import { Ctx } from "./ctx";

// @ts-ignore
import wasmUrl from "@vlcn.io/wa-crsqlite/wa-sqlite-async.wasm?url";
import App from "./App";

async function main() {
  const sqlite = await sqliteWasm(() => wasmUrl);

  const db = await sqlite.open("wdb-yjs");
  (window as any).db = db;

  const rx = tblrx(db);
  const sync = await startSync({
    localDb: db,
    // the id of the database to persist into on the server.
    // if a db with that id does not exist it can be created for you
    remoteDbId: "a0436f45-82bf-412d-886d-bbdedf6b9986",
    uri: `ws://${window.location.hostname}:8080/sync`,
    // the schema to apply to the db if it does not exist
    // TODO: validate that the opened db has the desired schema and version of that schema?
    create: {
      schemaName: "yjs-example",
    },
    rx,
  });

  const ctx: Ctx = {
    db,
    sync,
    rx,
  };

  window.onbeforeunload = () => {
    return db.close();
  };
  startApp(ctx);
}

function startApp(ctx: Ctx) {
  (window as any).ctx = ctx;
  const root = createRoot(document.getElementById("container")!);
  root.render(<App ctx={ctx} />);
}

main();
