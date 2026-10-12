/* global Cesium */
const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
for (const field of ["terrain", "buildings", "xyz"]) {
    if (params.has(field)) $(field).value = params.get(field);
}
const viewer = new Cesium.Viewer("scene", {
    baseLayer: false, geocoder: false, timeline: false, animation: false,
    baseLayerPicker: false, sceneModePicker: false, homeButton: false,
    navigationHelpButton: false, fullscreenButton: false,
    contextOptions: { webgl: { preserveDrawingBuffer: true } },
});
viewer.scene.globe.baseColor = Cesium.Color.fromCssColorString("#a8bca8");
viewer.scene.globe.maximumScreenSpaceError = 8;
viewer.scene.globe.depthTestAgainstTerrain = true;
let terrain;
let buildings;
let bounds;
let generation = 0;
let loaded = 0;
let failed = 0;
let imageryGeneration = 0;
const status = (message, error = false) => {
    $("status").textContent = message;
    $("status").className = error ? "error" : "";
};
window.cesiumPreview = { viewer, errors: [], selections: [] };
viewer.scene.renderError.addEventListener((scene, error) => {
    window.cesiumPreview.errors.push(String(error));
    status(String(error), true);
});
async function replaceImagery() {
    const current = ++imageryGeneration;
    viewer.imageryLayers.removeAll();
    if (!$("imagery-show").checked) return;
    // 局部影像作为覆盖层；Cesium 会把第一个影像层的边缘拉伸到整个地球。
    const background = document.createElement("canvas");
    background.width = 1;
    background.height = 1;
    const context = background.getContext("2d");
    context.fillStyle = "#a8bca8";
    context.fillRect(0, 0, 1, 1);
    const base = await Cesium.SingleTileImageryProvider.fromUrl(background.toDataURL());
    if (current !== imageryGeneration) return;
    viewer.imageryLayers.addImageryProvider(base);
    let provider;
    if ($("provider").value === "osm") {
        provider = new Cesium.OpenStreetMapImageryProvider({
            url: "https://tile.openstreetmap.org/",
        });
    } else if ($("provider").value === "arcgis") {
        provider = await Cesium.ArcGisMapServerImageryProvider.fromUrl(
            "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer");
    } else {
        const url = $("xyz").value;
        const options = { url };
        if ($("provider").value === "local") {
            const base = url.slice(0, url.indexOf("{z}"));
            const response = await fetch(base + "meta.json");
            if (!response.ok) throw new Error(`XYZ meta.json: ${response.status}`);
            const meta = await response.json();
            const b = meta.bounds || meta.rectangle;
            if (b) {
                const west = b.west ?? b[0];
                const south = b.south ?? b[1];
                const east = b.east ?? b[2];
                const north = b.north ?? b[3];
                if ([west, south, east, north].every(Number.isFinite)) {
                    options.rectangle = Cesium.Rectangle.fromDegrees(west, south, east, north);
                }
            }
            options.minimumLevel = meta.minZoom ?? meta.minzoom ?? 0;
            options.maximumLevel = meta.maxZoom ?? meta.maxzoom ?? 18;
        }
        provider = new Cesium.UrlTemplateImageryProvider(options);
    }
    if (current !== imageryGeneration) return;
    provider.errorEvent.addEventListener((error) => {
        error.retry = error.timesRetried < 2;
        if (!error.retry) {
            status(`影像加载失败：${error.message}`, true);
        }
    });
    viewer.imageryLayers.addImageryProvider(provider);
}
function locate() {
    if (!bounds) {
        if (buildings) {
            viewer.camera.flyToBoundingSphere(buildings.boundingSphere, { duration: 0 });
        }
        return;
    }
    const longitude = (bounds[0] + bounds[2]) / 2;
    const latitude = (bounds[1] + bounds[3]) / 2;
    const span = Math.max(bounds[2] - bounds[0], bounds[3] - bounds[1]) * 111320;
    viewer.camera.setView({
        destination: Cesium.Cartesian3.fromDegrees(longitude, latitude - span / 111320 * 0.7,
            Math.max(500, span * 0.8)),
        orientation: { heading: 0, pitch: -Cesium.Math.PI_OVER_FOUR, roll: 0 },
    });
}
async function load() {
    const current = ++generation;
    status("加载中");
    window.cesiumPreview.ready = false;
    window.cesiumPreview.errors.length = 0;
    window.cesiumPreview.selections.length = 0;
    failed = 0;
    loaded = 0;
    try {
        const address = $("terrain").value.trim();
        let nextTerrain;
        let nextBounds;
        if (address) {
            const directory = new URL(address.endsWith("/") ? address : address + "/",
                location.href);
            const response = await fetch(new URL("layer.json", directory));
            if (!response.ok) throw new Error(`layer.json: ${response.status}`);
            const layer = await response.json();
            nextBounds = layer.bounds;
            nextTerrain = await Cesium.CesiumTerrainProvider.fromUrl(directory.href);
            nextTerrain.errorEvent.addEventListener((error) => {
                failed++;
                window.cesiumPreview.errors.push(error.message);
                status(`地形加载失败：${error.message}`, true);
            });
        }
        if (current !== generation) return;
        terrain = nextTerrain;
        bounds = nextBounds;
        viewer.terrainProvider = $("terrain-show").checked && terrain ?
            terrain : new Cesium.EllipsoidTerrainProvider();
        const buildingAddress = $("buildings").value.trim();
        const nextBuildings = buildingAddress ?
            await Cesium.Cesium3DTileset.fromUrl(buildingAddress, {
            maximumScreenSpaceError: Number($("sse").value),
        }) : undefined;
        if (current !== generation) {
            nextBuildings?.destroy();
            return;
        }
        if (buildings) viewer.scene.primitives.remove(buildings);
        buildings = nextBuildings;
        if (buildings) {
            buildings.show = $("building-show").checked;
            buildings.tileFailed.addEventListener((error) => {
                failed++;
                window.cesiumPreview.errors.push(error.message);
                status(`建筑加载失败：${error.message}`, true);
            });
            buildings.tileLoad.addEventListener(() => loaded++);
            buildings.tileVisible.addEventListener((tile) => {
                const url = tile.content?.url || tile.content?.resource?.url || "";
                window.cesiumPreview.selections.push({ url, error: tile.geometricError });
                if (window.cesiumPreview.selections.length > 300) {
                    window.cesiumPreview.selections.shift();
                }
            });
            viewer.scene.primitives.add(buildings);
        }
        window.cesiumPreview.buildings = buildings;
        window.cesiumPreview.terrain = terrain;
        await replaceImagery();
        if (current !== generation) return;
        locate();
        window.cesiumPreview.ready = true;
        status(`已加载 · Cesium ${Cesium.VERSION}`);
    } catch (error) {
        window.cesiumPreview.errors.push(String(error));
        status(String(error), true);
    }
}
$("load").onclick = load;
$("locate").onclick = locate;
$("provider").onchange = () => replaceImagery().catch((error) => status(String(error), true));
$("imagery-show").onchange = $("provider").onchange;
$("terrain-show").onchange = () => {
    viewer.terrainProvider = $("terrain-show").checked && terrain ?
        terrain : new Cesium.EllipsoidTerrainProvider();
};
$("building-show").onchange = () => {
    if (buildings) buildings.show = $("building-show").checked;
};
$("wire").onchange = () => {
    if (buildings) buildings.debugWireframe = $("wire").checked;
    viewer.scene.globe._surface.tileProvider._debug.wireframe = $("wire").checked;
};
$("sse").oninput = () => {
    const value = Number($("sse").value);
    $("sse-value").value = value;
    viewer.scene.globe.maximumScreenSpaceError = value;
    if (buildings) buildings.maximumScreenSpaceError = value;
};
setInterval(async () => {
    try {
        const requests = await (await fetch("/requests")).json();
        $("requests").textContent = requests.slice(-30).map((entry) =>
            `${entry.status || 200} ${entry.path}`).join("\n");
        $("requests").style.whiteSpace = "pre-wrap";
        if (buildings && !failed) status(`建筑瓦块 ${loaded} · 请求 ${requests.length} · Cesium ${Cesium.VERSION}`);
    } catch { /* 外部静态服务器不提供请求统计。 */ }
}, 2000);
if (params.get("autoload") === "1") load();
