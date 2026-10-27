PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA busy_timeout = 5000;

CREATE TABLE IF NOT EXISTS metadata (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS mqtt_events (
  row_id INTEGER PRIMARY KEY AUTOINCREMENT,
  event_id TEXT UNIQUE,
  topic TEXT NOT NULL,
  kind TEXT NOT NULL,
  schema_version TEXT,
  node_id TEXT,
  trace_id TEXT,
  qos INTEGER NOT NULL,
  retained INTEGER NOT NULL CHECK (retained IN (0, 1)),
  valid INTEGER NOT NULL CHECK (valid IN (0, 1)),
  validation_error TEXT,
  payload_json TEXT NOT NULL,
  first_received_at_ms INTEGER NOT NULL,
  last_received_at_ms INTEGER NOT NULL,
  receiver_boot_id TEXT,
  first_received_uptime_ms INTEGER,
  last_received_uptime_ms INTEGER,
  first_receiver_time_valid INTEGER CHECK (first_receiver_time_valid IN (0, 1)),
  last_receiver_time_valid INTEGER CHECK (last_receiver_time_valid IN (0, 1)),
  delivery_count INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS boot_sessions (
  boot_id TEXT PRIMARY KEY,
  first_seen_at_ms INTEGER NOT NULL,
  first_seen_uptime_ms INTEGER NOT NULL,
  first_time_valid INTEGER NOT NULL CHECK (first_time_valid IN (0, 1)),
  last_seen_at_ms INTEGER NOT NULL,
  last_seen_uptime_ms INTEGER NOT NULL,
  last_time_valid INTEGER NOT NULL CHECK (last_time_valid IN (0, 1)),
  time_became_valid_at_ms INTEGER,
  time_became_valid_uptime_ms INTEGER
);

CREATE TABLE IF NOT EXISTS commands (
  command_id TEXT PRIMARY KEY,
  event_id TEXT NOT NULL,
  task_id TEXT,
  trace_id TEXT NOT NULL,
  target_node_id TEXT NOT NULL,
  target_device_id TEXT NOT NULL,
  action TEXT NOT NULL,
  parameters_json TEXT NOT NULL,
  issued_at_ms INTEGER NOT NULL,
  expires_at_ms INTEGER NOT NULL,
  latest_attempt INTEGER NOT NULL,
  status TEXT NOT NULL,
  first_received_at_ms INTEGER NOT NULL,
  updated_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS acknowledgements (
  event_id TEXT PRIMARY KEY,
  command_id TEXT NOT NULL,
  trace_id TEXT NOT NULL,
  node_id TEXT NOT NULL,
  target_device_id TEXT NOT NULL,
  ok INTEGER NOT NULL CHECK (ok IN (0, 1)),
  duplicate INTEGER NOT NULL CHECK (duplicate IN (0, 1)),
  status TEXT NOT NULL,
  executed_at_ms INTEGER NOT NULL,
  result_json TEXT,
  error_json TEXT,
  received_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS telemetry (
  event_id TEXT PRIMARY KEY,
  trace_id TEXT NOT NULL,
  node_id TEXT NOT NULL,
  seq INTEGER NOT NULL,
  sent_at_ms INTEGER NOT NULL,
  uptime_ms INTEGER NOT NULL,
  sensor_ready INTEGER NOT NULL CHECK (sensor_ready IN (0, 1)),
  simulated_value REAL,
  led_on INTEGER,
  readings_json TEXT,
  received_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS heartbeats (
  event_id TEXT PRIMARY KEY,
  trace_id TEXT NOT NULL,
  node_id TEXT NOT NULL,
  node_type TEXT NOT NULL,
  status TEXT NOT NULL,
  seq INTEGER NOT NULL,
  sent_at_ms INTEGER NOT NULL,
  uptime_ms INTEGER NOT NULL,
  version TEXT,
  received_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS devices (
  node_id TEXT PRIMARY KEY,
  node_type TEXT,
  status TEXT NOT NULL,
  last_event_id TEXT,
  last_seq INTEGER,
  last_sent_at_ms INTEGER,
  last_seen_at_ms INTEGER NOT NULL,
  version TEXT,
  updated_at_ms INTEGER NOT NULL,
  last_seen_receiver_boot_id TEXT,
  last_seen_receiver_uptime_ms INTEGER
);

CREATE TABLE IF NOT EXISTS storage_status (
  singleton_id INTEGER PRIMARY KEY CHECK (singleton_id = 1),
  checked_at_ms INTEGER NOT NULL,
  database_bytes INTEGER NOT NULL,
  filesystem_total_bytes INTEGER NOT NULL,
  filesystem_available_bytes INTEGER NOT NULL,
  filesystem_used_percent REAL NOT NULL,
  warning_threshold_percent INTEGER NOT NULL,
  warning_active INTEGER NOT NULL CHECK (warning_active IN (0, 1))
);

CREATE TABLE IF NOT EXISTS tasks (
  task_id TEXT PRIMARY KEY,
  request_event_id TEXT NOT NULL UNIQUE,
  trace_id TEXT NOT NULL,
  source_node_id TEXT NOT NULL,
  intent TEXT NOT NULL,
  target_device TEXT NOT NULL,
  priority INTEGER NOT NULL CHECK (priority BETWEEN 0 AND 100),
  safety_level TEXT NOT NULL,
  state TEXT NOT NULL,
  interruptible INTEGER NOT NULL CHECK (interruptible IN (0, 1)),
  resume_policy TEXT NOT NULL,
  confidence REAL NOT NULL CHECK (confidence >= 0.0 AND confidence <= 1.0),
  parameters_json TEXT NOT NULL,
  requested_at_ms INTEGER NOT NULL,
  created_at_ms INTEGER NOT NULL,
  updated_at_ms INTEGER NOT NULL,
  revision INTEGER NOT NULL DEFAULT 1,
  action_token TEXT NOT NULL,
  rejection_code TEXT,
  rejection_message TEXT
);

CREATE TABLE IF NOT EXISTS task_transitions (
  transition_id INTEGER PRIMARY KEY AUTOINCREMENT,
  event_id TEXT NOT NULL UNIQUE,
  task_id TEXT NOT NULL,
  trace_id TEXT NOT NULL,
  from_state TEXT,
  to_state TEXT NOT NULL,
  reason TEXT NOT NULL,
  actor TEXT NOT NULL,
  changed_at_ms INTEGER NOT NULL,
  FOREIGN KEY(task_id) REFERENCES tasks(task_id)
);

CREATE TABLE IF NOT EXISTS perceptions (
  event_id TEXT PRIMARY KEY,
  trace_id TEXT NOT NULL,
  source_node_id TEXT NOT NULL,
  observation_type TEXT NOT NULL,
  zone TEXT NOT NULL,
  confidence REAL NOT NULL CHECK (confidence >= 0.0 AND confidence <= 1.0),
  attributes_json TEXT NOT NULL,
  observed_at_ms INTEGER NOT NULL,
  received_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS world_state (
  state_key TEXT PRIMARY KEY,
  value_json TEXT NOT NULL,
  confidence REAL NOT NULL CHECK (confidence >= 0.0 AND confidence <= 1.0),
  source_node_id TEXT NOT NULL,
  trace_id TEXT NOT NULL,
  observation_event_id TEXT NOT NULL,
  observed_at_ms INTEGER NOT NULL,
  updated_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS user_feedback (
  event_id TEXT PRIMARY KEY,
  trace_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  client_id TEXT NOT NULL,
  action TEXT NOT NULL,
  reason TEXT,
  accepted INTEGER NOT NULL CHECK (accepted IN (0, 1)),
  error_text TEXT,
  sent_at_ms INTEGER NOT NULL,
  received_at_ms INTEGER NOT NULL,
  FOREIGN KEY(task_id) REFERENCES tasks(task_id)
);

CREATE TABLE IF NOT EXISTS safety_alerts (
  alert_id TEXT PRIMARY KEY,
  event_id TEXT NOT NULL UNIQUE,
  trace_id TEXT NOT NULL,
  source_node_id TEXT NOT NULL,
  alert_type TEXT NOT NULL,
  zone TEXT NOT NULL,
  severity TEXT NOT NULL,
  summary TEXT NOT NULL,
  evidence_json TEXT NOT NULL,
  automatic_actions_json TEXT NOT NULL,
  status TEXT NOT NULL,
  raised_at_ms INTEGER NOT NULL,
  acknowledged_at_ms INTEGER,
  updated_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS safety_acknowledgements (
  event_id TEXT PRIMARY KEY,
  trace_id TEXT NOT NULL,
  alert_id TEXT NOT NULL,
  client_id TEXT NOT NULL,
  action TEXT NOT NULL,
  accepted INTEGER NOT NULL CHECK (accepted IN (0, 1)),
  error_text TEXT,
  sent_at_ms INTEGER NOT NULL,
  received_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS privacy_events (
  event_id TEXT PRIMARY KEY,
  trace_id TEXT NOT NULL,
  source_id TEXT NOT NULL,
  event_kind TEXT NOT NULL,
  mode TEXT NOT NULL,
  camera_allowed INTEGER NOT NULL CHECK (camera_allowed IN (0, 1)),
  microphone_allowed INTEGER NOT NULL CHECK (microphone_allowed IN (0, 1)),
  recording INTEGER NOT NULL CHECK (recording IN (0, 1)),
  local_processing INTEGER NOT NULL CHECK (local_processing IN (0, 1)),
  event_at_ms INTEGER NOT NULL,
  received_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS privacy_current (
  singleton_id INTEGER PRIMARY KEY CHECK (singleton_id = 1),
  event_id TEXT NOT NULL,
  trace_id TEXT NOT NULL,
  source_node_id TEXT NOT NULL,
  mode TEXT NOT NULL,
  camera_allowed INTEGER NOT NULL CHECK (camera_allowed IN (0, 1)),
  microphone_allowed INTEGER NOT NULL CHECK (microphone_allowed IN (0, 1)),
  recording INTEGER NOT NULL CHECK (recording IN (0, 1)),
  local_processing INTEGER NOT NULL CHECK (local_processing IN (0, 1)),
  changed_at_ms INTEGER NOT NULL,
  updated_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS feedback_events (
  event_id TEXT PRIMARY KEY,
  trace_id TEXT NOT NULL,
  client_id TEXT NOT NULL,
  feedback_type TEXT NOT NULL,
  task_id TEXT,
  observation_event_id TEXT,
  rating TEXT NOT NULL,
  note TEXT,
  sent_at_ms INTEGER NOT NULL,
  received_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS system_health_reports (
  event_id TEXT PRIMARY KEY,
  trace_id TEXT NOT NULL,
  source_node_id TEXT NOT NULL,
  overall_status TEXT NOT NULL,
  components_json TEXT NOT NULL,
  storage_json TEXT NOT NULL,
  generated_at_ms INTEGER NOT NULL,
  received_at_ms INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_events_topic_received
  ON mqtt_events(topic, first_received_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_events_trace
  ON mqtt_events(trace_id);
CREATE INDEX IF NOT EXISTS idx_commands_trace
  ON commands(trace_id);
CREATE INDEX IF NOT EXISTS idx_commands_target_status
  ON commands(target_node_id, status, updated_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_ack_command
  ON acknowledgements(command_id, received_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_telemetry_node_time
  ON telemetry(node_id, received_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_telemetry_node_seq
  ON telemetry(node_id, seq);
CREATE INDEX IF NOT EXISTS idx_heartbeat_node_time
  ON heartbeats(node_id, received_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_tasks_state_priority
  ON tasks(state, priority DESC, updated_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_tasks_trace ON tasks(trace_id);
CREATE INDEX IF NOT EXISTS idx_task_transitions_task
  ON task_transitions(task_id, changed_at_ms);
CREATE INDEX IF NOT EXISTS idx_perceptions_zone_time
  ON perceptions(zone, observed_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_feedback_task_time
  ON user_feedback(task_id, received_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_safety_status_time
  ON safety_alerts(status, raised_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_feedback_events_time
  ON feedback_events(received_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_health_source_time
  ON system_health_reports(source_node_id, generated_at_ms DESC);

INSERT INTO metadata(key, value) VALUES('schema_version', '5')
ON CONFLICT(key) DO UPDATE SET value=excluded.value;
INSERT INTO metadata(key, value) VALUES('automatic_delete_enabled', 'false')
ON CONFLICT(key) DO UPDATE SET value=excluded.value;
INSERT INTO metadata(key, value) VALUES('storage_warning_percent', '70')
ON CONFLICT(key) DO UPDATE SET value=excluded.value;
