import { categories, tileAt, tileBounds, normalizeTile, compareTiles } from "./protocol.mjs";

const $ = (id) => document.getElementById(id);
lucide.createIcons();
const presets = {
    beijing: [116.425, 39.886],
    shandong: [116.9331413164, 36.7129163514],
    shanghai: [121.5045, 31.2398],
    guangzhou: [113.324, 23.111],
};
const state = { tiles: [], features: [], error: null, loaded: false, controller: null,
    selected: new Set(Object.keys(categories)), renderedCount: 0, comparison: null };
window.tencentGisPrototype = { state, normalizeTile, compareTiles };
const map = L.map("map", { preferCanvas: true, maxZoom: 20, minZoom: 3 })
    .setView([39.886, 116.425], 16);
window.tencentGisPrototype.map = map;
let baseLayer;
let vectorLayer;
const gridLayer = L.layerGroup().addTo(map);
let metadata;
let requestSequence = 0;
const renderer = L.canvas({ padding: 0.25, tolerance: 4 });

for (const [id, item] of Object.entries(categories)) {
    const label = document.createElement("label");
    label.className = "layer-row";
    label.innerHTML = `<input type="checkbox" data-category="${id}" checked>` +
        `<span class="swatch" style="background:${item.color}"></span>` +
        `<span>${item.name}</span><span class="count" id="count-${id}">0</span>`;
    label.querySelector("input").addEventListener("change", (event) => {
        if (event.target.checked) state.selected.add(id);
        else state.selected.delete(id);
        $("all-layers").checked = state.selected.size === Object.keys(categories).length;
        $("all-layers").indeterminate = state.selected.size > 0 && !$("all-layers").checked;
        render();
    });
    $("layers").append(label);
}

function status(message, error = false) {
    $("status").textContent = message;
    $("status").classList.toggle("error", error);
}

function setBase() {
    if (baseLayer) map.removeLayer(baseLayer);
    const id = $("basemap").value;
    if (id === "none") return;
    baseLayer = L.tileLayer(`/api/${id}/{z}/{x}/{y}`, {
        minZoom: 3, maxNativeZoom: 18, maxZoom: 20, attribution: "© 腾讯地图",
    }).addTo(map);
    baseLayer.on("tileerror", () => status("底图瓦片加载失败；矢量数据仍可单独检查", true));
}
setBase();

function visibleFeatures() {
    const visibility = new Map();
    return state.features.filter((feature) => state.selected.has(feature.properties.category) &&
        (!$("style-filter").checked || (() => {
            const { layerId, kind } = feature.properties;
            if (!visibility.has(layerId)) {
                visibility.set(layerId, (metadata.styles[`${kind}Styles`]?.[layerId] || [])
                    .some((style) => style.minScaleVal <= map.getZoom() &&
                        style.maxScaleVal >= map.getZoom()));
            }
            return visibility.get(layerId);
        })()));
}

function featureStyle(feature) {
    const properties = feature.properties;
    let color = categories[properties.category].color;
    if (properties.category === "building" && $("height-colors").checked &&
        properties.heightMeters > 0) {
        color = `hsl(${(properties.heightMeters * 23) % 360} 61% 45%)`;
    }
    const opacity = Number($("opacity").value) / 100;
    return { color, weight: properties.kind === "region" ? 0.8 : 2.3,
        fillColor: color, fillOpacity: opacity * 0.65, opacity, renderer };
}

function inspect(feature) {
    $("selection").hidden = false;
    const { localCoordinates, ...properties } = feature.properties;
    $("properties").textContent = JSON.stringify({ ...properties,
        geometry: feature.geometry.type, firstLocalPoint: localCoordinates[0][0] }, null, 2);
}

function updateStatistics() {
    const heights = state.features.map((f) => f.properties.heightMeters)
        .filter((height) => height > 0);
    const values = {
        "数据 / 显示层级": `${state.tiles[0]?.tile.z ?? "—"} / ${map.getZoom()}`,
        "瓦片 / 要素": `${state.tiles.length} / ${state.features.length.toLocaleString()}`,
        "当前可见": state.renderedCount.toLocaleString(),
        "带高度建筑": heights.length.toLocaleString(),
        "高度范围": heights.length ? `${Math.min(...heights)}–${Math.max(...heights)} m` : "—",
        "未知样式": state.features.filter((f) => !f.properties.knownStyle).length,
        "退化几何": state.features.filter((f) => f.geometry === null).length,
        "补充 POI": state.tiles.reduce((sum, t) => sum + t.extraPoiCount, 0),
    };
    $("statistics").replaceChildren();
    for (const [name, value] of Object.entries(values)) {
        const term = document.createElement("dt");
        term.textContent = name;
        const description = document.createElement("dd");
        description.textContent = value;
        $("statistics").append(term, description);
    }
    for (const id of Object.keys(categories)) {
        $(`count-${id}`).textContent = state.features.filter(
            (feature) => feature.properties.category === id).length.toLocaleString();
    }
}

function render() {
    if (vectorLayer) map.removeLayer(vectorLayer);
    const features = visibleFeatures();
    state.renderedCount = features.filter((feature) => feature.geometry !== null).length;
    vectorLayer = L.geoJSON(features, {
        renderer,
        style: featureStyle,
        pointToLayer: (feature, latlng) => L.circleMarker(latlng,
            { ...featureStyle(feature), radius: 3.5, fillOpacity: 0.8 }),
        onEachFeature: (feature, layer) => {
            const label = document.createElement("span");
            label.textContent = `${categories[feature.properties.category].name} · ` +
                `${feature.properties.layerId} ${feature.properties.name}`;
            layer.bindTooltip(label);
            layer.on("click", () => inspect(feature));
        },
    }).addTo(map);
    gridLayer.clearLayers();
    if ($("tile-grid").checked) {
        for (const { tile } of state.tiles) {
            const bounds = L.rectangle(tileBounds(tile),
                { color: "#177b74", fill: false, weight: 1, dashArray: "5 4",
                    interactive: false, renderer });
            gridLayer.addLayer(bounds);
        }
    }
    updateStatistics();
}

async function json(url, signal) {
    const response = await fetch(url, { signal });
    const body = await response.json();
    if (!response.ok) throw new Error(body.error || `HTTP ${response.status}`);
    return body;
}

function fit() {
    if (!state.tiles.length) return;
    const bounds = L.latLngBounds(state.tiles.flatMap(({ tile }) => tileBounds(tile)));
    map.fitBounds(bounds, { padding: [20, 40], maxZoom: 18 });
}

async function load() {
    state.controller?.abort();
    const sequence = ++requestSequence;
    const controller = new AbortController();
    state.controller = controller;
    state.error = null;
    state.loaded = false;
    $("load").disabled = true;
    $("cancel").disabled = false;
    $("compare").disabled = true;
    $("export").disabled = true;
    $("comparison").textContent = "尚未对比";
    state.comparison = null;
    try {
        const lon = Number($("lon").value);
        const lat = Number($("lat").value);
        if (!Number.isFinite(lon) || !Number.isFinite(lat) ||
            lon < 73 || lon > 136 || lat < 1 || lat > 55) {
            throw new Error("请输入中国范围内的有效 GCJ-02 经纬度");
        }
        const center = tileAt(lon, lat, Number($("zoom").value));
        const half = (Number($("grid").value) - 1) / 2;
        const wanted = [];
        for (let y = -half; y <= half; ++y) {
            for (let x = -half; x <= half; ++x) {
                wanted.push({ x: center.x + x, y: center.y + y, z: center.z });
            }
        }
        const loaded = [];
        let next = 0;
        status(`加载 0 / ${wanted.length} 个瓦片…`);
        await Promise.all(Array.from({ length: Math.min(3, wanted.length) }, async () => {
            while (next < wanted.length) {
                const tile = wanted[next++];
                const envelope = await json(`/api/vector/${tile.z}/${tile.x}/${tile.y}`,
                    controller.signal);
                const normalized = normalizeTile(envelope, tile, metadata);
                normalized.envelope = envelope;
                loaded.push(normalized);
                if (sequence === requestSequence) status(`加载 ${loaded.length} / ${wanted.length} 个瓦片…`);
            }
        }));
        if (controller.signal.aborted || sequence !== requestSequence) return;
        loaded.sort((a, b) => a.tile.y - b.tile.y || a.tile.x - b.tile.x);
        state.tiles = loaded;
        state.features = loaded.flatMap((tile) => tile.features);
        state.loaded = true;
        render();
        fit();
        $("tile-status").textContent = `GCJ-02 · z${center.z} · ${center.x}, ${center.y}`;
        status(`已加载 ${loaded.length} 个瓦片，${state.features.length.toLocaleString()} 个要素`);
        $("compare").disabled = false;
        $("export").disabled = false;
    } catch (error) {
        controller.abort();
        if (sequence !== requestSequence) return;
        if (error.name !== "AbortError") {
            state.error = error.message;
            status(error.message, true);
        } else status("已取消，保留上次加载结果");
    } finally {
        if (sequence === requestSequence) {
            $("load").disabled = false;
            $("cancel").disabled = true;
            $("compare").disabled = state.tiles.length === 0;
            $("export").disabled = state.tiles.length === 0;
        }
    }
}

async function compare() {
    const tiles = [...state.tiles];
    if (!tiles.length) return;
    const sequence = requestSequence;
    $("compare").disabled = true;
    $("comparison").textContent = "正在逐瓦片对比…";
    try {
        const results = [];
        for (const compressed of tiles) {
            const tile = compressed.tile;
            const absolute = await json(`/api/vector/${tile.z}/${tile.x}/${tile.y}?compress=0`);
            results.push({ tile, ...compareTiles(compressed,
                normalizeTile(absolute, tile, metadata, false)) });
        }
        if (sequence !== requestSequence) return;
        state.comparison = { matched: results.every((item) => item.matched), results };
        $("comparison").textContent = state.comparison.matched ?
            `${results.length} / ${results.length} 瓦片一致；分类、几何、原始字段与高度一致` :
            `发现 ${results.reduce((sum, item) => sum + item.differences, 0)} 处差异`;
    } catch (error) {
        if (sequence === requestSequence) $("comparison").textContent = `对比失败：${error.message}`;
    } finally {
        if (sequence === requestSequence) $("compare").disabled = false;
    }
}

$("query").addEventListener("submit", (event) => { event.preventDefault(); load(); });
$("cancel").addEventListener("click", () => state.controller?.abort());
$("preset").addEventListener("change", () => {
    const position = presets[$("preset").value];
    if (position) { $("lon").value = position[0]; $("lat").value = position[1]; load(); }
});
for (const id of ["lon", "lat"]) $(id).addEventListener("input", () => { $("preset").value = "custom"; });
$("fit").addEventListener("click", fit);
$("compare").addEventListener("click", compare);
$("basemap").addEventListener("change", setBase);
for (const id of ["height-colors", "tile-grid", "style-filter"]) $(id).addEventListener("change", render);
$("opacity").addEventListener("input", () => {
    $("opacity-value").textContent = `${$("opacity").value}%`;
    const opacity = Number($("opacity").value) / 100;
    vectorLayer?.setStyle({ opacity, fillOpacity: opacity * 0.65 });
});
$("all-layers").addEventListener("change", (event) => {
    state.selected = new Set(event.target.checked ? Object.keys(categories) : []);
    for (const checkbox of document.querySelectorAll("[data-category]")) {
        checkbox.checked = event.target.checked;
    }
    render();
});
$("close-selection").addEventListener("click", () => { $("selection").hidden = true; });
$("export").addEventListener("click", () => {
    const features = visibleFeatures().map((feature) => {
        const { localCoordinates, ...properties } = feature.properties;
        return { ...feature, properties };
    });
    const blob = new Blob([JSON.stringify({ type: "FeatureCollection",
        coordinateSystem: "GCJ-02", provider: "tencent", features })], { type: "application/json" });
    const anchor = document.createElement("a");
    anchor.href = URL.createObjectURL(blob);
    anchor.download = "腾讯矢量_GCJ02.geojson";
    anchor.click();
    setTimeout(() => URL.revokeObjectURL(anchor.href), 1000);
});
map.on("zoomend", () => $("style-filter").checked ? render() : updateStatistics());
try {
    metadata = await json("/api/meta");
    window.tencentGisPrototype.metadata = metadata;
    $("source-version").textContent = `腾讯地图 · 数据 v${metadata.version} · GCJ-02`;
    await load();
} catch (error) { state.error = error.message; status(error.message, true); }
