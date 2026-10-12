import fs from "node:fs/promises";
import path from "node:path";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const playwright = require(process.env.PLAYWRIGHT_MODULE || "playwright");
const address = process.argv[2] || "http://127.0.0.1:8765/?autoload=1";
const output = path.resolve(process.argv[3] || "tmp/cesium-browser-check");
await fs.mkdir(output, { recursive: true });
const browser = await playwright.chromium.launch({
    headless: true,
    channel: process.env.PLAYWRIGHT_CHANNEL || "msedge",
    proxy: process.env.BROWSER_PROXY ? { server: process.env.BROWSER_PROXY,
        bypass: "127.0.0.1,localhost" } : undefined,
    args: ["--enable-webgl", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"],
});
const results = [];
try {
    for (const viewport of [{ width: 1440, height: 900 }, { width: 390, height: 844 }]) {
        const page = await browser.newPage({ viewport });
        const errors = [];
        const requestFailures = [];
        const ktx2Responses = [];
        page.on("response", (response) => {
            if (new URL(response.url()).pathname.endsWith(".ktx2")) {
                ktx2Responses.push({
                    url: response.url(), status: response.status(),
                    contentType: response.headers()["content-type"],
                });
            }
        });
        page.on("requestfailed", (request) => requestFailures.push({
            url: request.url(), error: request.failure()?.errorText,
        }));
        page.on("pageerror", (error) => errors.push(String(error)));
        page.on("console", (message) => {
            if (message.type() === "error") errors.push(message.text());
        });
        await page.goto(address, { waitUntil: "domcontentloaded" });
        await page.waitForFunction(() => window.cesiumPreview?.ready, null, {
            timeout: 60000,
        });
        await page.evaluate(() => {
            document.querySelector("#sse").value = "1";
            document.querySelector("#sse").dispatchEvent(new Event("input"));
        });
        await page.waitForFunction(() => {
            const preview = window.cesiumPreview;
            const levels = preview.viewer.scene.globe._surface._tilesToRender;
            return preview.buildings.tilesLoaded &&
                preview.selections.some((tile) => tile.error === 0) &&
                levels.some((tile) => tile.level >= 5);
        }, null, { timeout: 120000 });
        await page.waitForTimeout(2000);
        await page.screenshot({ path: path.join(output, `${viewport.width}-near.png`) });
        const near = await page.evaluate(() => ({
            status: document.querySelector("#status").textContent,
            errors: window.cesiumPreview.errors,
            selections: window.cesiumPreview.selections,
            loaded: window.cesiumPreview.buildings.tilesLoaded,
            terrainLevels: window.cesiumPreview.viewer.scene.globe._surface._tilesToRender
                .map((tile) => tile.level),
        }));
        const pixels = await page.evaluate(async () => {
            const canvas = document.querySelector("#scene canvas");
            const blob = await new Promise((resolve) => canvas.toBlob(resolve));
            const image = await createImageBitmap(blob);
            const sample = new OffscreenCanvas(32, 32);
            const context = sample.getContext("2d");
            context.drawImage(image, 0, 0, 32, 32);
            const rgba = context.getImageData(0, 0, 32, 32).data;
            return new Set(Array.from({ length: 1024 }, (_, index) =>
                `${rgba[index * 4]},${rgba[index * 4 + 1]},${rgba[index * 4 + 2]}`)).size;
        });
        await page.evaluate(() => {
            window.cesiumPreview.selections.length = 0;
            document.querySelector("#sse").value = "8";
            document.querySelector("#sse").dispatchEvent(new Event("input"));
            window.cesiumPreview.viewer.camera.moveBackward(12000);
        });
        await page.waitForFunction(() => window.cesiumPreview.selections.some(
            (tile) => tile.error > 0), null, { timeout: 90000 });
        await page.waitForTimeout(2000);
        await page.screenshot({ path: path.join(output, `${viewport.width}-far.png`) });
        const far = await page.evaluate(() => window.cesiumPreview.selections);
        await page.selectOption("#provider", "arcgis");
        await page.waitForTimeout(10000);
        await page.screenshot({ path: path.join(output, `${viewport.width}-online.png`) });
        results.push({
            viewport, near, far, errors, requestFailures, ktx2Responses, canvasColors: pixels,
        });
        await page.close();
    }
    await fs.writeFile(path.join(output, "browser-report.json"), JSON.stringify(results, null, 2));
    console.log(JSON.stringify(results.map((entry) => ({
        viewport: entry.viewport,
        status: entry.near.status,
        errors: [...new Set([...entry.errors, ...entry.near.errors])],
        requestFailures: entry.requestFailures,
        ktx2Count: entry.ktx2Responses.length,
        canvasColors: entry.canvasColors,
        nearErrors: [...new Set(entry.near.selections.map((tile) => tile.error))],
        farErrors: [...new Set(entry.far.map((tile) => tile.error))],
        terrainLevels: [...new Set(entry.near.terrainLevels)],
    })), null, 2));
    if (results.some((entry) => entry.canvasColors < 8 || !entry.near.loaded ||
        entry.ktx2Responses.length === 0 ||
        entry.ktx2Responses.some((response) =>
            response.status !== 200 || response.contentType !== "image/ktx2") ||
        Math.max(...entry.near.terrainLevels) < 5 ||
        !entry.near.selections.some((tile) => tile.error === 0) ||
        !entry.far.some((tile) => tile.error > 0) ||
        entry.near.errors.length > 0 || entry.errors.length > 0)) {
        process.exitCode = 1;
    }
} finally {
    await browser.close();
}
