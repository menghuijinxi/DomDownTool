import fs from "node:fs/promises";
import path from "node:path";
import assert from "node:assert/strict";
import { normalizeTile, grids } from "./protocol.mjs";

const summaryPath = process.argv[2] || "tmp/postprocess-tests/baidu-vector/prototype-summary.json";
const cacheRoot = process.argv[3] || "tmp/baidu-gis-prototype/vector/v088-20261021";
const summaries = JSON.parse(await fs.readFile(summaryPath, "utf8"));
let features = 0, vertices = 0, checkedVertices = 0, checkedPoints = 0;

function segmentDistance(point, a, b) {
    const dx = b[0] - a[0], dy = b[1] - a[1];
    const ratio = Math.max(0, Math.min(1,
        ((point[0] - a[0]) * dx + (point[1] - a[1]) * dy) / (dx * dx + dy * dy || 1)));
    return Math.hypot(point[0] - a[0] - ratio * dx, point[1] - a[1] - ratio * dy);
}

for (const summary of summaries) {
    const tile = { x: summary.x, y: summary.y, z: summary.z };
    const bytes = await fs.readFile(path.join(cacheRoot, String(tile.z),
        String(tile.x), `${tile.y}.bin`));
    const decoded = normalizeTile(bytes.buffer.slice(bytes.byteOffset,
        bytes.byteOffset + bytes.byteLength), tile);
    const grid = grids[tile.z];
    assert.equal(summary.features.length, decoded.features.length);
    for (let index = 0; index < decoded.features.length; ++index) {
        const feature = decoded.features[index], p = feature.properties;
        const actual = summary.features[index];
        assert.equal(actual.styleId, p.styleId);
        assert.equal(actual.groupId, p.groupId);
        assert.equal(actual.sourceVertexCount, p.localCoordinates?.length || 0);
        assert.equal(actual.empty, feature.geometry === null);
        if (p.anchorMercator) {
            const expected = [tile.x * grid.span + Math.round(p.anchorMercator[0]),
                tile.y * grid.span + Math.round(p.anchorMercator[1])];
            assert(Math.hypot(actual.geometryMercator[0][0][0] - expected[0],
                actual.geometryMercator[0][0][1] - expected[1]) < 0.05);
            checkedPoints++;
        }
        if (p.localCoordinates && actual.geometryMercator.length) {
            const source = p.localCoordinates.map(([x, y]) =>
                [x * grid.span / grid.size, y * grid.span / grid.size]);
            for (const ring of actual.geometryMercator) {
                for (const point of ring) {
                    const local = [point[0] - tile.x * grid.span, point[1] - tile.y * grid.span];
                    const distance = Math.min(...source.map(([x, y]) =>
                        Math.hypot(local[0] - x, local[1] - y)));
                    if (distance <= 0.05) {
                        checkedVertices++;
                        continue;
                    }
                    let edgeDistance = Infinity;
                    for (let triangle = 0; triangle < (p.triangles?.length || 0); triangle += 3) {
                        for (let edge = 0; edge < 3; ++edge) {
                            edgeDistance = Math.min(edgeDistance, segmentDistance(local,
                                source[p.triangles[triangle + edge]],
                                source[p.triangles[triangle + (edge + 1) % 3]]));
                        }
                    }
                    // 并集交点会取整到百分之一局部单位；容差随原生格网缩放。
                    assert(edgeDistance < 0.0072 * grid.span / grid.size + 0.05,
                        `${tile.z}/${tile.x}/${tile.y}, style ${p.styleId}: ${edgeDistance}`);
                    checkedVertices++;
                }
            }
        }
        vertices += actual.sourceVertexCount;
    }
    features += decoded.features.length;
}
assert(summaries.length >= 16 && features >= 36000);
console.log(JSON.stringify({ tiles: summaries.length, features, vertices,
    checkedVertices, checkedPoints, matched: true }));
