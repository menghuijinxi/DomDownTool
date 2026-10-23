import assert from "node:assert/strict";
import test from "node:test";
import { classifyFeatures, decodeFeatureTypes } from "./classification.mjs";

const source = { version: "088", updateDate: "20261021" };
function surface(styleId, properties = {}) {
    return { type: "Feature", geometry: { type: "GeometryCollection", geometries: [] },
        properties: { provider: "baidu", category: "overlay", groupId: 15, styleId,
            tile: { z: 19, x: 25314, y: 9417 }, ...properties } };
}

test("环形内部道路及同类道路面不再落入未分类，保留接口空值", () => {
    const features = [surface(504812), surface(504813)];
    const result = classifyFeatures(features, { 504812: "", 504813: "" }, source);
    for (const { properties: p } of result) {
        assert.equal(p.category, "road");
        assert.equal(p.featureType, "");
        assert.equal(p.resolvedFeatureType, "ROAD");
        assert.equal(p.classificationKnown, true);
        assert.equal(p.classificationSource, "verified-road-surface");
    }
    assert.equal(features[0].properties.category, "overlay");
});

test("补充规则限于已核对的版本、原生层级、几何组和编号", () => {
    const feature = surface(504812);
    const types = { 504812: "" };
    for (const metadata of [{}, { ...source, version: "089" },
        { ...source, updateDate: "20261022" }]) {
        assert.equal(classifyFeatures([feature], types, metadata)[0].properties.category, "unknown");
    }
    for (const properties of [{ groupId: 7, category: "polygon" },
        { tile: { z: 17, x: 6328, y: 2354 } }, { provider: "other" }]) {
        assert.equal(classifyFeatures([surface(504812, properties)], types, source)[0]
            .properties.category, "unknown");
    }
    const unmapped = classifyFeatures([surface(504810), surface(501776), surface(60924)],
        { 504810: "", 501776: "UNCLASSIFIED", 60924: "UNCLASSIFIED" }, source);
    assert(unmapped.every((f) => f.properties.category === "unknown"));
});

test("官方明确类型优先，名称锚点不混入道路面", () => {
    const official = classifyFeatures([surface(504812)], { 504812: "LOCAL" }, source)[0].properties;
    assert.equal(official.category, "road");
    assert.equal(official.resolvedFeatureType, "LOCAL");
    assert.equal(official.classificationSource, "official-feature-type");
    const point = surface(504812, { category: "point", groupId: 3 });
    assert.equal(classifyFeatures([point], { 504812: "" }, source)[0].properties.category, "point");
    assert.throws(() => classifyFeatures([surface(504812)], {}, source), /504812/);
});

test("分类接口必须逐项返回有效类型，不执行 JSONP 内容", () => {
    assert.deepEqual(decodeFeatureTypes('baiduFeatureTypes(["LOCAL",""]);', [60499, 504812]),
        { 60499: "LOCAL", 504812: "" });
    for (const text of ['baiduFeatureTypes(["LOCAL"])', 'baiduFeatureTypes(["local",""])',
        'baiduFeatureTypes(["LOCAL",""]);alert(1)', '{"error":"denied"}']) {
        assert.throws(() => decodeFeatureTypes(text, [60499, 504812]));
    }
});
