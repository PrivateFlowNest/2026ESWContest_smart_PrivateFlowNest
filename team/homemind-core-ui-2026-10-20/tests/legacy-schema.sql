PRAGMA journal_mode = WAL;

CREATE TABLE metadata (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

CREATE TABLE mqtt_events (
  row_id INTEGER PRIMARY KEY AUTOINCREMENT,
  event_id TEXT UNIQUE,
  topic TEXT NOT NULL,
  kind TEXT NOT NULL,
  schema_version TEXT,
  node_id TEXT,
  trace_id TEXT,
  qos INTEGER NOT NULL,
  retained INTEGER NOT NULL,
  valid INTEGER NOT NULL,
  validation_error TEXT,
  payload_json TEXT NOT NULL,
  first_received_at_ms INTEGER NOT NULL,
  last_received_at_ms INTEGER NOT NULL,
  delivery_count INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE boot_sessions (
  boot_id TEXT PRIMARY KEY,
  first_seen_at_ms INTEGER NOT NULL,
  first_seen_uptime_ms INTEGER NOT NULL,
  first_time_valid INTEGER NOT NULL,
  last_seen_at_ms INTEGER NOT NULL,
  last_seen_uptime_ms INTEGER NOT NULL,
  last_time_valid INTEGER NOT NULL,
  time_became_valid_at_ms INTEGER
);

CREATE TABLE devices (
  node_id TEXT PRIMARY KEY,
  node_type TEXT,
  status TEXT NOT NULL,
  last_event_id TEXT,
  last_seq INTEGER,
  last_sent_at_ms INTEGER,
  last_seen_at_ms INTEGER NOT NULL,
  version TEXT,
  updated_at_ms INTEGER NOT NULL
);

INSERT INTO metadata(key, value) VALUES('schema_version', '2');
