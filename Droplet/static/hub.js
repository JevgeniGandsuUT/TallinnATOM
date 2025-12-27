(() => {
  const tbody = document.getElementById("devices-body");
  const serverTimeEl = document.getElementById("server-time");
  const connStateEl = document.getElementById("conn-state");

  if (!tbody) return;

  function fmt3(v){
    if (v === null || v === undefined) return "-";
    const n = Number(v);
    if (Number.isNaN(n)) return "-";
    return n.toFixed(3);
  }

  function ensureRow(uid){
    let row = tbody.querySelector(`tr[data-uid="${CSS.escape(uid)}"]`);
    if (row) return row;

    row = document.createElement("tr");
    row.setAttribute("data-uid", uid);
    row.innerHTML = `
      <td class="mono" data-k="device_id"></td>
      <td data-k="status"></td>
      <td data-k="valve"></td>
      <td data-k="now"></td>
      <td class="mono" data-k="prev"></td>
      <td class="mono" data-k="delta"></td>
      <td class="mono" data-k="time"></td>
      <td data-k="view"></td>
    `;
    tbody.appendChild(row);
    return row;
  }

  function updateRow(d){
    const uid = d.device_id;
    const row = ensureRow(uid);

    row.querySelector('[data-k="device_id"]').textContent = uid;

    const statusCell = row.querySelector('[data-k="status"]');
    statusCell.innerHTML = `<span class="pill ${d.offline ? "offline" : "open"}">${d.offline ? "Offline" : "Online"}</span>`;

    const valveCell = row.querySelector('[data-k="valve"]');
    valveCell.innerHTML = `<span class="pill mono">${d.valve_state || "-"}</span>`;

    const nowCell = row.querySelector('[data-k="now"]');
    const alarm = (d.pressure_now !== null && d.pressure_now !== undefined && Number(d.pressure_now) > 5);
    nowCell.innerHTML = `<span class="pill mono ${alarm ? "alarm" : ""}">${fmt3(d.pressure_now)}</span>`;

    row.querySelector('[data-k="prev"]').textContent  = fmt3(d.pressure_prev);
    row.querySelector('[data-k="delta"]').textContent = fmt3(d.delta);
    row.querySelector('[data-k="time"]').textContent  = d.time_utc || "-";

    const viewCell = row.querySelector('[data-k="view"]');
    const hasView = !!d.has_view;
    viewCell.innerHTML = `<a class="view-link ${hasView ? "" : "disabled"}" href="/device/${encodeURIComponent(uid)}">${hasView ? "Open" : "No init"}</a>`;
  }

  function applySnapshot(snapshot){
    if (serverTimeEl && snapshot.server_time_utc){
      serverTimeEl.textContent = snapshot.server_time_utc;
    }
    const list = snapshot.devices || [];
    list.sort((a,b) => (a.device_id || "").localeCompare(b.device_id || ""));
    for (const d of list) updateRow(d);
  }

  const es = new EventSource("/events/devices");

  es.onopen = () => {
    if (connStateEl) connStateEl.textContent = "· connected";
  };

  es.onerror = () => {
    if (connStateEl) connStateEl.textContent = "· reconnecting...";
  };

  es.addEventListener("devices", (evt) => {
    try{
      const snapshot = JSON.parse(evt.data);
      applySnapshot(snapshot);
    }catch(e){}
  });

  es.addEventListener("error", (evt) => {
    try{
      const payload = JSON.parse(evt.data);
      console.warn("SSE error:", payload);
    }catch(e){}
  });
})();
