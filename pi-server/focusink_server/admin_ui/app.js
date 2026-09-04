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
  "device-config": loadDeviceConfig,
  planner: loadPlanner,
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
    tbody.appendChild(el("tr", {}, [el("td", { class: "empty", colspan: "9", text: "No jobs yet." })]));
    return;
  }
  for (const job of jobs) {
    const thumbImg = el("img", {
      class: "job-thumb",
      loading: "lazy",
      alt: "",
      src: `${API}/jobs/${encodeURIComponent(job.job_id)}/thumbnail`,
    });
    // Jobs ingested before thumbnails existed have no thumbnail_path, so the
    // request 404s — just hide the image rather than showing a broken icon.
    thumbImg.onerror = () => thumbImg.remove();
    tbody.appendChild(
      el("tr", {}, [
        el("td", {}, [thumbImg]),
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

// -- calendars & Wi-Fi (synced to every device on its next wake) -----------

async function loadDeviceConfig() {
  await Promise.all([loadCalendars(), loadWifiNetworks()]);
}

async function loadCalendars() {
  const { calendars, max } = await api("/calendars");
  const tbody = document.querySelector("#calendars-table tbody");
  tbody.replaceChildren();
  if (calendars.length === 0) {
    tbody.appendChild(el("tr", {}, [el("td", { class: "empty", colspan: "3", text: "No calendars configured." })]));
  }
  for (const cal of calendars) {
    tbody.appendChild(
      el("tr", {}, [
        el("td", { text: cal.label || "—" }),
        el("td", {}, [el("code", { text: cal.url })]),
        el("td", {}, [
          el("button", {
            class: "btn btn-danger",
            text: "Remove",
            onclick: () => deleteCalendar(cal.id),
          }),
        ]),
      ]),
    );
  }
  document.getElementById("calendars-limit-hint").textContent = `${calendars.length} / ${max} calendars used.`;
}

document.getElementById("calendar-form").addEventListener("submit", async (evt) => {
  evt.preventDefault();
  const url = document.getElementById("cal-url").value.trim();
  const label = document.getElementById("cal-label").value.trim();
  try {
    await api("/calendars", { method: "POST", body: JSON.stringify({ url, label }) });
    evt.target.reset();
    await loadCalendars();
  } catch (err) {
    showError(err.message);
  }
});

async function deleteCalendar(id) {
  try {
    await api(`/calendars/${id}/delete`, { method: "POST" });
    await loadCalendars();
  } catch (err) {
    showError(err.message);
  }
}

async function loadWifiNetworks() {
  const { wifi_networks: networks, max } = await api("/wifi-networks");
  const tbody = document.querySelector("#wifi-table tbody");
  tbody.replaceChildren();
  if (networks.length === 0) {
    tbody.appendChild(el("tr", {}, [el("td", { class: "empty", colspan: "3", text: "No Wi-Fi networks configured." })]));
  }
  for (const net of networks) {
    tbody.appendChild(
      el("tr", {}, [
        el("td", { text: net.ssid }),
        el("td", { text: net.password ? "••••••••" : "—" }),
        el("td", {}, [
          el("button", {
            class: "btn btn-danger",
            text: "Remove",
            onclick: () => deleteWifiNetwork(net.id),
          }),
        ]),
      ]),
    );
  }
  document.getElementById("wifi-limit-hint").textContent = `${networks.length} / ${max} networks used.`;
}

document.getElementById("wifi-form").addEventListener("submit", async (evt) => {
  evt.preventDefault();
  const ssid = document.getElementById("wifi-ssid").value.trim();
  const password = document.getElementById("wifi-password").value;
  try {
    await api("/wifi-networks", { method: "POST", body: JSON.stringify({ ssid, password }) });
    evt.target.reset();
    await loadWifiNetworks();
  } catch (err) {
    showError(err.message);
  }
});

async function deleteWifiNetwork(id) {
  try {
    await api(`/wifi-networks/${id}/delete`, { method: "POST" });
    await loadWifiNetworks();
  } catch (err) {
    showError(err.message);
  }
}

// -- planner (per-device tasks + Pomodoro config) ---------------------------

// Fixed set, same order as firmware's store::Category and pi-server's
// planner.CATEGORIES -- see docs/planner.md. Hardcoded here (not fetched)
// since it's needed to populate the category <select> before any device/
// date is even chosen.
const PLANNER_CATEGORIES = ["Work", "Break", "Chore", "Health", "Social", "School", "Personal", "Other"];
const POMODORO_FIELDS = [
  "work_minutes",
  "break_minutes",
  "long_break_minutes",
  "sessions_before_long_break",
  "checkpoint_minutes",
];

function todayDateString() {
  const d = new Date();
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
}

async function loadPlanner() {
  const categorySelect = document.getElementById("task-category");
  if (categorySelect.options.length === 0) {
    for (const cat of PLANNER_CATEGORIES) {
      categorySelect.appendChild(el("option", { value: cat, text: cat }));
    }
  }

  const dateInput = document.getElementById("planner-date");
  if (!dateInput.value) dateInput.value = todayDateString();

  // Refetched every time this tab is activated (not cached) -- a device
  // paired after the Planner tab's first visit must show up without
  // requiring a full page reload.
  const deviceSelect = document.getElementById("planner-device");
  const previouslySelected = deviceSelect.value;
  const { devices } = await api("/devices");
  deviceSelect.replaceChildren();
  if (devices.length === 0) {
    deviceSelect.appendChild(el("option", { value: "", text: "No paired devices" }));
  }
  for (const device of devices) {
    deviceSelect.appendChild(el("option", { value: device.device_id, text: device.name || device.device_id }));
  }
  if (devices.some((d) => d.device_id === previouslySelected)) {
    deviceSelect.value = previouslySelected;
  }

  if (!deviceSelect.value) return;  // no device paired yet -- nothing to load
  await Promise.all([loadPlannerTasks(), loadPomodoroConfig()]);
}

function selectedPlannerDevice() {
  return document.getElementById("planner-device").value;
}

async function loadPlannerTasks() {
  const deviceId = selectedPlannerDevice();
  const date = document.getElementById("planner-date").value;
  if (!deviceId || !date) return;
  const { tasks } = await api(
    `/devices/${encodeURIComponent(deviceId)}/planner/tasks?date=${encodeURIComponent(date)}`,
  );
  const tbody = document.querySelector("#planner-tasks-table tbody");
  tbody.replaceChildren();
  if (tasks.length === 0) {
    tbody.appendChild(el("tr", {}, [el("td", { class: "empty", colspan: "6", text: "No tasks for this day." })]));
    return;
  }
  for (const task of tasks) {
    tbody.appendChild(
      el("tr", {}, [
        el("td", { text: task.title }),
        el("td", { text: task.category }),
        el("td", { text: task.start_time }),
        el("td", { text: task.end_time }),
        el("td", { text: task.done ? "yes" : "no" }),
        el("td", {}, [
          el("button", {
            class: "btn btn-danger",
            text: "Remove",
            onclick: () => deletePlannerTask(task.id),
          }),
        ]),
      ]),
    );
  }
}

document.getElementById("planner-device").addEventListener("change", () => {
  loadPlannerTasks().catch((err) => showError(err.message));
  loadPomodoroConfig().catch((err) => showError(err.message));
});
document.getElementById("planner-date").addEventListener("change", () => {
  loadPlannerTasks().catch((err) => showError(err.message));
});

document.getElementById("planner-task-form").addEventListener("submit", async (evt) => {
  evt.preventDefault();
  const deviceId = selectedPlannerDevice();
  if (!deviceId) {
    showError("No device selected.");
    return;
  }
  const payload = {
    date: document.getElementById("planner-date").value,
    title: document.getElementById("task-title").value.trim(),
    category: document.getElementById("task-category").value,
    start_time: document.getElementById("task-start").value,
    end_time: document.getElementById("task-end").value,
  };
  try {
    await api(`/devices/${encodeURIComponent(deviceId)}/planner/tasks`, {
      method: "POST",
      body: JSON.stringify(payload),
    });
    evt.target.reset();
    await loadPlannerTasks();
  } catch (err) {
    showError(err.message);
  }
});

async function deletePlannerTask(taskId) {
  const deviceId = selectedPlannerDevice();
  try {
    await api(`/devices/${encodeURIComponent(deviceId)}/planner/tasks/${taskId}/delete`, { method: "POST" });
    await loadPlannerTasks();
  } catch (err) {
    showError(err.message);
  }
}

async function loadPomodoroConfig() {
  const deviceId = selectedPlannerDevice();
  if (!deviceId) return;
  const config = await api(`/devices/${encodeURIComponent(deviceId)}/pomodoro/config`);
  for (const field of POMODORO_FIELDS) {
    document.getElementById(`pomo-${field}`).value = config[field];
  }
  document.getElementById("pomodoro-saved").hidden = true;
}

document.getElementById("pomodoro-form").addEventListener("submit", async (evt) => {
  evt.preventDefault();
  const deviceId = selectedPlannerDevice();
  if (!deviceId) {
    showError("No device selected.");
    return;
  }
  const payload = {};
  for (const field of POMODORO_FIELDS) {
    payload[field] = Number(document.getElementById(`pomo-${field}`).value);
  }
  try {
    await api(`/devices/${encodeURIComponent(deviceId)}/pomodoro/config`, {
      method: "POST",
      body: JSON.stringify(payload),
    });
    document.getElementById("pomodoro-saved").hidden = false;
  } catch (err) {
    showError(err.message);
  }
});

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
