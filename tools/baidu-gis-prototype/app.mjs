import { categories, grids, tileAt, tileBounds, normalizeTile, toMercator, fromMercator } from "./protocol.mjs";

const $ = (id) => document.getElementById(id);
lucide.createIcons();
const presets = { beijing: [116.425, 39.886], shandong: [116.9396, 36.719],
    shanghai: [121.511, 31.246], guangzhou: [113.3305, 23.117] };
const state = { tiles: [], features: [], error: null, loaded: false, controller: null,
    selected: new Set(Object.keys(categories)), renderedCount: 0 };
const crs = L.extend({}, L.CRS.Simple, {
    code: "BD09-MC",
    projection: {
        project: (latlng) => L.point(...toMercator(latlng.lng, latlng.lat)),
        unproject: (point) => { const [lng, lat] = fromMercator(point.x, point.y); return L.latLng(lat, lng); },
    },
    transformation: new L.Transformation(1, 0, -1, 0),
    scale: (zoom) => 2 ** (zoom - 18),
    zoom: (scale) => Math.log2(scale) + 18,
    distance: (a, b) => a.distanceTo(b),
});
const map = L.map("map", { crs, preferCanvas: true, minZoom: 3, maxZoom: 21 })
    .setView([39.886, 116.425], 18);
const renderer = L.canvas({ padding: .25, tolerance: 4 });
window.baiduGisPrototype = { state, map, normalizeTile };
let baseLayer, vectorLayer, sequence = 0, metadata;
const gridLayer = L.layerGroup().addTo(map);
function status(message, error = false) {
    $("status").textContent = message;
    $("status").classList.toggle("error", error);
}
for (const [id, category] of Object.entries(categories)) {
    const row = document.createElement("label");
    row.className = "layer-row";
    row.innerHTML = `<input type="checkbox" data-category="${id}" checked>` +
        `<span class="swatch" style="background:${category.color}"></span><span>${category.name}</span>` +
        `<span class="count" id="count-${id}">0</span>`;
    row.querySelector("input").addEventListener("change", (event) => {
        if (event.target.checked) state.selected.add(id); else state.selected.delete(id);
        $("all-layers").checked = state.selected.size === Object.keys(categories).length;
        $("all-layers").indeterminate = Boolean(state.selected.size) && !$("all-layers").checked;
        render();
    });
    $("layers").append(row);
}
function setBase() {
    if (baseLayer) map.removeLayer(baseLayer);
    if ($("basemap").value === "none") return;
    baseLayer = L.tileLayer("/api/electronic/{z}/{x}/{y}", {
        minZoom: 3, maxNativeZoom: 19, maxZoom: 21, noWrap: true, attribution: "© 百度地图",
    }).addTo(map);
    baseLayer.on("tileerror", () => status("百度底图加载失败", true));
}
setBase();
function visibleFeatures() {
    return state.features.filter((feature) => {
        const p = feature.properties;
        const grid = grids[p.tile.z];
        const level = Math.max(grid.start, Math.min(grid.end, Math.floor(map.getZoom())));
        const visible = Boolean((p.visibilityMask >> (7 - (level - grid.start))) & 1);
        return state.selected.has(p.category) && ($("style").value === "all" ||
            String(p.styleId) === $("style").value) &&
            (!$("style-filter").checked || visible);
    });
}
function featureStyle(feature) {
    const p = feature.properties;
    let color = categories[p.category].color;
    if (p.category === "building" && $("height-colors").checked && p.heightRaw > 0) {
        color = `hsl(${p.heightRaw * 23 % 360} 61% 45%)`;
    }
    const area = feature.geometry?.type === "GeometryCollection";
    const opacity = Number($("opacity").value) / 100;
    return { color, fillColor: color, opacity, fillOpacity: opacity * .65,
        weight: area ? 0 : 2.2, renderer };
}
function updateStatistics() {
    const heights = state.features.filter((f) => f.properties.category === "building")
        .map((f) => f.properties.heightRaw).filter((h) => h > 0);
    const skipped = state.tiles.reduce((sum, tile) => sum + tile.skipped.length, 0);
    const values = { "数据 / 显示层级": `${state.tiles[0]?.tile.z ?? "-"} / ${map.getZoom()}`,
        "瓦片 / 几何要素": `${state.tiles.length} / ${state.features.length.toLocaleString()}`,
        "当前可见": state.renderedCount.toLocaleString(), "带高度建筑": heights.length,
        "高度原值范围": heights.length ? `${Math.min(...heights)}–${Math.max(...heights)}` : "-",
        "未解析图层块": skipped };
    $("statistics").replaceChildren();
    for (const [name, value] of Object.entries(values)) {
        const dt = document.createElement("dt"), dd = document.createElement("dd");
        dt.textContent = name; dd.textContent = value;
        $("statistics").append(dt, dd);
    }
    for (const id of Object.keys(categories)) {
        $(`count-${id}`).textContent = state.features.filter((f) => f.properties.category === id)
            .length.toLocaleString();
    }
}
function render() {
    if (vectorLayer) map.removeLayer(vectorLayer);
    const features = visibleFeatures();
    state.renderedCount = features.filter((f) => f.geometry).length;
    vectorLayer = L.geoJSON(features, {
        renderer, style: featureStyle,
        pointToLayer: (feature, latlng) => L.circleMarker(latlng,
            { ...featureStyle(feature), radius: 3.5, fillOpacity: .8 }),
        onEachFeature: (feature, layer) => {
            const label = document.createElement("span");
            const p = feature.properties;
            label.textContent = `${p.name || categories[p.category].name} · 样式 ${p.styleId}` +
                (p.heightRaw ? ` · 高度原值 ${p.heightRaw}` : "");
            layer.bindTooltip(label);
            layer.on("click", () => {
                $("selection").hidden = false;
                const { rawCoordinates, localCoordinates, triangles, ...properties } = p;
                $("properties").textContent = JSON.stringify({ ...properties,
                    vertexCount: localCoordinates?.length ?? 1,
                    triangleCount: (triangles?.length ?? 0) / 3,
                    firstLocalPoint: localCoordinates?.[0] }, null, 2);
            });
        },
    }).addTo(map);
    gridLayer.clearLayers();
    if ($("tile-grid").checked) {
        for (const { tile } of state.tiles) {
            gridLayer.addLayer(L.rectangle(tileBounds(tile), { color: "#177b74", fill: false,
                weight: 1, dashArray: "5 4", interactive: false, renderer }));
        }
    }
    updateStatistics();
}
function fit() {
    if (state.tiles.length) map.fitBounds(L.latLngBounds(state.tiles.flatMap(({ tile }) =>
        tileBounds(tile))), { padding: [20, 45], maxZoom: 19 });
}
async function load() {
    state.controller?.abort();
    const current = ++sequence, controller = new AbortController();
    state.controller = controller; state.loaded = false; state.error = null;
    $("load").disabled = true; $("cancel").disabled = false; $("export").disabled = true;
    try {
        const lon = Number($("lon").value), lat = Number($("lat").value);
        if (!Number.isFinite(lon) || !Number.isFinite(lat) ||
            lon < 73 || lon > 136 || lat < 1 || lat > 55) throw new Error("请输入中国范围内的 BD09 经纬度");
        const center = tileAt(lon, lat, Number($("zoom").value));
        const half = (Number($("grid").value) - 1) / 2, wanted = [], loaded = [];
        for (let y = -half; y <= half; y++) for (let x = -half; x <= half; x++) {
            wanted.push({ x: center.x + x, y: center.y + y, z: center.z });
        }
        let next = 0;
        status(`加载 0 / ${wanted.length} 个瓦片…`);
        await Promise.all(Array.from({ length: Math.min(3, wanted.length) }, async () => {
            while (next < wanted.length) {
                const tile = wanted[next++];
                const response = await fetch(`/api/vector/${tile.z}/${tile.x}/${tile.y}`,
                    { signal: controller.signal });
                if (!response.ok) throw new Error((await response.json()).error);
                loaded.push(normalizeTile(await response.arrayBuffer(), tile));
                if (current === sequence) status(`加载 ${loaded.length} / ${wanted.length} 个瓦片…`);
            }
        }));
        if (controller.signal.aborted || current !== sequence) return;
        loaded.sort((a, b) => a.tile.y - b.tile.y || a.tile.x - b.tile.x);
        state.tiles = loaded; state.features = loaded.flatMap((t) => t.features);
        $("style").replaceChildren(new Option("全部", "all"));
        for (const id of [...new Set(state.features.map((f) => f.properties.styleId))].sort((a,b) => a-b)) {
            $("style").append(new Option(String(id), String(id)));
        }
        const groupCounts = new Map();
        for (const tile of loaded) for (const g of tile.groups) {
            const key = `${g.groupId} / ${g.order}${g.supported ? "" : "（未解析）"}`;
            groupCounts.set(key, (groupCounts.get(key) || 0) + 1);
        }
        $("groups").textContent = [...groupCounts].map(([key,count]) => `${key}: ${count}`).join("\n");
        state.loaded = true;
        fit(); render();
        $("tile-status").textContent = `BD09 · 数据 z${center.z} · ${center.x}, ${center.y}`;
        status(`已加载 ${loaded.length} 个瓦片，${state.features.length.toLocaleString()} 个几何要素`);
        $("export").disabled = false;
    } catch (error) {
        controller.abort();
        if (current !== sequence) return;
        if (error.name === "AbortError") status("已取消，保留上次加载结果");
        else { state.error = error.message; status(error.message, true); }
    } finally {
        if (current === sequence) {
            $("load").disabled = false; $("cancel").disabled = true;
            $("export").disabled = !state.tiles.length;
        }
    }
}
$("query").addEventListener("submit", (e) => { e.preventDefault(); load(); });
$("preset").addEventListener("change", () => {
    const point = presets[$("preset").value];
    if (point) { $("lon").value = point[0]; $("lat").value = point[1]; load(); }
});
for (const id of ["lon","lat"]) $(id).addEventListener("input", () => { $("preset").value = "custom"; });
$("fit").addEventListener("click", fit);
$("cancel").addEventListener("click", () => state.controller?.abort());
$("basemap").addEventListener("change", setBase);
for (const id of ["tile-grid","height-colors","style-filter","style"]) $(id).addEventListener("change", render);
$("opacity").addEventListener("input", () => { $("opacity-value").textContent = `${$("opacity").value}%`; render(); });
$("all-layers").addEventListener("change", (event) => {
    state.selected = new Set(event.target.checked ? Object.keys(categories) : []);
    for (const checkbox of document.querySelectorAll("[data-category]")) checkbox.checked = event.target.checked;
    render();
});
$("close-selection").addEventListener("click", () => { $("selection").hidden = true; });
$("export").addEventListener("click", () => {
    const features = visibleFeatures().map((feature) => {
        const { rawCoordinates, localCoordinates, triangles, ...properties } = feature.properties;
        return { ...feature, properties };
    });
    const blob = new Blob([JSON.stringify({ type: "FeatureCollection", coordinateSystem: "BD09",
        provider: "baidu", geometryEncoding: "original-triangle-faces", features })],
        { type: "application/json" });
    const anchor = document.createElement("a");
    anchor.href = URL.createObjectURL(blob); anchor.download = "百度矢量_BD09.geojson"; anchor.click();
    setTimeout(() => URL.revokeObjectURL(anchor.href), 1000);
});
map.on("zoomend", () => $("style-filter").checked ? render() : updateStatistics());
try {
    const response = await fetch("/api/meta");
    metadata = await response.json();
    if (!response.ok) throw new Error(metadata.error);
    window.baiduGisPrototype.metadata = metadata;
    $("source-version").textContent = `百度 · v${metadata.version} / ${metadata.updateDate} · BD09`;
    await load();
} catch (error) { state.error = error.message; status(error.message, true); }
