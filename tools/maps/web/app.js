// SPDX-License-Identifier: BSD-3-Clause
// The Map Pack tool's browser mode: draw Regions on an OpenStreetMap map,
// watch the estimate, save the Regions file, build the Pack. The server is
// browser.py; the Regions file's format is docs/maps/map-pack-format.md.
"use strict";

(function () {
  const DEPTHS = [14, 15, 16, 17];
  const DEFAULT_DEPTH = 16;
  const MIN_RADIUS_M = 5;
  const SIMPLIFY_PX = 1.5;  // a freehand line's wobble below this is dropped
  const TOOLS = ["pan", "circle", "freehand"];

  const $ = (id) => document.getElementById(id);
  const ui = {
    map: $("map"), regions: $("regions"), none: $("no-regions"), estimate: $("estimate"),
    warning: $("warning"), budget: $("budget"), status: $("status"), log: $("log"),
    file: $("file"), hint: $("hint"),
  };

  // -- sizes, as thistle_maps.py writes and reads them ----------------------

  const UNITS = [["GB", 2 ** 30], ["MB", 2 ** 20], ["KB", 2 ** 10]];

  function humanSize(n) {
    for (const [unit, scale] of UNITS) {
      if (n >= scale) return `${Number((n / scale).toPrecision(3))} ${unit}`;
    }
    return `${n} bytes`;
  }

  function parseSize(text) {
    const t = text.trim().toUpperCase().replace(/\s+/g, "").replace(/B$/, "");
    const scale = { K: 2 ** 10, M: 2 ** 20, G: 2 ** 30, T: 2 ** 40 }[t.slice(-1)];
    const value = Number(scale ? t.slice(0, -1) : t);
    return value > 0 ? Math.floor(value * (scale || 1)) : null;
  }

  // -- state ------------------------------------------------------------------

  // A Region: {name, deepest_zoom, kind: "circle"|"polygon", centre: [lat, lon],
  // radius_m, points: [[lat, lon], ...] (not closed), props, feature: the
  // members of a loaded feature and its properties, kept as they were}.
  let regions = [];
  let selected = null;
  let budget = 2 ** 30;
  let dirty = false;
  let lastEstimate = null;
  let state = {};

  const round7 = (v) => Math.round(v * 1e7) / 1e7;

  function fromGeoJSON(doc) {
    const out = [];
    for (const feature of (doc && doc.features) || []) {
      const props = Object.assign({}, feature.properties || {});
      const geometry = feature.geometry || {};
      const region = {
        name: typeof props.name === "string" ? props.name : "",
        deepest_zoom: DEPTHS.includes(props.deepest_zoom) ? props.deepest_zoom : DEFAULT_DEPTH,
        props, feature,
      };
      if (geometry.type === "Point") {
        region.kind = "circle";
        region.centre = [geometry.coordinates[1], geometry.coordinates[0]];
        region.radius_m = props.radius_m;
      } else if (geometry.type === "Polygon") {
        region.kind = "polygon";
        region.points = geometry.coordinates[0].map(([lon, lat]) => [lat, lon]);
        const first = region.points[0];
        const last = region.points[region.points.length - 1];
        if (region.points.length > 1 && first[0] === last[0] && first[1] === last[1]) {
          region.points.pop();
        }
      } else {
        continue;  // the server refuses such a file; nothing to draw
      }
      out.push(region);
    }
    return out;
  }

  function toGeoJSON() {
    const features = regions.map((r) => {
      const props = Object.assign({}, r.props);
      if (r.name.trim()) props.name = r.name.trim(); else delete props.name;
      props.deepest_zoom = r.deepest_zoom;
      let geometry;
      if (r.kind === "circle") {
        props.radius_m = r.radius_m;
        geometry = { type: "Point", coordinates: [round7(r.centre[1]), round7(r.centre[0])] };
      } else {
        delete props.radius_m;
        const ring = r.points.map(([lat, lon]) => [round7(lon), round7(lat)]);
        ring.push(ring[0].slice());
        geometry = { type: "Polygon", coordinates: [ring] };
      }
      return Object.assign({}, r.feature || {}, { type: "Feature", properties: props, geometry });
    });
    return { type: "FeatureCollection", thistle_regions: 1, features };
  }

  function nextName() {
    const taken = new Set(regions.map((r) => r.name));
    let n = regions.length + 1;
    while (taken.has(`Region ${n}`)) n += 1;
    return `Region ${n}`;
  }

  // -- the map ------------------------------------------------------------------

  const map = L.map(ui.map, { zoomControl: true, worldCopyJump: true }).setView([20, 0], 2);
  window.thistleMaps = { map };
  const layers = L.featureGroup().addTo(map);

  function style(region) {
    const chosen = region === selected;
    return { color: chosen ? "#c0392b" : "#2a5d9f", weight: chosen ? 3 : 2,
             fillOpacity: 0.12 + 0.05 * (region.deepest_zoom - 14) };
  }

  function drawRegions() {
    layers.clearLayers();
    regions.forEach((region, i) => {
      const layer = region.kind === "circle"
        ? L.circle(region.centre, { radius: region.radius_m, ...style(region) })
        : L.polygon(region.points, style(region));
      layer.bindTooltip(`${region.name || `Region ${i + 1}`}, to z${region.deepest_zoom}`);
      layer.on("click", (e) => { if (tool === "pan") { L.DomEvent.stop(e); select(region); } });
      layers.addLayer(layer);
    });
  }

  // -- the list ---------------------------------------------------------------

  function element(tag, attrs, ...children) {
    const el = document.createElement(tag);
    Object.assign(el, attrs);
    children.forEach((c) => el.append(c));
    return el;
  }

  function drawList() {
    ui.regions.replaceChildren();
    ui.none.hidden = regions.length > 0;
    regions.forEach((region, i) => {
      const name = element("input", { className: "name", value: region.name,
                                      placeholder: `Region ${i + 1}`, title: "The Region's name" });
      name.addEventListener("input", () => { region.name = name.value; changed({ list: false }); });
      const depth = element("select", { className: "depth", title: "The deepest zoom this Region needs" });
      DEPTHS.forEach((z) => depth.append(element("option", { value: String(z), textContent: `to z${z}` })));
      depth.value = String(region.deepest_zoom);
      depth.addEventListener("change", () => {
        region.deepest_zoom = Number(depth.value);
        changed({ list: false });
      });
      const remove = element("button", { className: "delete", textContent: "Delete",
                                         title: "Delete this Region" });
      remove.addEventListener("click", () => removeRegion(region));
      const row = element("div", { className: "row" }, name, depth, remove);
      const detail = element("div", { className: "row" });
      if (region.kind === "circle") {
        const radius = element("input", { className: "radius", type: "number", min: "1", step: "1",
                                          value: String(region.radius_m), title: "Radius in metres" });
        radius.addEventListener("change", () => {
          const r = Math.round(Number(radius.value));
          if (r >= 1) { region.radius_m = r; changed({ list: false }); }
          else radius.value = String(region.radius_m);
        });
        detail.append(element("span", { className: "kind", textContent: "Circle, radius" }), radius,
                      element("span", { className: "kind", textContent: "m" }));
      } else {
        detail.append(element("span", { className: "kind",
                                         textContent: `Freehand, ${region.points.length} points` }));
      }
      const item = element("li", { className: "region" + (region === selected ? " selected" : "") },
                           row, detail);
      item.addEventListener("click", (e) => {
        if (e.target === remove || selected === region) return;
        selected = region;
        drawRegions();
        markSelected();
      });
      ui.regions.append(item);
    });
  }

  function markSelected() {
    [...ui.regions.children].forEach((li, i) => li.classList.toggle("selected", regions[i] === selected));
  }

  function select(region) {
    selected = region;
    drawRegions();
    markSelected();
    const i = regions.indexOf(region);
    if (i >= 0) ui.regions.children[i].scrollIntoView({ block: "nearest" });
  }

  function removeRegion(region) {
    regions = regions.filter((r) => r !== region);
    if (selected === region) selected = null;
    changed();
  }

  function changed({ list = true } = {}) {
    dirty = true;
    setStatus("Not saved yet");
    drawRegions();
    if (list) drawList();
    scheduleEstimate();
  }

  // -- drawing ------------------------------------------------------------------

  let tool = "pan";
  let stroke = null;  // what is being drawn: {kind, centre, points, last, layer}

  function setTool(name) {
    tool = name;
    for (const t of TOOLS) $(`tool-${t}`).classList.toggle("active", t === name);
    ui.map.classList.toggle("drawing", name !== "pan");
    if (name === "pan") map.dragging.enable(); else map.dragging.disable();
    ui.hint.textContent = {
      pan: "Choose Circle or Freehand, then draw on the map.",
      circle: "Press at the centre and let go on the edge.",
      freehand: "Hold the button down and draw round the place.",
    }[name];
    cancelStroke();
  }

  function cancelStroke() {
    if (stroke && stroke.layer) map.removeLayer(stroke.layer);
    stroke = null;
  }

  map.on("mousedown", (e) => {
    if (tool === "pan" || (e.originalEvent && e.originalEvent.button !== 0)) return;
    if (tool === "circle") {
      stroke = { kind: "circle", centre: e.latlng, last: e.latlng,
                 layer: L.circle(e.latlng, { radius: 0, color: "#c0392b", weight: 2 }).addTo(map) };
    } else {
      stroke = { kind: "polygon", points: [e.latlng], last: e.latlng,
                 layer: L.polyline([e.latlng], { color: "#c0392b", weight: 2 }).addTo(map) };
    }
  });

  map.on("mousemove", (e) => {
    if (!stroke) return;
    stroke.last = e.latlng;
    if (stroke.kind === "circle") stroke.layer.setRadius(map.distance(stroke.centre, e.latlng));
    else { stroke.points.push(e.latlng); stroke.layer.addLatLng(e.latlng); }
  });

  function finishStroke(latlng) {
    if (!stroke) return;
    const done = stroke;
    cancelStroke();
    let region = null;
    if (done.kind === "circle") {
      const radius = Math.round(map.distance(done.centre, latlng));
      if (radius >= MIN_RADIUS_M) {
        region = { kind: "circle", centre: [done.centre.lat, done.centre.lng], radius_m: radius };
      }
    } else {
      done.points.push(latlng);
      const px = done.points.map((ll) => map.latLngToLayerPoint(ll));
      const kept = L.LineUtil.simplify(px, SIMPLIFY_PX).map((p) => map.layerPointToLatLng(p));
      if (kept.length > 1 && kept[0].equals(kept[kept.length - 1])) kept.pop();
      if (kept.length >= 3) region = { kind: "polygon", points: kept.map((ll) => [ll.lat, ll.lng]) };
    }
    if (!region) { setStatus("Too small to be a Region"); return; }
    Object.assign(region, { name: nextName(), deepest_zoom: DEFAULT_DEPTH, props: {}, feature: null });
    regions.push(region);
    selected = region;
    setTool("pan");
    changed();
  }

  map.on("mouseup", (e) => finishStroke(e.latlng));
  document.addEventListener("mouseup", (e) => {
    // Let go outside the map: finish where the pointer last was on it.
    if (stroke && !ui.map.contains(e.target)) finishStroke(stroke.last);
  });

  for (const t of TOOLS) $(`tool-${t}`).addEventListener("click", () => setTool(t));

  document.addEventListener("keydown", (e) => {
    const typing = ["INPUT", "SELECT", "TEXTAREA"].includes(document.activeElement.tagName);
    if (e.key === "Escape") { setTool("pan"); return; }
    if (typing) return;
    if (e.key === "c" || e.key === "C") setTool("circle");
    else if (e.key === "f" || e.key === "F") setTool("freehand");
    else if ((e.key === "Delete" || e.key === "Backspace") && selected) removeRegion(selected);
  });

  // -- the server -------------------------------------------------------------

  async function post(path, body) {
    const response = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json" },
                                         body: JSON.stringify(body) });
    const answer = await response.json().catch(() => ({ error: `${response.status}` }));
    if (!response.ok) throw new Error(answer.error || `${response.status}`);
    return answer;
  }

  function setStatus(text, error = false) {
    ui.status.textContent = text;
    ui.status.classList.toggle("error", error);
  }

  // -- the estimate -------------------------------------------------------------

  let estimateTimer = null;
  let estimateSeq = 0;

  function scheduleEstimate() {
    clearTimeout(estimateTimer);
    estimateTimer = setTimeout(runEstimate, 200);
  }

  function showEstimate(result) {
    lastEstimate = result;
    if (!result) {
      ui.estimate.textContent = "0 tiles";
      ui.estimate.dataset.bytes = "0";
      ui.warning.hidden = true;
      return;
    }
    const levels = Object.entries(result.tiles_per_zoom).map(([z, n]) => `z${z} ${n}`).join(", ");
    const how = result.measured ? "measured from the map data" : "priced as a city's tiles";
    ui.estimate.textContent = `${result.tiles} tiles (${levels}), about ${humanSize(result.bytes)}, ${how}`;
    ui.estimate.dataset.bytes = String(result.bytes);
    ui.warning.hidden = result.bytes <= budget;
    ui.warning.textContent = `More than the ${humanSize(budget)} budget`;
  }

  async function runEstimate() {
    const seq = ++estimateSeq;
    if (!regions.length) { showEstimate(null); return; }
    try {
      const result = await post("/api/estimate", toGeoJSON());
      if (seq === estimateSeq) showEstimate(result);
    } catch (err) {
      if (seq === estimateSeq) {
        ui.estimate.textContent = `No estimate: ${err.message}`;
        ui.warning.hidden = true;
      }
    }
  }

  ui.budget.addEventListener("change", () => {
    const value = parseSize(ui.budget.value);
    if (value) budget = value;
    ui.budget.value = humanSize(budget);
    showEstimate(lastEstimate);
  });

  $("measure").addEventListener("click", async () => {
    if (!regions.length) { setStatus("Draw a Region first", true); return; }
    const seq = ++estimateSeq;
    setStatus("Measuring: reading the map data and drawing a sample of the tiles...");
    try {
      const result = await post("/api/measure", toGeoJSON());
      if (seq === estimateSeq) showEstimate(result);
      setStatus(dirty ? "Measured; not saved yet" : "Measured");
    } catch (err) {
      setStatus(`Cannot measure: ${err.message}`, true);
    }
  });

  // -- saving and building ------------------------------------------------------

  async function save() {
    try {
      const answer = await post("/api/save", toGeoJSON());
      dirty = false;
      setStatus(`Saved to ${answer.saved}`);
      return true;
    } catch (err) {
      setStatus(`Not saved: ${err.message}`, true);
      return false;
    }
  }

  $("save").addEventListener("click", save);

  $("build").addEventListener("click", async () => {
    if (!(await save())) return;
    try {
      const answer = await post("/api/build", {});
      setStatus(`Building ${answer.output}...`);
      $("build").disabled = true;
      ui.log.textContent = "";
      pollBuild();
    } catch (err) {
      setStatus(`Cannot build: ${err.message}`, true);
    }
  });

  async function pollBuild() {
    let status;
    try {
      status = await (await fetch("/api/build")).json();
    } catch (err) {
      setTimeout(pollBuild, 1000);
      return;
    }
    ui.log.textContent = status.log.join("\n");
    ui.log.scrollTop = ui.log.scrollHeight;
    if (status.state === "running") { setTimeout(pollBuild, 500); return; }
    $("build").disabled = false;
    if (status.state === "done") setStatus(`Built ${state.output}`);
    else setStatus("The build failed; its output is below", true);
  }

  window.addEventListener("beforeunload", (e) => { if (dirty) e.preventDefault(); });

  // -- start ------------------------------------------------------------------

  async function start() {
    state = await (await fetch("/api/state")).json();
    L.tileLayer(state.tiles_url, { maxZoom: 19, attribution: state.attribution }).addTo(map);
    budget = state.budget;
    ui.budget.value = humanSize(budget);
    ui.file.textContent = `${state.regions_path} → ${state.output}`;
    if (state.error) setStatus(state.error, true);
    regions = fromGeoJSON(state.regions);
    drawRegions();
    drawList();
    if (regions.length) map.fitBounds(layers.getBounds(), { padding: [40, 40] });
    showEstimate(null);
    await runEstimate();
  }

  start();
})();
