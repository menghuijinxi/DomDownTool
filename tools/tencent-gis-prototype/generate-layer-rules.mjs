// 从已验证的 SDK 元数据生成 C++ 静态分类表，避免运行时依赖 SDK 脚本和样式服务。
import fs from "node:fs/promises";

const metadata = await fetch("http://127.0.0.1:8776/api/meta").then((response) => response.json());
const rules = {};
for (const kind of ["region", "line", "point", "label"]) {
    for (const [tag, ids] of Object.entries(metadata.tags[`${kind}Tag`] || {})) {
        for (const id of ids) {
            const styles = metadata.styles[`${kind}Styles`]?.[id] || [];
            rules[id] = [tag,
                styles.length ? Math.min(...styles.map((style) => style.minScaleVal)) : 0,
                styles.length ? Math.max(...styles.map((style) => style.maxScaleVal)) : 30,
                styles[0]?.stylename || ""];
        }
    }
}
const encoded = JSON.stringify(rules);
const chunks = Array.from({ length: Math.ceil(encoded.length / 8000) },
    (_, index) => 'R"tencent(' + encoded.slice(index * 8000, (index + 1) * 8000) + ')tencent"');
await fs.writeFile(new URL("../../src/TencentVectorLayerRules.inc", import.meta.url),
    "// Generated from Tencent GL JS SDK 1.8.2.3, style 99, data 405.\n" +
    "// Regenerate: node tools/tencent-gis-prototype/generate-layer-rules.mjs\n" +
    "std::string(" + chunks[0] + ")\n" +
    chunks.slice(1).map((chunk) => " + " + chunk + "\n").join(""));
