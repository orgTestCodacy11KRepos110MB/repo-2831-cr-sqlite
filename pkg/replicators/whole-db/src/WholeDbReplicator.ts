import { DB } from "@vlcn.io/crsqlite-wasm";
import { parse as uuidParse, stringify as uuidStringify } from "uuid";
type SiteIDWire = string;
type SiteIDLocal = Uint8Array;
type CID = string;
type QuoteConcatedPKs = string;
type TableName = string;
type Version = string;
type TODO = any;

/**
 * The `poke` protocol is the simplest option in terms of
 * - causal delivery of messages
 * - retry on drop
 */
interface PokeProtocol {
  /**
   * Tell all connected sites that we have updates from this site
   * ending at `pokerVersion`
   */
  poke(poker: SiteIDWire, pokerVersion: BigInt): void;

  /**
   * Push changes to the given site in response to their request for changes.
   */
  pushChanges(to: SiteIDWire, changesets: Changeset[]): void;

  /**
   * Request changes from a given site since a given version
   * in response to a poke from that site.
   */
  requestChanges(from: SiteIDWire, since: BigInt): void;

  /**
   * Receive a poke froma given site.
   * In response, we'll compute what changes we're missing from that site
   * and request those changes.
   */
  onPoked(cb: (pokedBy: SiteIDWire, pokerVersion: BigInt) => void): void;

  /**
   * When someone new connects we can just poke them to kick off
   * initial sync. Simple.
   */
  onNewConnection(cb: (siteID: SiteIDWire) => void): void;

  /**
   * A peer has requested changes from us.
   */
  onChangesRequested(cb: (from: SiteIDWire, since: BigInt) => void): void;

  /**
   * We have received changes from a peer.
   */
  onChangesReceived(cb: (changesets: Changeset[]) => void): void;

  dispose(): void;
}

type Changeset = [
  TableName,
  QuoteConcatedPKs,
  CID,
  any, // val,
  Version,
  SiteIDWire // site_id
];

const api = {
  install(db: DB, network: PokeProtocol): WholeDbReplicator {
    const ret = new WholeDbReplicator(db, network);
    return ret;
  },
};

// TODO: we need to handle initial sync.
// Well, that should be easy. Just poke people on connect.

class WholeDbReplicator {
  private crrs: string[] = [];
  private pendingNotification = false;
  private siteId: SiteIDLocal;
  private siteIdWire: SiteIDWire;

  constructor(private db: DB, private network: PokeProtocol) {
    this.db = db;
    db.createFunction("crsql_wdbreplicator", this.crrChanged);

    this.siteId = db.execA("SELECT crsql_siteid()")[0][0];
    this.siteIdWire = uuidStringify(this.siteId);
    this.installTriggers();
    this.createPeerTrackingTable();

    this.network.onPoked(this.poked);
    this.network.onNewConnection(this.newConnection);
    this.network.onChangesReceived(this.changesReceived);
    this.network.onChangesRequested(this.changesRequested);
  }

  dispose() {
    // remove trigger(s)
    // function extension is fine to stay, it'll get removed on connection termination
    this.crrs.forEach((crr) => {
      this.db.exec(`DROP TRIGGER IF EXISTS "${crr}__crsql_wdbreplicator";`);
    });

    this.network.dispose();
  }

  schemaChanged() {
    this.installTriggers();
  }

  private installTriggers() {
    // find all crr tables
    // TODO: ensure we all not notified
    // if we're in the process of applying sync changes.
    // TODO: we can also just track that internally.
    const crrs = this.db.execA(
      "SELECT name FROM sqlite_master WHERE name LIKE '%__crsql_clock'"
    );
    crrs.forEach((crr) => {
      this.db.exec(
        `CREATE TRIGGER "${crr}__crsql_wdbreplicator" AFTER UPDATE ON "${crr}"
        BEGIN
          select crsql_wdbreplicator() WHERE crsql_internal_sync_bit() = 0;
        END;
      `
      );
    });
    this.crrs = crrs;
  }

  private createPeerTrackingTable() {
    this.db.exec(
      "CREATE TABLE IF NOT EXISTS __crsql_wdbreplicator_peers (site_id primary key, version)"
    );
  }

  private crrChanged = (context: TODO) => {
    if (this.pendingNotification) {
      return;
    }

    this.pendingNotification = true;
    queueMicrotask(() => {
      this.pendingNotification = false;
      const dbv = this.db.execA("SELECT crsql_dbversion()")[0][0];
      this.network.poke(this.siteIdWire, dbv);
    });
  };

  private poked = (pokedBy: SiteIDWire, pokerVersion: BigInt) => {
    const stmt = this.db.prepare(
      "SELECT version FROM __crsql_wdbreplicator_peers WHERE site_id = ?"
    );
    let ourVersionForPoker: BigInt | null = null;
    try {
      stmt.bindBlob(uuidParse(pokedBy));
      stmt.step();
      ourVersionForPoker = BigInt(stmt.get(1));
    } finally {
      stmt.finalize();
    }

    if (ourVersionForPoker == null) {
      throw new Error("unable to determin db version to return to poker");
    }

    // the poker version can be less than our version for poker if a set of
    // poke messages were queued up behind a sync.
    if (pokerVersion <= ourVersionForPoker) {
      return;
    }

    // ask the poker for changes since our version
    this.network.requestChanges(this.siteIdWire, ourVersionForPoker);
  };

  private newConnection = (siteId: SiteIDWire) => {
    // treat it as a crr change so we can kick off sync
    this.crrChanged(null);
  };

  // if we fail to apply, re-request
  // TODO: other retry mechanisms
  private changesReceived = (changesets: Changeset[]) => {
    const stmt = this.db.prepare(
      "INSERT INTO crsql_changes VALUES (?, ?, ?, ?, ?, ?)"
    );
    // TODO: may want to chunk
    try {
      changesets.forEach((cs) => {
        for (let i = 1; i <= 6; ++i) {
          if (i == 6) {
            stmt.bindBlob(i, uuidParse(cs[i - 1]));
          } else {
            stmt.bind(i, cs[i - 1]);
          }
        }
        stmt.step();
        stmt.reset(true);
      });
    } finally {
      stmt.finalize();
    }
  };

  private changesRequested = (from: SiteIDWire, since: BigInt) => {
    const fromAsBlob = uuidParse(from);
    const stmt = this.db.prepare(
      "SELECT * FROM crsql_changes WHERE site_id != ? AND version > ?"
    );

    const changes: any[] = [];
    try {
      stmt.bindBlob(1, fromAsBlob);
      stmt.bind(2, since);
      while (stmt.step()) {
        const row: any[] = [];
        stmt.get(row);
        changes.push(row);
      }
    } finally {
      stmt.finalize();
    }

    if (changes.length == 0) {
      return;
    }
    this.network.pushChanges(from, changes);
  };
}

export default api;
