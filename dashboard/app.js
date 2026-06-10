/* Predator RF CoC Dashboard — app.js
 * Vanilla JS, no build step. Targets any modern browser on Pi/Linux.
 */

'use strict';

/* ── Config ── */
const API = '';           // same origin
const FLEET_POLL_MS   = 10_000;
const APPROVALS_POLL_MS = 5_000;
const RNS_POLL_MS     = 8_000;
const STATUS_POLL_MS  = 15_000;

/* ── State ── */
const tracks = new Map();   // emitter_id → track dict
let   eventsReceived = 0;
let   lastEventTime  = null;
let   sseConnected   = false;
let   sseSource      = null;

/* ── Helpers ── */
function fmtMHz(hz) {
  if (hz == null) return '—';
  return (hz / 1e6).toFixed(4);
}
function fmtAge(ns) {
  if (ns == null) return '—';
  const ms = (Date.now() - ns / 1e6);
  if (ms < 2000)   return '<2s';
  if (ms < 60000)  return Math.round(ms / 1000) + 's';
  if (ms < 3600000) return Math.round(ms / 60000) + 'm';
  return Math.round(ms / 3600000) + 'h';
}
function fmtTs(ns) {
  if (!ns) return '—';
  return new Date(ns / 1e6).toISOString().replace('T', ' ').slice(0, 19) + 'Z';
}
function fmtBytes(n) {
  if (!n) return '0';
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1048576).toFixed(1) + ' MB';
}
function esc(s) {
  // Escapes ALL four HTML-sensitive chars plus both quote chars so the
  // result is safe in both element-content AND attribute-value context
  // (regardless of whether the attribute is single- or double-quoted).
  return String(s ?? '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}
function stateClass(s) {
  const m = { new: 'state-new', tracking: 'state-tracking', stable: 'state-stable',
               coasting: 'state-coasting', lost: 'state-lost' };
  return m[s] || 'state-new';
}
function threatClass(t) {
  const m = { unknown: 'threat-unknown', low: 'threat-low', medium: 'threat-medium',
               high: 'threat-high', critical: 'threat-critical' };
  return m[t] || 'threat-unknown';
}
function setPillClass(el, cls) {
  el.className = 'pill ' + (cls || '');
}

/* ── Clock ── */
function tickClock() {
  document.getElementById('clock').textContent =
    new Date().toISOString().replace('T', ' ').slice(0, 19) + ' UTC';
}
tickClock();
setInterval(tickClock, 1000);

// No tab navigation — all panels are always visible in the responsive grid.

/* ── Modal ── */
const modal     = document.getElementById('track-modal');
const modalClose = document.getElementById('modal-close');
modalClose.addEventListener('click', () => { modal.hidden = true; });
modal.addEventListener('click', e => { if (e.target === modal) modal.hidden = true; });
document.addEventListener('keydown', e => { if (e.key === 'Escape') modal.hidden = true; });

/* ── Status bar polling ── */
async function pollStatus() {
  try {
    const r = await fetch(`${API}/api/v1/status`);
    if (!r.ok) throw new Error(r.status);
    const d = await r.json();
    const el = document.getElementById('sb-backend');
    el.textContent = 'OK';
    el.className = 'sb-v ok';
  } catch {
    const el = document.getElementById('sb-backend');
    el.textContent = 'UNREACHABLE';
    el.className = 'sb-v err';
  }
}
pollStatus();
setInterval(pollStatus, STATUS_POLL_MS);

/* ══════════════════════════════════════════════
   Fleet panel
══════════════════════════════════════════════ */
async function pollFleet() {
  try {
    const r = await fetch(`${API}/api/v1/nodes/`);
    if (!r.ok) throw new Error(r.status);
    const nodes = await r.json();
    renderFleet(nodes);
    document.getElementById('fleet-age').textContent = 'Updated ' + new Date().toISOString().slice(11, 19) + 'Z';
    document.getElementById('pill-nodes').textContent = nodes.length + ' NODE' + (nodes.length !== 1 ? 'S' : '');
    setPillClass(document.getElementById('pill-nodes'), nodes.length ? 'live' : '');
    document.getElementById('fleet-subtitle').textContent =
      nodes.length + ' node' + (nodes.length !== 1 ? 's' : '') + ' registered';
  } catch (err) {
    document.getElementById('fleet-subtitle').textContent = 'Poll error: ' + err.message;
  }
}

function gpsQualityLabel(n) {
  if (!n.gps_synchronized) return { cls: 'gps-none', label: '✗ No GPS' };
  if (n.timing_pps_lock)   return { cls: 'gps-ok',   label: '✔ GPS+PPS' };
  if (n.timing_source)     return { cls: 'gps-ok',   label: '✔ ' + esc(n.timing_source.toUpperCase()) };
  return { cls: 'gps-ok', label: '✔ GPS' };
}

function renderFleet(nodes) {
  const tbody = document.getElementById('fleet-tbody');
  if (!nodes.length) {
    tbody.innerHTML = '<tr class="empty-row"><td colspan="8">No nodes registered</td></tr>';
    return;
  }
  tbody.innerHTML = nodes.map(n => {
    const hostname = esc((n.kujhad_host || '—') + ':' + (n.kujhad_port || 5259));
    const nodeId   = esc(n.node_id || '—');
    const hw       = esc(n.hardware_code || '—');
    const gps      = gpsQualityLabel(n);
    const trustPct = Math.round((n.trust_score ?? 0) * 100);
    const lastContact = n.last_contact_ns ? fmtAge(n.last_contact_ns) : '—';
    const tdoaLabel = n.can_do_tdoa
      ? '<span style="color:var(--accent)">✔</span>'
      : '<span class="dim">—</span>';
    return `<tr class="clickable" data-node="${esc(n.node_id)}">
      <td style="color:var(--cyan)">${hostname}</td>
      <td><span title="${nodeId}" style="font-size:10px;color:var(--text-dim)">${nodeId.slice(0, 12)}${nodeId.length > 12 ? '…' : ''}</span></td>
      <td>${hw}</td>
      <td><span class="gps-badge ${gps.cls}">${gps.label}</span></td>
      <td>
        <div class="trust-bar">
          <div class="bar-track"><div class="bar-fill" style="width:${trustPct}%"></div></div>
          <span style="color:var(--text-dim);font-size:10px">${trustPct}%</span>
        </div>
      </td>
      <td class="dim" style="font-size:11px">${lastContact}</td>
      <td>${tdoaLabel}</td>
      <td>
        <button class="btn-tune" data-tune-node="${esc(n.node_id)}">Tune…</button>
      </td>
    </tr>`;
  }).join('');
}


pollFleet();
setInterval(pollFleet, FLEET_POLL_MS);

/* ══════════════════════════════════════════════
   Tracks panel — SSE-driven
══════════════════════════════════════════════ */
function connectSSE() {
  if (sseSource) { sseSource.close(); sseSource = null; }
  setPillClass(document.getElementById('pill-sse'), '');
  document.getElementById('pill-sse').textContent = 'SSE…';

  sseSource = new EventSource(`${API}/api/v1/events/stream`);

  sseSource.onopen = () => {
    sseConnected = true;
    document.getElementById('pill-sse').textContent = 'SSE';
    setPillClass(document.getElementById('pill-sse'), 'live');
  };

  sseSource.onerror = () => {
    sseConnected = false;
    document.getElementById('pill-sse').textContent = 'SSE ✗';
    setPillClass(document.getElementById('pill-sse'), 'warn');
    // EventSource auto-reconnects; we just track the visual state
  };

  sseSource.onmessage = ev => {
    try {
      const event = JSON.parse(ev.data);
      handleRFEvent(event);
    } catch { /* ignore parse errors */ }
  };
}

function handleRFEvent(event) {
  eventsReceived++;
  lastEventTime = Date.now();
  document.getElementById('sb-events').textContent = eventsReceived;
  document.getElementById('sb-last-event').textContent =
    new Date().toISOString().slice(11, 19) + 'Z';

  // Build/update a lightweight track entry from the raw event.
  // The SSE stream publishes RFEvent dicts (not track dicts), so we
  // synthesise a minimal track record per emitter to drive the table.
  // A full GET /api/v1/tracks/ will overwrite with server-side track data.
  const eid = event.node_id + ':' + Math.round((event.frequency || 0) / 25000) * 25000;
  const existing = tracks.get(eid) || {};
  tracks.set(eid, Object.assign(existing, {
    _eid: eid,
    primary_frequency: event.frequency,
    last_power_dbfs:   event.power_dbfs,
    last_seen_ns:      event.timestamp_ns,
    state:             existing.state || 'new',
    confidence:        existing.confidence || 0.1,
    threat_level:      existing.threat_level || 'unknown',
    detecting_nodes:   [event.node_id],
    modulation:        event.modulation,
    estimated_lat:     existing.estimated_lat,
    estimated_lon:     existing.estimated_lon,
    observation_count: (existing.observation_count || 0) + 1,
    _from_sse:         true,
  }));

  // Periodically refresh from the authoritative tracks API
  scheduleTrackRefresh();
  renderTracks();

  // Forward location to map iframe
  postMapUpdate(event);
}

// Debounced track refresh: at most once per 2s
let _trackRefreshTimer = null;
function scheduleTrackRefresh() {
  if (_trackRefreshTimer) return;
  _trackRefreshTimer = setTimeout(() => {
    _trackRefreshTimer = null;
    refreshTracksFromAPI();
  }, 2000);
}

async function refreshTracksFromAPI() {
  try {
    const minConf = parseFloat(document.getElementById('filter-conf').value) || 0;
    const state   = document.getElementById('filter-state').value;
    let url = `${API}/api/v1/tracks/?min_confidence=${minConf}&limit=200`;
    if (state) url += `&state=${state}`;
    const r = await fetch(url);
    if (!r.ok) return;
    const list = await r.json();
    // Merge API track data into our map
    for (const t of list) {
      const key = t.emitter_id || t._eid;
      tracks.set(key, Object.assign(tracks.get(key) || {}, t, { _eid: key }));
    }
    // Remove tracks no longer returned by the API (purged by server)
    // We keep SSE-only ephemeral ones until a timeout, don't purge them here.
    document.getElementById('tracks-age').textContent =
      'Updated ' + new Date().toISOString().slice(11, 19) + 'Z';
    renderTracks();
    // Push full track list to map iframe for accurate emitter marker layer.
    // Use the readiness-aware helper so updates are queued if the map
    // iframe hasn't fired predator-map-ready yet.
    postBulkTracksToMap(list);
    document.getElementById('pill-tracks').textContent =
      list.length + ' TRACK' + (list.length !== 1 ? 'S' : '');
    setPillClass(document.getElementById('pill-tracks'), list.length ? 'live' : '');
  } catch { /* silent */ }
}

function renderTracks() {
  const tbody = document.getElementById('tracks-tbody');
  const minConf = parseFloat(document.getElementById('filter-conf').value) || 0;
  const filterState = document.getElementById('filter-state').value;

  let list = Array.from(tracks.values())
    .filter(t => (t.confidence || 0) >= minConf)
    .filter(t => !filterState || t.state === filterState)
    .sort((a, b) => (b.confidence || 0) - (a.confidence || 0));

  if (!list.length) {
    tbody.innerHTML = '<tr class="empty-row"><td colspan="8">No tracks match filter</td></tr>';
    return;
  }

  tbody.innerHTML = list.map(t => {
    const key = esc(t.emitter_id || t._eid || '');
    const loc = (t.estimated_lat != null && t.estimated_lon != null)
      ? `${t.estimated_lat.toFixed(4)}, ${t.estimated_lon.toFixed(4)}`
      : '<span class="dim">—</span>';
    const nodes = (t.detecting_nodes || []).map(n => esc(n)).join(', ') || '<span class="dim">—</span>';
    const confPct = Math.round((t.confidence || 0) * 100);
    const confColor = confPct >= 70 ? 'var(--accent)' : confPct >= 40 ? 'var(--warn)' : 'var(--text-dim)';
    return `<tr class="clickable" data-key="${key}">
      <td>
        <span class="state-dot ${stateClass(t.state)}"></span>
        <span style="font-size:10px;text-transform:uppercase;letter-spacing:.08em">${esc(t.state || 'new')}</span>
      </td>
      <td><span class="freq-val">${fmtMHz(t.primary_frequency)}</span><span class="mhz-unit"> MHz</span></td>
      <td style="color:var(--text-dim)">${(t.last_power_dbfs ?? '—') !== '—' ? (t.last_power_dbfs).toFixed(1) + ' dBFS' : '—'}</td>
      <td><span style="color:${confColor};font-weight:600">${confPct}%</span></td>
      <td><span class="threat ${threatClass(t.threat_level)}">${esc(t.threat_level || 'unknown')}</span></td>
      <td style="font-size:11px">${loc}</td>
      <td style="font-size:11px;color:var(--text-dim)">${nodes}</td>
      <td style="font-size:11px;color:var(--text-faint)">${t.observation_count ?? '—'}</td>
    </tr>`;
  }).join('');
}

// Filter controls
document.getElementById('filter-state').addEventListener('change', () => { renderTracks(); refreshTracksFromAPI(); });
document.getElementById('filter-conf').addEventListener('input', renderTracks);

function showTrackDetail(key) {
  const t = tracks.get(key);
  if (!t) return;
  document.getElementById('modal-title').textContent =
    'Track: ' + (t.primary_frequency ? fmtMHz(t.primary_frequency) + ' MHz' : key.slice(0, 16));

  const body = document.getElementById('modal-body');
  const anomalies = (t.anomaly_flags || []).length
    ? `<div class="anomaly-list">${(t.anomaly_flags || []).map(f => `<span class="anomaly-badge">${esc(f)}</span>`).join('')}</div>`
    : '<span class="dim">None</span>';

  const ellipse = (t.tdoa_ellipse_a_m != null)
    ? `${t.tdoa_ellipse_a_m.toFixed(0)} m × ${(t.tdoa_ellipse_b_m || 0).toFixed(0)} m @ ${(t.tdoa_ellipse_theta_deg || 0).toFixed(1)}°`
    : '—';

  body.innerHTML = `
    <div class="modal-section">
      <div class="modal-section-title">Signal</div>
      <div class="modal-kv kv-row"><span class="kv-k">Frequency</span><span class="kv-v freq-val">${fmtMHz(t.primary_frequency)} MHz</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">Power</span><span class="kv-v">${t.last_power_dbfs != null ? t.last_power_dbfs.toFixed(2) + ' dBFS' : '—'}</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">Modulation</span><span class="kv-v">${esc(t.modulation || '—')}</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">Protocol</span><span class="kv-v">${esc(t.protocol || '—')}</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">Observations</span><span class="kv-v">${t.observation_count ?? '—'}</span></div>
    </div>

    <div class="modal-section">
      <div class="modal-section-title">Track</div>
      <div class="modal-kv kv-row"><span class="kv-k">State</span><span class="kv-v"><span class="state-dot ${stateClass(t.state)}"></span>${esc(t.state || '—')}</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">Confidence</span><span class="kv-v">${Math.round((t.confidence || 0) * 100)}%</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">Threat</span><span class="kv-v"><span class="threat ${threatClass(t.threat_level)}">${esc(t.threat_level || 'unknown')}</span></span></div>
      <div class="modal-kv kv-row"><span class="kv-k">Motion state</span><span class="kv-v">${esc(t.motion_state || '—')}</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">First seen</span><span class="kv-v dim" style="font-size:10px">${fmtTs(t.first_seen_ns)}</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">Last seen</span><span class="kv-v dim" style="font-size:10px">${fmtTs(t.last_seen_ns)}</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">Detecting nodes</span><span class="kv-v" style="font-size:11px">${(t.detecting_nodes || []).map(n => esc(n)).join(', ') || '—'}</span></div>
    </div>

    <div class="modal-section">
      <div class="modal-section-title">Location</div>
      <div class="modal-kv kv-row"><span class="kv-k">Lat / Lon</span><span class="kv-v">${t.estimated_lat != null ? t.estimated_lat.toFixed(6) + ', ' + (t.estimated_lon || 0).toFixed(6) : '—'}</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">Method</span><span class="kv-v">${esc(t.location_method || '—')}</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">Confidence</span><span class="kv-v">${t.location_confidence != null ? Math.round(t.location_confidence * 100) + '%' : '—'}</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">Error radius</span><span class="kv-v">${t.location_error_radius_m != null ? Math.round(t.location_error_radius_m) + ' m' : '—'}</span></div>
      <div class="modal-kv kv-row"><span class="kv-k">TDOA ellipse</span><span class="kv-v">${ellipse}</span></div>
    </div>

    <div class="modal-section">
      <div class="modal-section-title">Anomaly flags</div>
      ${anomalies}
    </div>

    <div class="modal-section">
      <div class="modal-section-title">Frequency history (last 50)</div>
      <div class="sparkline-wrap">
        <canvas class="sparkline" id="sparkline-canvas" width="600" height="44"></canvas>
      </div>
    </div>`;

  modal.hidden = false;
  drawSparkline(t);
}
window.showTrackDetail = showTrackDetail;

function drawSparkline(t) {
  const canvas = document.getElementById('sparkline-canvas');
  if (!canvas) return;
  const history = t.frequency_history || [];
  if (history.length < 2) {
    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    return;
  }
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  ctx.clearRect(0, 0, W, H);

  const min = Math.min(...history);
  const max = Math.max(...history);
  const range = max - min || 1;

  ctx.strokeStyle = '#3fd17d';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  history.forEach((v, i) => {
    const x = (i / (history.length - 1)) * W;
    const y = H - ((v - min) / range) * (H - 6) - 3;
    i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
  });
  ctx.stroke();

  // Axis labels
  ctx.fillStyle = '#3a4f5c';
  ctx.font = '9px monospace';
  ctx.fillText(fmtMHz(max) + ' MHz', 2, 11);
  ctx.fillText(fmtMHz(min) + ' MHz', 2, H - 3);
}

/* Initial track load */
refreshTracksFromAPI();
connectSSE();

/* ══════════════════════════════════════════════
   Approvals panel
══════════════════════════════════════════════ */
async function pollApprovals() {
  try {
    const r = await fetch(`${API}/api/v1/approvals`);
    if (!r.ok) throw new Error(r.status);
    const list = await r.json();
    renderApprovals(list);
    document.getElementById('approvals-age').textContent =
      'Updated ' + new Date().toISOString().slice(11, 19) + 'Z';

    const pendingCount = list.filter(a => a.state === 'pending').length;
    document.getElementById('pill-approvals').textContent =
      pendingCount + ' PENDING';
    setPillClass(document.getElementById('pill-approvals'), pendingCount ? 'warn' : '');

    // Flash the approvals panel header when there are pending items
    const apprHead = document.querySelector('#panel-approvals .panel-head h2');
    if (apprHead) {
      apprHead.style.color = pendingCount > 0 ? 'var(--warn)' : '';
    }

    document.getElementById('approvals-subtitle').textContent =
      pendingCount + ' pending';
  } catch (err) {
    document.getElementById('approvals-subtitle').textContent = 'Poll error: ' + err.message;
  }
}

function renderApprovals(list) {
  const pending = list.filter(a => a.state === 'pending');
  const body = document.getElementById('approvals-body');
  if (!pending.length) {
    body.innerHTML = '<div class="empty-card">No pending approvals</div>';
    return;
  }
  body.innerHTML = pending.map(a => {
    const freq = fmtMHz(a.primary_frequency);
    const threat = `<span class="threat ${threatClass(a.threat_level)}">${esc(a.threat_level)}</span>`;
    const loc = (a.estimated_lat != null)
      ? `${a.estimated_lat.toFixed(4)}, ${a.estimated_lon.toFixed(4)}`
      : (a.fallback_location ? a.fallback_location.map(v => v.toFixed(4)).join(', ') + ' (node)' : '—');
    return `<div class="approval-card" data-approval-id="${esc(a.approval_id)}">
      <div class="appr-detail">
        <div class="appr-freq">${freq} MHz</div>
        <div class="appr-meta">Threat ${threat} &nbsp;·&nbsp; ${esc(a.recommended_action || '—')}</div>
        <div class="appr-meta">Location: ${loc}</div>
        <div class="appr-meta">Emitter: <span class="mono">${esc((a.emitter_id || '').slice(0, 16))}…</span></div>
        <div class="appr-id">ID: ${esc(a.approval_id)}</div>
      </div>
      <div class="appr-actions">
        <button class="btn-approve" data-action="approve">Approve</button>
        <button class="btn-reject"  data-action="reject">Reject</button>
      </div>
    </div>`;
  }).join('');
}

async function decideApproval(id, action, triggerEl) {
  // Find card by data attribute — avoids the id="appr-{untrusted}" pattern.
  const body = document.getElementById('approvals-body');
  const card = body
    ? body.querySelector(`.approval-card[data-approval-id="${CSS.escape(id)}"]`)
    : null;
  if (card) card.querySelectorAll('button').forEach(b => { b.disabled = true; });
  try {
    const r = await fetch(`${API}/api/v1/approvals/${encodeURIComponent(id)}/${action}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ operator: 'dashboard' }),
    });
    if (!r.ok) throw new Error(r.status);
    if (action === 'approve' && card) {
      card.classList.add('approved');
      setTimeout(() => { card.remove(); pollApprovals(); }, 900);
    } else {
      if (card) card.remove();
      pollApprovals();
    }
  } catch (err) {
    if (card) card.querySelectorAll('button').forEach(b => { b.disabled = false; });
    alert('Action failed: ' + err.message);
  }
}

/* ── Delegated event listeners ─────────────────────────────────────────
 * Inline onclick handlers that interpolate API data are unsafe (XSS);
 * use event delegation on stable container elements instead.
 * Wired up once at DOMContentLoaded.
 */
document.addEventListener('DOMContentLoaded', () => {
  // Fleet table: Tune… button
  const fleetTbody = document.getElementById('fleet-tbody');
  if (fleetTbody) {
    fleetTbody.addEventListener('click', async ev => {
      const btn = ev.target.closest('[data-tune-node]');
      if (!btn) return;
      ev.stopPropagation();
      const nodeId = btn.dataset.tuneNode;
      const freqStr = window.prompt(`Tune node ${nodeId} to frequency (Hz):`);
      if (!freqStr) return;
      const hz = parseFloat(freqStr);
      if (!isFinite(hz) || hz <= 0) { alert('Invalid frequency'); return; }
      try {
        const r = await fetch(
          `${API}/api/v1/nodes/${encodeURIComponent(nodeId)}/tune?frequency_hz=${hz}`,
          { method: 'POST' });
        const d = await r.json();
        if (d.ok) { alert(`Tune command sent to ${nodeId}.`); }
        else      { alert(`Tune returned ok=false from ${nodeId}.`); }
      } catch (err) { alert('Tune failed: ' + err.message); }
    });
  }

  // Tracks table: row click → detail modal
  const tracksTbody = document.getElementById('tracks-tbody');
  if (tracksTbody) {
    tracksTbody.addEventListener('click', ev => {
      const row = ev.target.closest('tr[data-key]');
      if (!row) return;
      showTrackDetail(row.dataset.key);
    });
  }

  // Approvals: approve / reject buttons
  const approvalsBody = document.getElementById('approvals-body');
  if (approvalsBody) {
    approvalsBody.addEventListener('click', ev => {
      const btn = ev.target.closest('[data-action]');
      if (!btn) return;
      const card = btn.closest('[data-approval-id]');
      if (!card) return;
      const id     = card.dataset.approvalId;
      const action = btn.dataset.action;
      decideApproval(id, action, btn);
    });
  }
});

pollApprovals();
setInterval(pollApprovals, APPROVALS_POLL_MS);

/* ══════════════════════════════════════════════
   Map panel — postMessage bridge
══════════════════════════════════════════════ */

// Map iframe is always in the DOM (always-on grid, not lazy-loaded).
// We queue outbound messages until the iframe signals predator-map-ready.
let mapReady = false;
const mapQueue = [];   // pending messages; at most one predator-tracks-bulk kept

function _mapPost(msg) {
  const iframe = document.getElementById('map-iframe');
  if (mapReady && iframe?.contentWindow) {
    iframe.contentWindow.postMessage(msg, location.origin);
  } else {
    if (msg.type === 'predator-tracks-bulk') {
      // Only keep the latest bulk snapshot — older ones are stale.
      const idx = mapQueue.findIndex(m => m.type === 'predator-tracks-bulk');
      if (idx >= 0) mapQueue[idx] = msg;
      else          mapQueue.push(msg);
    } else {
      mapQueue.push(msg);
    }
  }
}

function postBulkTracksToMap(list) {
  _mapPost({ type: 'predator-tracks-bulk', tracks: list });
}

function postMapUpdate(event) {
  if (!event.node_lat && !event.node_lon) return;
  _mapPost({
    type:      'predator-track-update',
    node_id:   event.node_id,
    frequency: event.frequency,
    lat:       event.node_lat,
    lon:       event.node_lon,
    power:     event.power_dbfs,
  });
}

window.addEventListener('message', ev => {
  // Origin check: only accept messages from our own origin (the map iframe
  // is same-origin at /maps/index.html so this rejects any cross-origin frames).
  if (ev.origin !== location.origin) return;
  if (ev.data?.type !== 'predator-map-ready') return;

  mapReady = true;
  const iframe = document.getElementById('map-iframe');
  const cw = iframe?.contentWindow;
  if (!cw) return;

  // Flush queued messages
  mapQueue.forEach(msg => cw.postMessage(msg, location.origin));
  mapQueue.length = 0;

  // Immediately send current authoritative track set so markers appear
  // even if the first bulk poll happened before the map was ready.
  if (tracks.size) {
    cw.postMessage(
      { type: 'predator-tracks-bulk', tracks: [...tracks.values()] },
      location.origin);
  }
});

/* ══════════════════════════════════════════════
   RNS status panel
══════════════════════════════════════════════ */
async function pollRNS() {
  try {
    const r = await fetch(`${API}/api/v1/rns/status`);
    if (!r.ok) throw new Error(r.status);
    const d = await r.json();
    renderRNS(d);
    document.getElementById('rns-age').textContent =
      'Updated ' + new Date().toISOString().slice(11, 19) + 'Z';
  } catch (err) {
    document.getElementById('rns-daemon').textContent = 'Error: ' + err.message;
    document.getElementById('rns-daemon').className = 'kv-v warn';
  }
}

function fmtMsAge(ms) {
  if (!ms) return '—';
  const age = Date.now() - ms;
  if (age < 2000)    return '<2s';
  if (age < 60000)   return Math.round(age / 1000) + 's ago';
  if (age < 3600000) return Math.round(age / 60000) + 'm ago';
  return Math.round(age / 3600000) + 'h ago';
}

function renderRNS(d) {
  const daemonEl = document.getElementById('rns-daemon');
  daemonEl.textContent = d.daemon || '—';
  daemonEl.className = 'kv-v ' + (d.daemon === 'running' ? 'ok' : 'warn');

  document.getElementById('rns-hash').textContent = d.identity_hash || '—';
  document.getElementById('rns-lib').textContent =
    d.rns_available ? 'Available' : 'Not installed';
  document.getElementById('rns-lib').className = 'kv-v ' + (d.rns_available ? 'ok' : 'dim');

  const cot = d.cot_bridge || {};
  document.getElementById('rns-cot-pub').textContent = cot.published ?? '—';
  document.getElementById('rns-cot-rx').textContent  = cot.received  ?? '—';

  // Interfaces
  const ifaces = d.interfaces || [];
  const ifaceList = document.getElementById('rns-ifaces-list');
  if (!ifaces.length) {
    ifaceList.innerHTML = '<span class="dim">No interfaces configured</span>';
  } else {
    ifaceList.innerHTML = ifaces.map(iface => {
      const upCls = iface.up ? 'iface-up' : 'iface-down';
      const peers = iface.peers ? `<span class="iface-peers">${iface.peers}p</span>` : '<span class="dim">0p</span>';
      const ifac = iface.ifac_active
        ? `<span class="ifac-badge" title="IFAC net: ${esc(iface.ifac_netname)}">IFAC</span>`
        : '';
      return `<div class="iface-row">
        <span class="iface-indicator ${upCls}" title="${iface.up ? 'Up' : 'Down'}"></span>
        <span class="iface-name" title="${esc(iface.name)}">${esc(iface.name)}</span>
        <span class="iface-type">${esc(iface.type || '—')}</span>
        ${peers}
        ${ifac}
      </div>`;
    }).join('');
  }

  // Peer table
  const peers = d.peers || [];
  const peerList = document.getElementById('rns-peers-list');
  if (!peers.length) {
    peerList.innerHTML = '<span class="dim">No peers seen yet — peers appear when remote Reticulum nodes announce themselves</span>';
    return;
  }
  // Build an iface name lookup from interfaces list
  const ifaceNames = {};
  for (const iface of ifaces) {
    if (iface.id) ifaceNames[iface.id] = iface.name;
  }
  peerList.innerHTML = `
    <div class="iface-row" style="color:var(--text-faint);font-size:10px;font-weight:700;letter-spacing:.1em">
      <span></span>
      <span style="grid-column:2/4">Hash-16</span>
      <span>Interface</span>
      <span>Last Heard</span>
    </div>` +
    peers.map(p => {
      const ifaceName = (p.iface_id && ifaceNames[p.iface_id])
        ? esc(ifaceNames[p.iface_id])
        : (p.iface_id ? esc(p.iface_id.slice(0, 8)) : '<span class="dim">—</span>');
      return `<div class="iface-row">
        <span class="iface-indicator iface-up" title="Known peer"></span>
        <span class="iface-name mono" style="grid-column:2/4" title="${esc(p.hash16)}">${esc(p.hash16)}</span>
        <span class="iface-type">${ifaceName}</span>
        <span class="dim" style="font-size:10px">${fmtMsAge(p.last_heard_ms)}</span>
      </div>`;
    }).join('');
}

pollRNS();
setInterval(pollRNS, RNS_POLL_MS);
