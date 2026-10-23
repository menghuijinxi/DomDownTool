// 前端协议验证模块；验证完成后作为 C++ 实现的对照材料。
export const categories = {
    building: { name: "建筑", color: "#d24e67" },
    road: { name: "道路", color: "#cc8a16" },
    highway: { name: "高速", color: "#e55320" },
    subway: { name: "地铁", color: "#8c57bb" },
    water: { name: "水体", color: "#218dce" },
    green: { name: "绿地", color: "#35975e" },
    boundary: { name: "边界", color: "#935a38" },
    point: { name: "地点", color: "#377a9c" },
    label: { name: "文字", color: "#58626c" },
    other: { name: "其他", color: "#84949c" },
};

const groups = { 65536: "point", 131072: "line", 196608: "region",
    262144: "label", 327680: "arrow" };

export function decodeEnvelope(envelope) {
    if (!envelope || typeof envelope !== "object" || Array.isArray(envelope)) {
        throw new Error("腾讯响应必须为单瓦片 JSON 对象");
    }
    let decoded = envelope;
    if (!("ret" in envelope)) {
        if (typeof envelope.data !== "string" ||
            !/^[A-Za-z0-9+/]*={0,2}$/.test(envelope.data) || envelope.data.length === 0) {
            throw new Error("腾讯响应缺少有效的 Base64 data");
        }
        const binary = atob(envelope.data);
        const bytes = new Uint8Array(Math.ceil(binary.length / 2) * 2);
        const rotate = (value, bits) => ((value >> (8 - bits)) | (value << bits)) & 255;
        for (let offset = 0; offset < binary.length; offset += 2) {
            let left = binary.charCodeAt(offset);
            let right = offset + 1 < binary.length ? binary.charCodeAt(offset + 1) : 0;
            for (let round = 0; round < 3; ++round) {
                [left, right] = [right, left ^
                    (rotate(right, 2) ^ (rotate(right, 1) & rotate(right, 3)))];
            }
            bytes[offset] = left;
            bytes[offset + 1] = right;
        }
        const text = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
        const end = text.lastIndexOf("}");
        if (end < 0) throw new Error("腾讯 data 还原后不是 JSON");
        decoded = JSON.parse(text.slice(0, end + 1));
    }
    if (!Number.isInteger(decoded.ret) || decoded.ret !== 0) {
        throw new Error(`腾讯服务返回错误 ret=${decoded.ret ?? "缺失"}`);
    }
    if (!Array.isArray(decoded.features)) {
        throw new Error("腾讯响应缺少 features 数组");
    }
    return decoded;
}

export function validateTile(tile) {
    if (!Number.isInteger(tile.z) || tile.z < 3 || tile.z > 18 ||
        !Number.isInteger(tile.x) || !Number.isInteger(tile.y) ||
        tile.x < 0 || tile.y < 0 || tile.x >= 2 ** tile.z || tile.y >= 2 ** tile.z) {
        throw new Error("腾讯矢量瓦片坐标无效，数据层级暂支持 z3–18");
    }
    return tile;
}

export function tmsY(tile) {
    validateTile(tile);
    return 2 ** tile.z - 1 - tile.y;
}

export function tileAt(lon, lat, z) {
    const scale = 2 ** z;
    const radians = lat * Math.PI / 180;
    return validateTile({
        x: Math.floor((lon + 180) / 360 * scale),
        y: Math.floor((1 - Math.asinh(Math.tan(radians)) / Math.PI) / 2 * scale),
        z,
    });
}

export function localToLonLat(tile, point) {
    const scale = 2 ** tile.z;
    return [
        (tile.x + point[0] / 256) / scale * 360 - 180,
        Math.atan(Math.sinh(Math.PI * (1 - 2 * (tile.y + point[1] / 256) / scale))) *
            180 / Math.PI,
    ];
}

export function tileBounds(tile) {
    const [west, north] = localToLonLat(tile, [0, 0]);
    const [east, south] = localToLonLat(tile, [256, 256]);
    return [[south, west], [north, east]];
}

export function coordinatePath(encoded, compress) {
    if (!Array.isArray(encoded) || encoded.length < 2 || encoded.length % 2 ||
        !encoded.every(Number.isFinite)) {
        throw new Error("腾讯几何坐标必须为成对的有限数值");
    }
    let x = 0;
    let y = 0;
    const points = [];
    for (let index = 0; index < encoded.length; index += 2) {
        x = compress ? x + encoded[index] : encoded[index];
        y = compress ? y + encoded[index + 1] : encoded[index + 1];
        points.push([x, y]);
    }
    return points;
}

export function layerInfo(id, metadata, zoom) {
    const kind = groups[id & 0xf0000];
    if (!kind) throw new Error(`暂不支持的腾讯几何图层 ${id}`);
    const tags = metadata.tags?.[`${kind}Tag`] || {};
    const tag = Object.keys(tags).find((key) => tags[key].includes(id)) || "unmapped";
    const styles = metadata.styles?.[`${kind}Styles`]?.[id] || [];
    const active = styles.filter((entry) =>
        entry.minScaleVal <= zoom && entry.maxScaleVal >= zoom).at(-1);
    let category = "other";
    // 几何类别优先，地点和文字不因名称匹配被误当成建筑面或道路中心线。
    if (kind === "point") category = "point";
    else if (kind === "label") category = "label";
    else if (kind === "region") {
        if (tag === "building") category = "building";
        else if (tag === "water") category = "water";
        else if (tag === "grass" || tag === "park") category = "green";
        else if (tag === "subwayarea") category = "subway";
    } else if (kind === "line") {
        if (tag === "highway" || tag === "motorway") category = "highway";
        else if (["mainroad", "secondaryroad", "street", "path"].includes(tag)) category = "road";
        else if (tag === "water") category = "water";
        else if (tag === "subwayline") category = "subway";
        else if (tag === "guojie" || tag === "shengjie") category = "boundary";
    }
    return { kind, tag, category, styleName: active?.stylename || styles[0]?.stylename || "",
        minZoom: styles.length ? Math.min(...styles.map((entry) => entry.minScaleVal)) : null,
        maxZoom: styles.length ? Math.max(...styles.map((entry) => entry.maxScaleVal)) : null,
        styleVisible: Boolean(active), knownStyle: styles.length > 0 };
}

export function normalizeTile(envelope, tile, metadata, compress = true) {
    validateTile(tile);
    const decoded = decodeEnvelope(envelope);
    const features = [];
    let groupedLabels = 0;
    const entries = [];
    for (const entry of decoded.features) {
        if (!Array.isArray(entry)) throw new Error("腾讯要素不是数组");
        if (Array.isArray(entry[0])) {
            groupedLabels++;
            entries.push(...entry);
        } else entries.push(entry);
    }
    // 扩展 POI 与基础 POI 同样保留，不能只读取 features 丢失补充地点。
    if (decoded.pois !== undefined && !Array.isArray(decoded.pois)) {
        throw new Error("腾讯 pois 格式不支持");
    }
    const all = entries.map((raw) => ({ raw, origin: "features" }));
    for (const raw of decoded.pois || []) all.push({ raw, origin: "pois" });
    for (const { raw, origin } of all) {
        if (!Array.isArray(raw) || !Number.isInteger(raw[0])) {
            throw new Error("腾讯要素缺少图层编号");
        }
        const info = layerInfo(raw[0], metadata, tile.z);
        const paths = Array.isArray(raw[1]?.[0]) ? raw[1] : [raw[1]];
        const local = paths.map((entry) => coordinatePath(entry, compress));
        const geographic = local.map((points) => points.map((p) => localToLonLat(tile, p)));
        let geometry;
        let geometryIssue = null;
        if (info.kind === "point" || info.kind === "label") {
            geometry = { type: "Point", coordinates: geographic[0][0] };
        } else if (info.kind === "region") {
            const degenerate = local.some((points) => points.length < 3 ||
                points.reduce((area, point, index) => {
                    const next = points[(index + 1) % points.length];
                    return area + point[0] * next[1] - next[0] * point[1];
                }, 0) === 0);
            const polygons = geographic.map((points) => {
                const ring = points.map((point) => [...point]);
                if (ring[0][0] !== ring.at(-1)[0] || ring[0][1] !== ring.at(-1)[1]) {
                    ring.push([...ring[0]]);
                }
                return [ring];
            });
            geometry = polygons.length === 1 ? { type: "Polygon", coordinates: polygons[0] } :
                { type: "MultiPolygon", coordinates: polygons };
            if (degenerate) {
                geometry = null;
                geometryIssue = "腾讯原始面路径退化，无法绘制有效面";
            }
        } else {
            if (geographic.some((points) => points.length < 2)) {
                geometry = null;
                geometryIssue = "腾讯原始线路径少于两个顶点";
            } else geometry = geographic.length === 1 ?
                { type: "LineString", coordinates: geographic[0] } :
                { type: "MultiLineString", coordinates: geographic };
        }
        const height = raw[0] === 196619 && typeof raw[2] === "number" ? raw[2] : null;
        features.push({
            type: "Feature",
            id: `${tile.z}/${tile.x}/${tile.y}/${origin}/${features.length}`,
            geometry,
            properties: {
                provider: "tencent", coordinateSystem: "GCJ-02", layerId: raw[0], ...info,
                heightMeters: height, geometryIssue, name: typeof raw[2] === "string" ? raw[2] : "",
                tile: `${tile.z}/${tile.x}/${tile.y}`, origin, localCoordinates: local,
                rawFields: raw.slice(2),
            },
        });
    }
    return { type: "FeatureCollection", coordinateSystem: "GCJ-02", tile,
        features, groupedLabels, baseFeatureCount: entries.length,
        extraPoiCount: (decoded.pois || []).length };
}

export function compareTiles(first, second) {
    const signature = (feature) => JSON.stringify([
        feature.properties.origin, feature.properties.layerId, feature.properties.localCoordinates,
        feature.properties.heightMeters, feature.properties.name, feature.properties.rawFields,
    ]);
    const left = first.features.map(signature).sort();
    const right = second.features.map(signature).sort();
    let differences = Math.abs(left.length - right.length);
    for (let index = 0; index < Math.min(left.length, right.length); ++index) {
        if (left[index] !== right[index]) differences++;
    }
    return { matched: differences === 0, differences,
        compressedFeatures: left.length, absoluteFeatures: right.length };
}
