// Vanilla JS, no build step — matches this project's "no web framework"
// convention (see requirements.txt). Auth is HTTP Basic; the browser's
// native prompt handles the 401/WWW-Authenticate challenge and caches
// credentials for the rest of the session, so every fetch() below is
// unauthenticated-looking on purpose — the browser adds the header.

const API = "/api/admin/v1";

const errorBanner = document.getElementById("error-banner");

function showError(message) {
  errorBanner.textContent = message;
  errorBanner.hidden = false;
}

function clearError() {
  errorBanner.hidden = true;
}

async function api(path, options = {}) {
  const resp = await fetch(`${API}${path}`, {
    headers: { "Content-Type": "application/json" },
    ...options,
  });
  if (!resp.ok) {
    let detail = resp.statusText;
    try {
      const body = await resp.json();
      detail = body.error || detail;
    } catch (_) {
      // ignore non-JSON error bodies
    }
    throw new Error(`${resp.status}: ${detail}`);
  }
  return resp.json();
}

function fmtBytes(n) {
  if (n == null) return "—";
  const units = ["B", "KB", "MB", "GB"];
  let i = 0;
  let v = n;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i += 1;
  }
  return `${v.toFixed(v < 10 && i > 0 ? 1 : 0)} ${units[i]}`;
}

function fmtTime(epochSeconds) {
  if (!epochSeconds) return "—";
  return new Date(epochSeconds * 1000).toLocaleString();
}

function el(tag, attrs = {}, children = []) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (key === "text") node.textContent = value;
    else if (key === "onclick") node.addEventListener("click", value);
    else node.setAttribute(key, value);
  }
  for (const child of children) node.appendChild(child);
  return node;
}

// -- tabs ---------------------------------------------------------------

const loaders = {
  dashboard: loadDashboard,
  jobs: loadJobs,
  devices: loadDevices,
  approvals: loadApprovals,
  settings: loadSettings,
};

document.getElementById("tabs").addEventListener("click", (evt) => {
  const btn = evt.target.closest(".tab-btn");
  if (!btn) return;
  activateTab(btn.dataset.tab);
});

function activateTab(name) {
  for (const btn of document.querySelectorAll(".tab-btn")) {
    btn.classList.toggle("active", btn.dataset.tab === name);
  }
  for (const panel of document.querySelectorAll(".tab-panel")) {
    panel.hidden = panel.id !== `tab-${name}`;
  }
  clearError();
  loaders[name]?.().catch((err) => showError(err.message));
}

// -- dashboard ---------------------------------------------------------------

async function loadDashboard() {
  const status = await api("/status");
  document.getElementById("stat-pending").textContent = status.jobs_pending;
  document.getElementById("stat-devices").textContent = status.device_count;
  document.getElementById("stat-unapplied").textContent = status.unapplied_approvals;
  document.getElementById("stat-printer").textContent = status.printer_ready ? "configured" : "not set";
  document.getElementById("stat-relay").textContent = !status.relay_enabled
    ? "disabled"
    : status.relay_running
      ? "running"
      : "enabled, not running";
}

// -- jobs ---------------------------------------------------------------

async function loadJobs() {
  const { jobs } = await api("/jobs");
  const tbody = document.querySelector("#jobs-table tbody");
  tbody.replaceChildren();
  if (jobs.length === 0) {
    tbody.appendChild(el("tr", {}, [el("td", { class: "empty", colspan: "8", text: "No jobs yet." })]));
    return;
  }
  for (const job of jobs) {
    tbody.appendChild(
      el("tr", {}, [
        el("td", { text: job.title }),
        el("td", { text: job.status }),
        el("td", { text: String(job.page_count) }),
        el("td", { text: fmtBytes(job.original_bytes) }),
        el("td", { text: String(job.delivered_count) }),
        el("td", { text: job.last_action || "—" }),
        el("td", { text: fmtTime(job.created_at) }),
        el("td", {}, [
          el("div", { class: "row-actions" }, [
            el("button", { class: "btn", text: "Reprint", onclick: () => runJobAction(job.job_id, "print") }),
            el("button", { class: "btn", text: "Requeue", onclick: () => runJobAction(job.job_id, "requeue") }),
            el("button", { class: "btn", text: "Archive", onclick: () => runJobAction(job.job_id, "delete") }),
            el("button", {
              class: "btn btn-danger",
              text: "Purge",
              onclick: () => {
                if (confirm(`Permanently delete "${job.title}" and its files?`)) runJobAction(job.job_id, "purge");
              },
            }),
          ]),
        ]),
      ]),
    );
  }
}

async function runJobAction(jobId, action) {
  try {
    await api(`/jobs/${encodeURIComponent(jobId)}/action`, {
      method: "POST",
      body: JSON.stringify({ action }),
    });
    await loadJobs();
  } catch (err) {
    showError(err.message);
  }
}

// -- devices ---------------------------------------------------------------

async function loadDevices() {
  const { devices } = await api("/devices");
  const tbody = document.querySelector("#devices-table tbody");
  tbody.replaceChildren();
  if (devices.length === 0) {
    tbody.appendChild(el("tr", {}, [el("td", { class: "empty", colspan: "5", text: "No paired devices yet." })]));
    return;
  }
  for (const device of devices) {
    tbody.appendChild(
      el("tr", {}, [
        el("td", { text: device.name }),
        el("td", {}, [el("code", { text: device.device_id })]),
        el("td", { text: fmtTime(device.paired_at) }),
        el("td", { text: fmtTime(device.last_seen_at) }),
        el("td", {}, [
          el("div", { class: "row-actions" }, [
            el("button", {
              class: "btn",
              text: "Rotate token",
              onclick: () => rotateToken(device.device_id, device.name),
            }),
            el("button", {
              class: "btn btn-danger",
              text: "Revoke",
              onclick: () => {
                if (confirm(`Revoke "${device.name}"? Its current token stops working immediately.`)) {
                  revokeDevice(device.device_id);
                }
              },
            }),
          ]),
        ]),
      ]),
    );
  }
}

async function revokeDevice(deviceId) {
  try {
    await api(`/devices/${encodeURIComponent(deviceId)}/revoke`, { method: "POST" });
    await loadDevices();
  } catch (err) {
    showError(err.message);
  }
}

async function rotateToken(deviceId, name) {
  try {
    const result = await api(`/devices/${encodeURIComponent(deviceId)}/rotate-token`, { method: "POST" });
    window.prompt(
      `New token for "${name}" — copy this into /system/device.json on its SD card now, it will not be shown again:`,
      result.device_token,
    );
    await loadDevices();
  } catch (err) {
    showError(err.message);
  }
}

// -- approvals ---------------------------------------------------------------

async function loadApprovals() {
  const { approvals } = await api("/approvals");
  const tbody = document.querySelector("#approvals-table tbody");
  tbody.replaceChildren();
  if (approvals.length === 0) {
    tbody.appendChild(el("tr", {}, [el("td", { class: "empty", colspan: "6", text: "No approvals yet." })]));
    return;
  }
  for (const a of approvals) {
    tbody.appendChild(
      el("tr", {}, [
        el("td", { text: a.job_title || a.job_id }),
        el("td", { text: a.action }),
        el("td", { text: a.received_via }),
        el("td", { text: a.applied ? "yes" : "no" }),
        el("td", { text: a.detail || a.error || "—" }),
        el("td", { text: fmtTime(a.received_at) }),
      ]),
    );
  }
}

// -- settings ---------------------------------------------------------------

const SETTINGS_FIELDS = [
  "cups_queue",
  "retention_days",
  "relay_url",
  "relay_account_id",
  "relay_account_token",
  "relay_poll_interval_seconds",
  "relay_allow_document_sync",
];

async function loadSettings() {
  const settings = await api("/settings");
  for (const field of SETTINGS_FIELDS) {
    const input = document.getElementById(`set-${field}`);
    if (input.type === "checkbox") input.checked = Boolean(settings[field]);
    else input.value = settings[field];
  }
  document.getElementById("settings-saved").hidden = true;
}

document.getElementById("settings-form").addEventListener("submit", async (evt) => {
  evt.preventDefault();
  const payload = {};
  for (const field of SETTINGS_FIELDS) {
    const input = document.getElementById(`set-${field}`);
    if (input.type === "checkbox") payload[field] = input.checked;
    else if (input.type === "number") payload[field] = Number(input.value);
    else payload[field] = input.value;
  }
  try {
    await api("/settings", { method: "POST", body: JSON.stringify(payload) });
    document.getElementById("settings-saved").hidden = false;
  } catch (err) {
    showError(err.message);
  }
});

// -- boot ---------------------------------------------------------------

activateTab("dashboard");
