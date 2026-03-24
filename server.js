/*
  Museum IoT Server — Complete with:
  - Visitor counting (IR sensors)
  - Phone detection (Wi-Fi probe)
  - Events log (real-time sync)
  - Reset flag (syncs simulator + dashboard)
  - Multi-room capacity alerts
*/

const express = require('express');
const cors    = require('cors');
const app     = express();

app.use(cors({ origin: '*', methods: ['GET','POST','OPTIONS'], allowedHeaders: ['Content-Type'] }));
app.options('*', cors());
app.use(express.json());

// ─── Stores ───────────────────────────────────────────────────────────
const rooms      = {};   // roomId → latest room data
const events     = [];   // last 200 events (entry/exit/phone/reset)
const phones     = {};   // roomId → phone detection data
let   resetFlag  = 0;    // timestamp of last reset — clients check this

// ─── POST /api/visitor ────────────────────────────────────────────────
app.post('/api/visitor', (req, res) => {
  const { room_id, room_name, current_count, total_entries, total_exits, capacity, timestamp, event_type } = req.body;
  if (!room_id) return res.status(400).json({ error: 'room_id required' });

  rooms[room_id] = {
    room_id, room_name,
    current_count: parseInt(current_count) || 0,
    total_entries: parseInt(total_entries) || 0,
    total_exits:   parseInt(total_exits)   || 0,
    capacity:      parseInt(capacity)      || 100,
    timestamp, last_seen: Date.now(),
    occupancy_pct: Math.round((parseInt(current_count)||0) / (parseInt(capacity)||100) * 100),
  };

  if (event_type && event_type !== 'reset') {
    events.push({
      type: event_type,
      room_id, room_name,
      count: parseInt(current_count) || 0,
      time: new Date().toTimeString().slice(0,5),
      received_at: Date.now(),
    });
    if (events.length > 200) events.shift();
  }

  console.log(`[${event_type||'UPDATE'}] ${room_name}: ${current_count}/${capacity}`);
  res.json({ ok: true });
});

// ─── GET /api/rooms ───────────────────────────────────────────────────
app.get('/api/rooms', (req, res) => {
  const list = Object.values(rooms);
  res.json({
    rooms: list,
    total_visitors: list.reduce((s,r) => s + r.current_count, 0),
    reset_flag: resetFlag,
  });
});

// ─── GET /api/events ──────────────────────────────────────────────────
app.get('/api/events', (req, res) => {
  const since    = parseInt(req.query.since) || 0;
  const filtered = events.filter(e => e.received_at > since);
  res.json({ events: filtered, server_time: Date.now(), reset_flag: resetFlag });
});

// ─── POST /api/phone ──────────────────────────────────────────────────
app.post('/api/phone', (req, res) => {
  const { room_id, room_name, phone_detected, timestamp } = req.body;
  if (!room_id) return res.status(400).json({ error: 'room_id required' });

  phones[room_id] = { room_id, room_name, phone_detected, timestamp, received_at: Date.now() };

  // Add to events log
  events.push({
    type: 'phone',
    room_id, room_name,
    count: 0,
    time: new Date().toTimeString().slice(0,5),
    received_at: Date.now(),
  });
  if (events.length > 200) events.shift();

  console.log(`[PHONE] Detected in ${room_name}`);
  res.json({ ok: true });
});

// ─── GET /api/phones ─────────────────────────────────────────────────
app.get('/api/phones', (req, res) => {
  res.json({ phones, total: Object.values(phones).filter(p => p.phone_detected).length });
});

// ─── POST /api/reset ─────────────────────────────────────────────────
app.post('/api/reset', (req, res) => {
  // Clear all room counts
  Object.keys(rooms).forEach(k => {
    rooms[k].current_count = 0;
    rooms[k].total_entries = 0;
    rooms[k].total_exits   = 0;
    rooms[k].occupancy_pct = 0;
  });
  // Clear phone detections
  Object.keys(phones).forEach(k => { phones[k].phone_detected = false; });
  // Clear events
  events.length = 0;
  // Set reset flag — clients detect this and reset themselves
  resetFlag = Date.now();
  console.log('[RESET] Full system reset at', new Date().toISOString());
  res.json({ ok: true, reset_flag: resetFlag });
});

// ─── GET /health ──────────────────────────────────────────────────────
app.get('/health', (req, res) => res.json({ status: 'ok', uptime: process.uptime(), reset_flag: resetFlag }));

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => console.log(`Museum IoT Server running on port ${PORT}`));
