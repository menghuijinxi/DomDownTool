# Cesium 验证页

需要 Node.js。启动时指定包含导出数据的根目录：

```powershell
node tools/cesium-preview/server.mjs --data "D:/导出目录" --port 8765
```

打开 `http://127.0.0.1:8765/`，输入相对 `/data/` 的地形目录、
建筑 `tileset.json` 和 XYZ URL 模板。默认目录为 `terrain/`、
`buildings/` 和 `xyz/`。

测试页固定 CesiumJS 1.146.0，从 jsDelivr 加载，需要网络。
terrain 使用 `CesiumTerrainProvider`，建筑使用 `Cesium3DTileset`。
支持切换本地 XYZ、OpenStreetMap、Esri World Imagery 和自定义 XYZ，
不修改已有 XYZ 导出。在线服务的网络可达性和使用许可由使用方确认；
ArcGIS provider 保留服务提供的署名。

CDN 不稳定时，启动服务器可额外指定
`--cesium-build "Cesium 1.146.0/Build/Cesium"`，
服务器将库脚本、Workers、WASM 和 Assets 从该目录提供，验证本地数据不再需要 CDN。
这只是测试网页依赖，不进入下载器的 C++ 依赖；在线底图仍需网络。

`.terrain` 文件已经 gzip 压缩；本服务器设置 `Content-Encoding: gzip`。
部署到其他服务器时必须保持这一响应头，并配置正确的 MIME 和 CORS。
不要对已压缩文件重复 gzip。

建筑纹理使用 KTX2 / BasisU ETC1S，服务器返回 `image/ktx2`。
部署时必须保留 GLB 相对纹理路径，完整复制 `textures/` 共享散图及各层图集，
只复制 GLB 会缺少贴图。导出组件的“生成 Mipmap”默认关闭，
开启后会编码完整 mip 链；验证页由 Cesium 的 BasisU 转码器加载两种输出。
XYZ 保持原有 JPG/PNG，不参与此压缩流程。

Copernicus 高程是 EGM2008 正高。导出参数支持 GeographicLib 的 Geoid PGM
文件及手动高度偏移；没有 Geoid 文件时不进行精确椭球高转换，
`export-report.json` 会明确记录。精确对齐其他椭球高数据需提供对应模型。

`CesiumExportTest` 是离线格式和网格测试；在 Release 工作目录执行
`CesiumExportTest.exe --real-data` 会使用真实 DEM/GIS/高德 XYZ 生成测试数据，
产物位于源码根目录 `tmp/postprocess-tests/cesium-real-data/`。
其中 `buildings/` 默认无 mip，`buildings-mips/` 为同一批真实建筑的完整 mip 版本；
可用页面“建筑 Tileset”路径切换对照。

`ExportTaskManagerTest.exe --real-cesium-task` 另外验证正式任务执行链，
产物和记录位于 `tmp/postprocess-tests/cesium-task-real/`。

浏览器回归需要 Playwright 和 Chromium/Edge：

```powershell
node tools/cesium-preview/verify.mjs "http://127.0.0.1:8765/?autoload=1"
```

`PLAYWRIGHT_MODULE` 可指定已有 Playwright 模块，`PLAYWRIGHT_CHANNEL` 默认为 `msedge`，
`BROWSER_PROXY` 可配置测试浏览器代理。桌面/手机截图和报告保存在
`tmp/cesium-browser-check/`，报告包含 KTX2 的实际请求状态及 MIME。
