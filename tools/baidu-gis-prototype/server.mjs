import http from "node:http";
import path from "node:path";
import fs from "node:fs/promises";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import { validateTile, encodeParameters, normalizeTile, grids } from "./protocol.mjs";

const root = path.dirname(fileURLToPath(import.meta.url));
const workspace = path.resolve(root, "../..");
const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) args.set(process.argv[i], process.argv[i + 1]);
const cacheRoot = path.resolve(args.get("--cache") || path.join(workspace, "tmp/baidu-gis-prototype"));
const port = Number(args.get("--port") || 8777);
await fs.mkdir(cacheRoot, { recursive: true });

async function remote(url, target, validate = () => {}) {
    try {
        const cached = await fs.readFile(target);
        validate(cached);
        return cached;
    } catch (error) {
        if (error.code !== "ENOENT") await fs.rm(target, { force: true });
    }
    const response = await fetch(url, { signal: AbortSignal.timeout(20000) });
    if (!response.ok) throw new Error(`百度服务 HTTP ${response.status}`);
    const bytes = Buffer.from(await response.arrayBuffer());
    validate(bytes);
    await fs.mkdir(path.dirname(target), { recursive: true });
    await fs.writeFile(target, bytes);
    return bytes;
}
const sdk = await remote("https://api.map.baidu.com/getscript?v=1.0&type=webgl",
    path.join(cacheRoot, "sdk-gl.js"));
const text = sdk.toString("utf8");
const versions = JSON.parse(text.match(/window\.TILE_VERSION=(\{.*?\});/)?.[1] || "null");
if (!versions?.ditu?.normal) throw new Error("百度 SDK 缺少矢量版本配置");
const { version, updateDate } = versions.ditu.normal;
const sdkArray = (name) => JSON.parse(text.match(
    new RegExp(`\\.${name}=(\\[[\\d,]+\\])`))?.[1] || "null");
const levels = sdkArray("baseScaleZoom");
const spans = sdkArray("baseScaleZoomMercatorSize");
const sizes = sdkArray("baseScaleTileSize");
if (!Array.isArray(levels) || levels.some((z, i) =>
    grids[z]?.span !== spans?.[i] || grids[z]?.size !== sizes?.[i])) {
    throw new Error("百度 SDK 原生格网规则改变，需重新验证");
}
const metadata = { version, updateDate, coordinateSystem: "BD09",
    sdkSha256: crypto.createHash("sha256").update(sdk).digest("hex") };
const assets = new Map([
    ["/vendor/leaflet.js", ["https://unpkg.com/leaflet@1.9.4/dist/leaflet.js", "application/javascript"]],
    ["/vendor/leaflet.css", ["https://unpkg.com/leaflet@1.9.4/dist/leaflet.css", "text/css"]],
    ["/vendor/lucide.js", ["https://unpkg.com/lucide@0.468.0/dist/umd/lucide.min.js", "application/javascript"]],
]);
for (const [route, [url]] of assets) {
    await remote(url, path.join(cacheRoot, "vendor", path.basename(route)));
}
const inFlight = new Map();
async function tileBytes(kind, tile) {
    if (kind === "vector") validateTile(tile);
    else if (!Number.isInteger(tile.z) || tile.z < 3 || tile.z > 19 ||
        !Number.isInteger(tile.x) || !Number.isInteger(tile.y) ||
        Math.abs(tile.x) > 200000 || Math.abs(tile.y) > 200000) {
        throw new Error("底图瓦片索引无效");
    }
    const key = `${kind}/v${version}-${updateDate}/${tile.z}/${tile.x}/${tile.y}`;
    if (inFlight.has(key)) return inFlight.get(key);
    const request = (async () => {
        let url;
        if (kind === "vector") {
            const params = `x=${tile.x}&y=${tile.y}&z=${tile.z}&styles=pl&textimg=0` +
                `&v=${version}&udt=${updateDate}&json=0`;
            url = "https://maponline0.bdimg.com/pvd/?qt=vtile&param=" +
                encodeURIComponent(encodeParameters(params));
        } else {
            const x = tile.x < 0 ? `M${-tile.x}` : tile.x;
            const sourceY = -tile.y - 1;
            const y = sourceY < 0 ? `M${-sourceY}` : sourceY;
            url = `https://maponline0.bdimg.com/tile/?qt=vtile&x=${x}&y=${y}` +
                `&z=${tile.z}&styles=pl&scaler=1`;
        }
        const bytes = await remote(url, path.join(cacheRoot, key + ".bin"), (buffer) => {
            if (kind === "vector") normalizeTile(buffer.buffer.slice(
                buffer.byteOffset, buffer.byteOffset + buffer.byteLength), tile);
            else if (buffer[0] !== 137 || buffer[1] !== 80) throw new Error("百度底图不是 PNG");
        });
        return bytes;
    })();
    inFlight.set(key, request);
    try { return await request; } finally { inFlight.delete(key); }
}
const staticFiles = new Set(["index.html", "app.mjs", "protocol.mjs"]);
const server = http.createServer(async (request, response) => {
    try {
        if (request.method !== "GET") { response.writeHead(405).end(); return; }
        const url = new URL(request.url, "http://127.0.0.1");
        response.setHeader("Cache-Control", "no-store");
        if (url.pathname === "/favicon.ico") { response.writeHead(204).end(); return; }
        if (url.pathname === "/api/meta") {
            response.setHeader("Content-Type", "application/json; charset=utf-8");
            response.end(JSON.stringify(metadata));
            return;
        }
        const match = url.pathname.match(/^\/api\/(vector|electronic)\/(\d+)\/(-?\d+)\/(-?\d+)$/);
        if (match) {
            const tile = { z: Number(match[2]), x: Number(match[3]), y: Number(match[4]) };
            response.setHeader("Content-Type", match[1] === "vector" ?
                "application/octet-stream" : "image/png");
            response.end(await tileBytes(match[1], tile));
            return;
        }
        if (assets.has(url.pathname)) {
            response.setHeader("Content-Type", assets.get(url.pathname)[1]);
            response.end(await fs.readFile(path.join(cacheRoot, "vendor", path.basename(url.pathname))));
            return;
        }
        if (url.pathname === "/style.css") {
            response.setHeader("Content-Type", "text/css; charset=utf-8");
            response.end(await fs.readFile(path.join(root, "../tencent-gis-prototype/style.css")));
            return;
        }
        const name = url.pathname === "/" ? "index.html" : url.pathname.slice(1);
        if (!staticFiles.has(name)) { response.writeHead(404).end(); return; }
        response.setHeader("Content-Type", name.endsWith(".html") ?
            "text/html; charset=utf-8" : "application/javascript; charset=utf-8");
        response.end(await fs.readFile(path.join(root, name)));
    } catch (error) {
        response.writeHead(502, { "Content-Type": "application/json; charset=utf-8" });
        response.end(JSON.stringify({ error: error.message }));
    }
});
server.listen(port, "127.0.0.1", () => {
    console.log(`Baidu GIS prototype: http://127.0.0.1:${port}/`);
    console.log(`Vector ${version} / ${updateDate}; cache ${cacheRoot}`);
});
