/*
 ╔══════════════════════════════════════════════════════════════════╗
 ║   MUSEUM SERVER — Updated with Wi-Fi Probe Support               ║
 ║   Add this to your existing museum_server.js                     ║
 ╚══════════════════════════════════════════════════════════════════╝
*/

const express = require('express');
const cors    = require('cors');
const app     = express();

app.use(cors());
app.use(express.json());

// ─── Existing stores ──────────────────────────────────────────────────
const rooms   = {};
const history = {};
const alerts  = [];

// ─── NEW: Probe stores ────────────────────────────────────────────────
const probeData   = {};   // roomId → latest probe snapshot
const probeHistory= {};   // roomId → time-series array

// ─── EXISTING ROUTES (unchanged) ─────────────────────────────────────

app.post('/api/visitor', (req, res) => {
  const { room_id, room_name, current_count, total_entries, total_exits, capacity, timestamp } = req.body;
  if (!room_id) return res.status(400).json({ error: 'room_id required' });
  rooms[room_id] = {
    room_id, room_name, current_count, total_entries, total_exits, capacity, timestamp,
    last_seen: Date.now(),
    occupancy_pct: Math.round(current_count / capacity * 100),
  };
  if (!history[room_id]) history[room_id] = [];
  history[room_id].push({ timestamp, count: current_count });
  if (history[room_id].length > 17280) history[room_id].shift();
  console.log(`[VISITOR] Room: ${room_name} | Count: ${current_count}/${capacity}`);
  res.json({ ok: true });
});

app.post('/api/visitor/alert', (req, res) => {
  alerts.push({ ...req.body, received_at: Date.now() });
  res.json({ ok: true });
});

app.get('/api/rooms', (req, res) => {
  const result = Object.values(rooms).map(r => ({
    ...r,
    is_online: (Date.now() - r.last_seen) < 30000,
    probe: probeData[r.room_id] || null,  // attach probe data per room
  }));
  res.json({ rooms: result, total_visitors: result.reduce((s,r)=>s+r.current_count,0) });
});

app.get('/api/rooms/:roomId/history', (req, res) => {
  const data = history[req.params.roomId] || [];
  const hourly = {};
  data.forEach(({ timestamp, count }) => {
    const h = new Date(timestamp * 1000).getHours();
    if (!hourly[h]) hourly[h] = { hour: h, samples: [], max: 0 };
    hourly[h].samples.push(count);
    hourly[h].max = Math.max(hourly[h].max, count);
  });
  const result = Array.from({length:24},(_,h) => ({
    hour: h,
    avg: hourly[h] ? Math.round(hourly[h].samples.reduce((a,b)=>a+b,0)/hourly[h].samples.length) : 0,
    max: hourly[h]?.max || 0,
  }));
  res.json({ room_id: req.params.roomId, history: result });
});

app.get('/api/alerts', (req, res) => res.json({ alerts: alerts.slice(-50) }));

// ─── NEW: Wi-Fi Probe Routes ──────────────────────────────────────────

// POST /api/probe — ESP32 probe sniffer sends aggregated counts
app.post('/api/probe', (req, res) => {
  const { room_id, phones_in_range, unique_today, active_entries, timestamp } = req.body;
  if (!room_id) return res.status(400).json({ error: 'room_id required' });

  probeData[room_id] = {
    room_id,
    phones_in_range,
    unique_today,
    active_entries,
    timestamp,
    received_at: Date.now(),
  };

  // Append to probe history (for trend charts)
  if (!probeHistory[room_id]) probeHistory[room_id] = [];
  probeHistory[room_id].push({
    t: Date.now(),
    phones_in_range,
    unique_today,
  });
  // Keep last 2 hours at 10s resolution = 720 records
  if (probeHistory[room_id].length > 720) probeHistory[room_id].shift();

  console.log(`[PROBE] Room: ${room_id} | Phones nearby: ${phones_in_range} | Unique today: ${unique_today}`);
  res.json({ ok: true });
});

// GET /api/probe — dashboard fetches all rooms' probe snapshots
app.get('/api/probe', (req, res) => {
  const museum_total_phones = Object.values(probeData)
    .reduce((s, r) => s + (r.phones_in_range || 0), 0);
  const museum_unique_today = Math.max(
    ...Object.values(probeData).map(r => r.unique_today || 0), 0
  );
  res.json({
    rooms: probeData,
    museum_total_phones,
    museum_unique_today,
  });
});

// GET /api/probe/:roomId/history — time-series for charts
app.get('/api/probe/:roomId/history', (req, res) => {
  const data = probeHistory[req.params.roomId] || [];
  res.json({ room_id: req.params.roomId, history: data });
});

// GET /api/summary — combined visitor + probe summary
app.get('/api/summary', (req, res) => {
  const totalVisitors = Object.values(rooms).reduce((s,r) => s + r.current_count, 0);
  const totalPhones   = Object.values(probeData).reduce((s,r) => s + (r.phones_in_range||0), 0);

  // Phone-to-person ratio: phones / IR count gives device density
  const phoneRatio = totalVisitors > 0
    ? Math.round((totalPhones / totalVisitors) * 100) / 100
    : 0;

  res.json({
    total_visitors_ir:   totalVisitors,
    total_phones_nearby: totalPhones,
    phone_per_person:    phoneRatio,
    rooms: Object.keys(rooms).map(id => ({
      room_id:      id,
      ir_count:     rooms[id]?.current_count || 0,
      phone_count:  probeData[id]?.phones_in_range || 0,
      unique_today: probeData[id]?.unique_today || 0,
    })),
  });
});

app.get('/health', (req, res) => res.json({ status: 'ok', uptime: process.uptime() }));

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
  console.log(`Museum IoT Server (with Probe) running on http://localhost:${PORT}`);
});
