(() => {
  const root = document.querySelector(".wrap[data-uid]");
  if (!root) return;

  const uid = root.getAttribute("data-uid");
  const conn = document.getElementById("conn");

  function pickDevice(snapshot){
    const list = snapshot && snapshot.devices ? snapshot.devices : [];
    return list.find(d => d.device_id === uid);
  }

  function pushToFragment(d){
    if (!d) return;
    if (typeof window.handleSensorUpdate !== "function") return;

    window.handleSensorUpdate({
      uid: d.device_id,
      ts_ms: d.time_ms,
      valve_state: d.valve_state,
      pressure_prev: d.pressure_prev,
      pressure_now: d.pressure_now
    });
  }

  async function loadHistory(){
    try{
      const res = await fetch(`/api/device/${encodeURIComponent(uid)}/history?hours=24&limit=50`, { cache: "no-store" });
      const snap = await res.json();
      if (typeof window.handleHistorySnapshot === "function"){
        window.handleHistorySnapshot(snap);
      }
    }catch(e){}
  }

  const es = new EventSource("/events/devices");

  es.onopen = () => { if (conn) conn.textContent = "connected"; };
  es.onerror = () => { if (conn) conn.textContent = "reconnecting…"; };

  // load history once
  loadHistory();

  es.addEventListener("devices", (evt) => {
    try{
      const snapshot = JSON.parse(evt.data);
      const d = pickDevice(snapshot);
      pushToFragment(d);
    }catch(e){}
  });
})();
