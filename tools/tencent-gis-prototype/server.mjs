import http from "node:http";
import path from "node:path";
import fs from "node:fs/promises";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import { validateTile, tmsY } from "./protocol.mjs";

const root = path.dirname(fileURLToPath(import.meta.url));
const workspace = path.resolve(root, "../..");
const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    args.set(process.argv[index], process.argv[index + 1]);
}
const cacheRoot = path.resolve(args.get("--cache") || path.join(workspace, "tmp/tencent-gis-prototype"));
const port = Number(args.get("--port") || 8776);
await fs.mkdir(cacheRoot, { recursive: true });

async function remote(url, target) {
    try { return await fs.readFile(target); } catch (error) {
        if (error.code !== "ENOENT") throw error;
    }
    const response = await fetch(url, { signal: AbortSignal.timeout(20000) });
    if (!response.ok) throw new Error(`上游 HTTP ${response.status}: ${new URL(url).hostname}`);
    const buffer = Buffer.from(await response.arrayBuffer());
    await fs.mkdir(path.dirname(target), { recursive: true });
    await fs.writeFile(target, buffer);
    return buffer;
}

// 缓存只落在 tmp；不把官方 SDK、样式和真实地图数据提交到仓库。
const sdkBytes = await remote("https://map.qq.com/api/gljs?v=1.exp", path.join(cacheRoot, "sdk.js"));
const sdk = sdkBytes.toString("utf8");
const marker = "var n={regionTag:";
const start = sdk.indexOf(marker) + "var n=".length;
if (start < marker.length) throw new Error("官方 SDK 中找不到分类表，暂不支持此版本");
let end = start;
let depth = 0;
for (; end < sdk.length; ++end) {
    if (sdk[end] === "{") depth++;
    if (sdk[end] === "}" && --depth === 0) break;
}
const tags = JSON.parse(sdk.slice(start, end + 1).replace(/([A-Za-z_$][\w$]*):/g, '"$1":'));
const version = Number(sdk.match(/dataVersion:\{sat:\d+,tile:(\d+)/)?.[1]);
if (!Number.isInteger(version)) throw new Error("官方 SDK 缺少矢量版本配置");
const styleConfig = JSON.parse(await remote(
    "https://vectorsdk.map.qq.com/fileupdate/jsapi/style?autoup=1&id=0&version=-1",
    path.join(cacheRoot, "style-config.json")));
const styleUrl = new URL(styleConfig.url);
if (styleUrl.protocol !== "https:" ||
    !["mapstyle.qpic.cn", "vectorsdk.map.qq.com"].includes(styleUrl.hostname)) {
    throw new Error("腾讯样式地址不在预期域名范围");
}
const style = JSON.parse(await remote(styleUrl.href, path.join(cacheRoot, "style.json")));
const metadata = { version, sdkSha256: crypto.createHash("sha256").update(sdkBytes).digest("hex"),
    styleVersion: styleConfig.ver, tags, styles: style.data };
const assets = new Map([
    ["/vendor/leaflet.js", ["https://unpkg.com/leaflet@1.9.4/dist/leaflet.js", "application/javascript"]],
    ["/vendor/leaflet.css", ["https://unpkg.com/leaflet@1.9.4/dist/leaflet.css", "text/css"]],
    ["/vendor/lucide.js", ["https://unpkg.com/lucide@0.468.0/dist/umd/lucide.min.js",
        "application/javascript"]],
]);
await Promise.all([...assets].map(async ([route, [url]]) =>
    remote(url, path.join(cacheRoot, "vendor", path.basename(route)))));
const mime = { ".html": "text/html; charset=utf-8", ".mjs": "application/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8" };
const staticFiles = new Set(["index.html", "app.mjs", "protocol.mjs", "style.css"]);
const requests = [];
const inFlight = new Map();
async function tileBytes(kind, tile, compress) {
    const key = `${kind}/${version}/${compress}/${tile.z}/${tile.x}/${tile.y}`;
    if (inFlight.has(key)) return inFlight.get(key);
    const request = (async () => {
        const server = (tile.x + tile.y) % 4;
        const y = tmsY(tile);
        let url;
        if (kind === "vector") {
            url = `https://rt${server}.map.gtimg.com/tile?styleid=0&z=${tile.z}&x=${tile.x}` +
                `&y=${y}&version=${version}&building=1&style=100&compress=${compress}&mapType=hybrid`;
        } else if (kind === "electronic") {
            url = `https://rt${server}.map.gtimg.com/tile?z=${tile.z}&x=${tile.x}&y=${y}` +
                "&styleid=0&scene=0&version=114";
        } else {
            url = `https://p${server}.map.gtimg.com/sateTiles/${tile.z}/` +
                `${Math.floor(tile.x / 16)}/${Math.floor(y / 16)}/${tile.x}_${y}.png`;
        }
        const target = path.join(cacheRoot, `${key}.bin`);
        const bytes = await remote(url, target);
        const jpeg = bytes[0] === 255 && bytes[1] === 216;
        const png = bytes[0] === 137 && bytes[1] === 80 && bytes[2] === 78;
        if (kind === "vector") {
            const envelope = JSON.parse(bytes.toString("utf8"));
            if (!envelope || (!("ret" in envelope) && typeof envelope.data !== "string")) {
                await fs.rm(target, { force: true });
                throw new Error("腾讯矢量响应格式改变");
            }
        } else if (!jpeg && !png) {
            await fs.rm(target, { force: true });
            throw new Error("底图响应不是 PNG 或 JPEG");
        }
        requests.push({ kind, ...tile, tmsY: y, compress, bytes: bytes.length });
        if (requests.length > 300) requests.shift();
        return { bytes, contentType: kind === "vector" ? "application/json" :
            jpeg ? "image/jpeg" : "image/png" };
    })();
    inFlight.set(key, request);
    try { return await request; } finally { inFlight.delete(key); }
}

const server = http.createServer(async (request, response) => {
    try {
        const url = new URL(request.url, "http://127.0.0.1");
        if (request.method !== "GET") {
            response.writeHead(405).end();
            return;
        }
        if (url.pathname === "/favicon.ico") {
            response.writeHead(204).end();
            return;
        }
        response.setHeader("Cache-Control", "no-store");
        if (url.pathname === "/api/meta" || url.pathname === "/api/requests") {
            response.setHeader("Content-Type", "application/json; charset=utf-8");
            response.end(JSON.stringify(url.pathname === "/api/meta" ? metadata : requests));
            return;
        }
        const match = url.pathname.match(/^\/api\/(vector|electronic|satellite)\/(\d+)\/(\d+)\/(\d+)$/);
        if (match) {
            const tile = validateTile({ z: Number(match[2]), x: Number(match[3]), y: Number(match[4]) });
            const compress = url.searchParams.get("compress") === "0" ? 0 : 1;
            const { bytes, contentType } = await tileBytes(match[1], tile, compress);
            response.setHeader("Content-Type", contentType);
            response.end(bytes);
            return;
        }
        if (assets.has(url.pathname)) {
            response.setHeader("Content-Type", assets.get(url.pathname)[1]);
            response.end(await fs.readFile(path.join(cacheRoot, "vendor", path.basename(url.pathname))));
            return;
        }
        const name = url.pathname === "/" ? "index.html" : url.pathname.slice(1);
        if (!staticFiles.has(name)) {
            response.writeHead(404).end("Not found");
            return;
        }
        response.setHeader("Content-Type", mime[path.extname(name)]);
        response.end(await fs.readFile(path.join(root, name)));
    } catch (error) {
        response.writeHead(502, { "Content-Type": "application/json; charset=utf-8" });
        response.end(JSON.stringify({ error: error.message }));
    }
});
server.listen(port, "127.0.0.1", () => {
    console.log(`Tencent GIS prototype: http://127.0.0.1:${port}/`);
    console.log(`Vector version ${version}; cache ${cacheRoot}`);
});
