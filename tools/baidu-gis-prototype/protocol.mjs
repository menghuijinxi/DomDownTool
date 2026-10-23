// 独立协议验证；坐标保持 BD09，正式程序接入前需另做 WGS84 转换。
export const categories = {
    building: { name: "建筑", color: "#c55374" },
    polygon: { name: "地表面", color: "#55a06b" },
    overlay: { name: "叠加面", color: "#bd8e30" },
    line: { name: "地表线", color: "#d17828" },
    elevated: { name: "立体线", color: "#8363b3" },
    point: { name: "地点 / 文字锚点", color: "#3a7fbb" },
};
export const grids = {
    3: { size: 256, span: 8388608, start: 3, end: 3 },
    5: { size: 512, span: 4194304, start: 4, end: 5 },
    7: { size: 512, span: 1048576, start: 6, end: 7 },
    9: { size: 512, span: 262144, start: 8, end: 9 },
    10: { size: 256, span: 65536, start: 10, end: 10 },
    12: { size: 512, span: 32768, start: 11, end: 13 },
    15: { size: 512, span: 4096, start: 14, end: 15 },
    17: { size: 1024, span: 2048, start: 16, end: 17 },
    19: { size: 1024, span: 512, start: 18, end: 21 },
};
const forwardBands = [75, 60, 45, 30, 15, 0];
const inverseBands = [12890594.86, 8362377.87, 5591021, 3481989.83, 1678043.12, 0];
const forward = [
    [-.0015702102444,111320.7020616939,1704480524535203,-10338987376042340,26112667856603880,-35149669176653700,26595700718403920,-10725012454188240,1800819912950474,82.5],
    [.0008277824516172526,111320.7020463578,647795574.6671607,-4082003173.641316,10774905663.51142,-15171875531.51559,12053065338.62167,-5124939663.577472,913311935.9512032,67.5],
    [.00337398766765,111320.7020202162,4481351.045890365,-23393751.19931662,79682215.47186455,-115964993.2797253,97236711.15602145,-43661946.33752821,8477230.501135234,52.5],
    [.00220636496208,111320.7020209128,51751.86112841131,3796837.749470245,992013.7397791013,-1221952.21711287,1340652.697009075,-620943.6990984312,144416.9293806241,37.5],
    [-.0003441963504368392,111320.7020576856,278.2353980772752,2485758.690035394,6070.750963243378,54821.18345352118,9540.606633304236,-2710.55326746645,1405.483844121726,22.5],
    [-.0003218135878613132,111320.7020701615,.00369383431289,823725.6402795718,.46104986909093,2351.343141331292,1.58060784298199,8.77738589078284,.37238884252424,7.45],
];
const inverse = [
    [1.410526172116255e-8,.00000898305509648872,-1.9939833816331,200.9824383106796,-187.2403703815547,91.6087516669843,-23.38765649603339,2.57121317296198,-.03801003308653,17337981.2],
    [-7.435856389565537e-9,.000008983055097726239,-.78625201886289,96.32687599759846,-1.85204757529826,-59.36935905485877,47.40033549296737,-16.50741931063887,2.28786674699375,10260144.86],
    [-3.030883460898826e-8,.00000898305509983578,.30071316287616,59.74293618442277,7.357984074871,-25.38371002664745,13.45380521110908,-3.29883767235584,.32710905363475,6856817.37],
    [-1.981981304930552e-8,.000008983055099779535,.03278182852591,40.31678527705744,.65659298677277,-4.44255534477492,.85341911805263,.12923347998204,-.04625736007561,4482777.06],
    [3.09191371068437e-9,.000008983055096812155,.00006995724062,23.10934304144901,-.00023663490511,-.6321817810242,-.00663494467273,.03430082397953,-.00466043876332,2555164.4],
    [2.890871144776878e-9,.000008983055095805407,-3.068298e-8,7.47137025468032,-.00000353937994,-.02145144861037,-.00001234426596,.00010322952773,-.00000323890364,826088.5],
];
function polynomial(x, y, c) {
    const b = Math.abs(y) / c[9];
    return [(c[0] + c[1] * Math.abs(x)) * Math.sign(x),
        c.slice(2, 9).reduce((sum, value, i) => sum + value * b ** i, 0) * Math.sign(y)];
}
export function toMercator(lon, lat) {
    if (!Number.isFinite(lon) || !Number.isFinite(lat) || Math.abs(lon) > 180 || Math.abs(lat) > 74) {
        throw new Error("BD09 经纬度无效");
    }
    return polynomial(lon, lat, forward[forwardBands.findIndex((v) => Math.abs(lat) >= v)]);
}
export function fromMercator(x, y) {
    return polynomial(x, y, inverse[inverseBands.findIndex((v) => Math.abs(y) >= v)]);
}
export function validateTile(tile) {
    if (!grids[tile.z] || !Number.isInteger(tile.x) || !Number.isInteger(tile.y) ||
        Math.abs(tile.x * grids[tile.z].span) > 30000000 ||
        Math.abs(tile.y * grids[tile.z].span) > 20000000) {
        throw new Error("百度矢量格网无效，需使用原生数据层级");
    }
    return tile;
}
export function tileAt(lon, lat, z) {
    const [x, y] = toMercator(lon, lat);
    if (!grids[z]) throw new Error("暂不支持该百度数据层级");
    return validateTile({ x: Math.floor(x / grids[z].span), y: Math.floor(y / grids[z].span), z });
}
export function tileBounds(tile) {
    validateTile(tile);
    const span = grids[tile.z].span;
    const [west, south] = fromMercator(tile.x * span, tile.y * span);
    const [east, north] = fromMercator((tile.x + 1) * span, (tile.y + 1) * span);
    return [[south, west], [north, east]];
}
export function encodeParameters(text) {
    let bits = [...text].map((c) => (c.charCodeAt(0) << 1).toString(2).padStart(8, "0")).join("");
    const padding = 5 - bits.length % 5;
    bits = "0".repeat(padding) + bits;
    return bits.match(/.{5}/g).map((v) => String.fromCharCode(parseInt(v, 2) + 50)).join("") + padding;
}
export function coordinatePath(values) {
    if (!values.length || values.length % 2 || !values.every(Number.isFinite)) {
        throw new Error("百度差分坐标长度无效");
    }
    const points = [];
    let x = 0, y = 0;
    for (let i = 0; i < values.length; i += 2) {
        x += values[i] / 100;
        y += values[i + 1] / 100;
        points.push([x, y]);
    }
    return points;
}
export function normalizeTile(buffer, tile) {
    validateTile(tile);
    const view = new DataView(buffer);
    function unsigned(offset) {
        if (offset < 0 || offset % 4 || offset + 4 > view.byteLength) {
            throw new Error("百度二进制字段超出响应边界");
        }
        return view.getUint32(offset, true);
    }
    function children(block, header) {
        const [offset, length] = block;
        if (length === header * 4) return [];
        if (!Number.isInteger(header) || header < 0 || header > length / 4 - 1) {
            throw new Error("百度块头长度无效");
        }
        const count = unsigned(offset + header * 4);
        let start = offset + (header + 1 + count) * 4;
        if (start > offset + length) throw new Error("百度子块目录越界");
        const result = [];
        for (let i = 0; i < count; ++i) {
            const size = unsigned(offset + (header + 1 + i) * 4);
            if (start + size > offset + length) throw new Error("百度子块内容越界");
            result.push([start, size]);
            start += Math.ceil(size / 4) * 4;
        }
        return result;
    }
    function int32(block) {
        if (block[1] % 4) throw new Error("百度顶点数组不是 Int32");
        return Array.from({ length: block[1] / 4 }, (_, i) =>
            view.getInt32(block[0] + i * 4, true));
    }
    function uint16(block) {
        if (block[1] % 2) throw new Error("百度索引数组不是 Uint16");
        return Array.from({ length: block[1] / 2 }, (_, i) =>
            view.getUint16(block[0] + i * 2, true));
    }
    if (view.byteLength < 40 || unsigned(0) !== 2 || unsigned(4) !== 2) {
        throw new Error("暂不支持的百度响应版本或错误响应");
    }
    const sections = children([0, view.byteLength], unsigned(4) + 2);
    if (sections.length !== 5) throw new Error("暂不支持的百度顶层块结构");
    const features = [], groups = [], skipped = [];
    const categoryByGroup = { 3: "point", 4: "line", 7: "polygon", 8: "building",
        15: "overlay", 16: "elevated" };
    for (const group of children(sections[0], 0)) {
        const groupHeader = unsigned(group[0]);
        if (groupHeader !== 2) throw new Error("暂不支持的百度图层头");
        const groupId = unsigned(group[0] + 4);
        const order = unsigned(group[0] + 8);
        const category = categoryByGroup[groupId];
        groups.push({ groupId, order, bytes: group[1], supported: Boolean(category) });
        if (!category) {
            skipped.push({ groupId, order, bytes: group[1] });
            continue;
        }
        for (const style of children(group, groupHeader + 1)) {
            if (unsigned(style[0]) !== 1) throw new Error("暂不支持的百度样式头");
            const styleId = unsigned(style[0] + 4);
            for (const record of children(style, 2)) {
                if (groupId === 3) {
                    const header = unsigned(record[0]);
                    if (header !== 5) throw new Error("暂不支持的百度地点头");
                    const chunks = children(record, header + 1);
                    if (chunks.length < 2) throw new Error("百度地点缺少属性块");
                    const readString = ([offset, length]) => new TextDecoder("utf-8", { fatal: true })
                        .decode(new Uint8Array(buffer, offset, length));
                    const anchor = [unsigned(record[0] + 8) / 100,
                        unsigned(record[0] + 12) / 100];
                    const span = grids[tile.z].span;
                    features.push({ type: "Feature", geometry: { type: "Point",
                        coordinates: fromMercator(tile.x * span + Math.round(anchor[0]),
                            tile.y * span + Math.round(anchor[1])) }, properties: {
                        provider: "baidu", coordinateSystem: "BD09", category, groupId, order,
                        styleId, visibilityMask: unsigned(record[0] + 4),
                        name: readString(chunks[1]), guid: readString(chunks[0]),
                        rank: unsigned(record[0] + 16), direction: unsigned(record[0] + 20),
                        anchorMercator: anchor, tile: { ...tile },
                    } });
                    continue;
                }
                if (unsigned(record[0]) !== 2) throw new Error("暂不支持的百度要素头");
                const visibilityMask = unsigned(record[0] + 4);
                const rawValue = unsigned(record[0] + 8);
                const chunks = children(record, 3);
                if (chunks.length < 2) throw new Error("百度要素缺少几何块");
                const rawCoordinates = int32(chunks[0]);
                if (!rawCoordinates.length) continue;
                const localCoordinates = coordinatePath(rawCoordinates);
                const grid = grids[tile.z];
                const coordinates = localCoordinates.map(([x, y]) => fromMercator(
                    tile.x * grid.span + x * grid.span / grid.size,
                    tile.y * grid.span + y * grid.span / grid.size));
                let geometry, triangles = [];
                if ([7, 8, 15].includes(groupId)) {
                    const indexBlock = chunks[groupId === 8 ? 1 : 2];
                    if (!indexBlock) throw new Error("百度面缺少原始三角网格");
                    triangles = uint16(indexBlock);
                    if (triangles.length % 3) throw new Error("百度三角网格索引长度无效");
                    const geometries = [];
                    for (let i = 0; i < triangles.length; i += 3) {
                        const vertices = triangles.slice(i, i + 3).map((index) => coordinates[index]);
                        if (vertices.some((p) => !p)) throw new Error("百度三角网格索引超出顶点边界");
                        geometries.push({ type: "Polygon", coordinates: [[...vertices, vertices[0]]] });
                    }
                    geometry = geometries.length ? { type: "GeometryCollection", geometries } : null;
                } else {
                    geometry = coordinates.length >= 2 ? { type: "LineString", coordinates } : null;
                }
                features.push({ type: "Feature", geometry, properties: {
                    provider: "baidu", coordinateSystem: "BD09", category, groupId, order, styleId,
                    visibilityMask, rawValue, rawCoordinates, localCoordinates, triangles,
                    heightRaw: groupId === 8 ? rawValue : null,
                    tile: { ...tile },
                } });
            }
        }
    }
    return { tile, features, groups, skipped, textureHeight: unsigned(8),
        bytes: view.byteLength };
}
