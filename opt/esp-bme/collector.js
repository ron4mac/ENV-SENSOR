/**
 * Sensor Collector Server
 * -----------------------
 * Receives JSON POSTs from the ESP32-C6, stores the last N readings
 * in memory, and serves a live dashboard on GET /
 *
 * Requirements: Node.js ≥ 18  (no npm packages needed — uses built-ins only)
 *
 * Run:
 *   node collector.js
 *
 * Endpoints:
 *   POST /api/reading   ← ESP32 posts here
 *   GET  /api/readings  ← returns last 500 readings as JSON array
 *   GET  /              ← live dashboard HTML
 */

const http = require('http');
const fs = require('fs');

const PORT         = 3000;
const MAX_READINGS = 500;   // keep last N readings in memory

const readings = [];   // newest at index 0

var rfuRes = null;

/* ── Helpers ── */
function timestamp() {
  return new Date().toISOString();
}

function respond(res, status, contentType, body) {
  res.writeHead(status, {
    'Content-Type': contentType,
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Headers': 'Content-Type, Authorization',
  });
  res.end(body);
}

/* ── Request handler ── */
const server = http.createServer((req, res) => {

  // CORS pre-flight
  if (req.method === 'OPTIONS') {
    res.writeHead(204, { 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Headers': '*' });
    return res.end();
  }

  /* POST /api/reading ─────────────────────────────────────────── */
  if (req.method === 'POST' && req.url === '/api/reading') {
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
      try {
        const data = JSON.parse(body);
        data.server_time = timestamp();
        readings.unshift(data);                         // newest first
        if (readings.length > MAX_READINGS) readings.pop();
        console.log(`[${data.server_time}] ← ${data.device}  T=${data.temperature_c}°C  H=${data.humidity_pct}%  P=${data.pressure_hpa}hPa  Gas=${data.gas_kohm}kΩ  IAQ=${data.iaq} (${data.iaq_label})  boot#${data.boot_count}`);
        respond(res, rfuRes ? 201 : 200, 'application/json', JSON.stringify({ ok: true, stored: readings.length }));
        if (rfuRes) {
        	const ip = req.socket.remoteAddress;
        	respond(rfuRes, 200, 'application/json', JSON.stringify({ IP: ip }));
        	rfuRes = null;
        }
      } catch (e) {
        console.error('[POST] Bad JSON:', e.message);
        respond(res, 400, 'application/json', JSON.stringify({ error: 'Invalid JSON' }));
      }
    });
    return;
  }

  /* GET /api/readings ─────────────────────────────────────────── */
  if (req.method === 'GET' && req.url === '/api/readings') {
    respond(res, 200, 'application/json', JSON.stringify(readings));
    return;
  }

  /* GET / — serve dashboard ───────────────────────────────────── */
  if (req.method === 'GET' && (req.url === '/' || req.url === '/index.html')) {
  	fs.readFile('dashboard.html', function(error, content) {
  		respond(res, 200, 'text/html; charset=utf-8', content);
  	});
    return;
  }

  /* GET /api/rfu ──────────────────────────────────────────────── */
  if (req.method === 'GET' && req.url === '/api/rfu') {
    rfuRes = res;
    //respond(res, 200, 'application/json', JSON.stringify(readings));
    return;
  }

  respond(res, 404, 'text/plain', 'Not found');
});

server.listen(PORT, () => {
  console.log(`\n╔══════════════════════════════════════════╗`);
  console.log(`║  Sensor Collector running                ║`);
  console.log(`║  POST  http://localhost:${PORT}/api/reading ║`);
  console.log(`║  Dashboard  http://localhost:${PORT}/       ║`);
  console.log(`╚══════════════════════════════════════════╝\n`);
});
