export const semanticCategories = {
    building: { name: "建筑", color: "#c55374" },
    water: { name: "水体", color: "#218dce" },
    green: { name: "绿地", color: "#35975e" },
    road: { name: "道路", color: "#cc8a16" },
    highway: { name: "高速", color: "#e55320" },
    railway: { name: "铁路 / 高铁", color: "#657078" },
    subway: { name: "地铁 / 设施", color: "#8954b4" },
    boundary: { name: "边界", color: "#935a38" },
    scenic: { name: "景点区域", color: "#688d32" },
    education: { name: "教育区域", color: "#549c9b" },
    medical: { name: "医疗区域", color: "#bc6676" },
    estate: { name: "地产小区", color: "#8c819c" },
    entertainment: { name: "休闲娱乐", color: "#b8843f" },
    transportation: { name: "交通设施", color: "#568aad" },
    manmade: { name: "其他人造区域", color: "#9a987b" },
    roadFacility: { name: "道路设施 / 标线", color: "#9d7547" },
    point: { name: "地点 / 文字锚点", color: "#3a7fbb" },
    other: { name: "其他已识别类型", color: "#7d8f85" },
    unknown: { name: "未分类", color: "#898989" },
};

const categoriesByType = new Map();
for (const [category, types] of Object.entries({
    building: ["BUILDING"],
    water: ["WATER", "OCEAN", "WATEROTHER"],
    green: ["GREEN"],
    road: ["ROAD", "NATIONALWAY", "PROVINCIALWAY", "CITYHIGHWAY", "ARTERIAL",
        "TERTIARYWAY", "FOURLEVELWAY", "LOCAL", "SCENICSPOTSWAY", "UNIVERSITYWAY", "VACATIONWAY"],
    highway: ["HIGHWAY"],
    railway: ["RAILWAY", "RAILROAD", "HIGHRAILWAY"],
    subway: ["SUBWAY", "SUBWAYSTATION"],
    boundary: ["BOUNDARY", "ADMINISTRATIVE"],
    scenic: ["SCENICSPOTS"],
    education: ["EDUCATION"],
    medical: ["MEDICAL"],
    estate: ["ESTATE"],
    entertainment: ["ENTERTAINMENT"],
    transportation: ["TRANSPORTATION", "PARKINGLOT"],
    manmade: ["MANMADE", "AOIOTHER", "SHOPPING", "PLAYGROUND"],
    roadFacility: ["FOOTBRIDGE", "CROSSWALK", "UNDERPASS", "PARKINGSPACE",
        "LANELINE", "DIVIDER", "CURB", "GUARDRAIL"],
})) {
    for (const type of types) categoriesByType.set(type, category);
}

export function decodeFeatureTypes(text, ids) {
    const match = text.match(/^baiduFeatureTypes\(([\s\S]*)\)\s*;?\s*$/);
    if (!match) throw new Error("百度分类接口响应无效");
    const values = JSON.parse(match[1]);
    if (!Array.isArray(values) || values.length !== ids.length ||
        values.some((value) => typeof value !== "string" || !/^[A-Z0-9_]*$/.test(value))) {
        throw new Error("百度分类编号与返回类型未一一对应");
    }
    return Object.fromEntries(ids.map((id, index) => [id, values[index]]));
}

// v088 道路面：SDK 分组、默认样式及四城跨层级道路中心线已交叉核对。
const verifiedRoadSurfaces = new Set([504812, 504813]);

export function classifyFeatures(features, types, metadata = {}) {
    return features.map((feature) => {
        const p = feature.properties;
        const geometryCategory = p.geometryCategory || p.category;
        if (!Object.hasOwn(types, p.styleId)) throw new Error(`缺少百度样式 ${p.styleId} 的分类结果`);
        const featureType = types[p.styleId];
        const officialKnown = Boolean(featureType && featureType !== "UNCLASSIFIED");
        const roadSurface = !officialKnown && p.provider === "baidu" &&
            metadata.version === "088" && metadata.updateDate === "20261021" &&
            p.tile?.z === 19 && p.groupId === 15 && verifiedRoadSurfaces.has(p.styleId);
        const known = officialKnown || roadSurface;
        const resolvedFeatureType = roadSurface ? "ROAD" : featureType;
        let category = known ? categoriesByType.get(resolvedFeatureType) || "other" : "unknown";
        // 水体名称、建筑名称等点锚点仍是点，不能混入可导出的面和线。
        if (geometryCategory === "point") category = "point";
        else if (geometryCategory === "building") category = "building";
        return { ...feature, properties: { ...p, geometryCategory, featureType, resolvedFeatureType, category,
            classificationKnown: known,
            classificationSource: officialKnown ? "official-feature-type" :
                roadSurface ? "verified-road-surface" :
                geometryCategory === "building" ? "geometry-group" : "unmapped" } };
    });
}
