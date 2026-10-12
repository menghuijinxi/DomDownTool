import http from "node:http";
import path from "node:path";
import fs from "node:fs";
import { fileURLToPath } from "node:url";

const options = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    options.set(process.argv[index], process.argv[index + 1]);
}
const webRoot = path.dirname(fileURLToPath(import.meta.url));
const dataRoot = path.resolve(options.get("--data") || ".");
const cesiumRoot = options.has("--cesium-build") ?
    path.resolve(options.get("--cesium-build")) : undefined;
const port = Number(options.get("--port") || 8765);
const types = {
    ".html": "text/html; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json",
    ".glb": "model/gltf-binary",
    ".terrain": "application/vnd.quantized-mesh",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".png": "image/png",
    ".ktx2": "image/ktx2",
    ".wasm": "application/wasm",
    ".svg": "image/svg+xml",
};
const recent = [];
const server = http.createServer(async (request, response) => {
    response.setHeader("Access-Control-Allow-Origin", "*");
    response.setHeader("Access-Control-Allow-Headers", "Accept, Range, Content-Type");
    if (request.method === "OPTIONS") {
        response.writeHead(204).end();
        return;
    }
    let url;
    let relative;
    try {
        url = new URL(request.url, "http://localhost");
        relative = decodeURIComponent(url.pathname.startsWith("/data/") ?
            url.pathname.slice(6) : cesiumRoot && url.pathname.startsWith("/cesium/") ?
                url.pathname.slice(8) : url.pathname === "/" ? "index.html" :
                url.pathname.slice(1));
    } catch {
        response.writeHead(400).end("Invalid path");
        return;
    }
    if (url.pathname === "/requests") {
        response.setHeader("Content-Type", "application/json");
        response.end(JSON.stringify(recent));
        return;
    }
    const isData = url.pathname.startsWith("/data/");
    const isCesium = cesiumRoot && url.pathname.startsWith("/cesium/");
    const root = isData ? dataRoot : isCesium ? cesiumRoot : webRoot;
    const target = path.resolve(root, relative);
    if (!target.startsWith(root + path.sep) || !["GET", "HEAD"].includes(request.method)) {
        response.writeHead(403).end();
        return;
    }
    try {
        const realRoot = await fs.promises.realpath(root);
        const realTarget = await fs.promises.realpath(target);
        if (!realTarget.startsWith(realRoot + path.sep)) {
            response.writeHead(403).end();
            return;
        }
        const stat = await fs.promises.stat(realTarget);
        if (!stat.isFile()) {
            response.writeHead(404).end();
            return;
        }
        const extension = path.extname(realTarget).toLowerCase();
        response.setHeader("Content-Type", types[extension] || "application/octet-stream");
        response.setHeader("Cache-Control", "no-cache");
        if (extension === ".terrain") {
            response.setHeader("Content-Encoding", "gzip");
        }
        if (cesiumRoot && !isData && !isCesium &&
            realTarget === path.join(realRoot, "index.html")) {
            const html = (await fs.promises.readFile(realTarget, "utf8")).replaceAll(
                "https://cdn.jsdelivr.net/npm/cesium@1.146.0/Build/Cesium/", "/cesium/");
            response.setHeader("Content-Length", Buffer.byteLength(html));
            response.end(request.method === "HEAD" ? undefined : html);
            return;
        }
        response.setHeader("Content-Length", stat.size);
        recent.push({ path: url.pathname, bytes: stat.size, time: Date.now() });
        if (recent.length > 1000) recent.shift();
        if (request.method === "HEAD") response.end();
        else {
            const stream = fs.createReadStream(realTarget);
            stream.on("error", () => response.destroy());
            response.on("close", () => stream.destroy());
            stream.pipe(response);
        }
    } catch {
        recent.push({ path: url.pathname, status: 404, time: Date.now() });
        response.writeHead(404).end("Not found");
    }
});
server.listen(port, "127.0.0.1", () => {
    console.log(`Cesium preview: http://127.0.0.1:${port}/`);
    console.log(`Data directory: ${dataRoot}`);
    if (cesiumRoot) console.log(`Local Cesium build: ${cesiumRoot}`);
});
