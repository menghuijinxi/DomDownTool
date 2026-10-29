# Esri World Imagery Wayback 接入调查

调查与接入日期：2026-10-29。已增加桌面在线预览，不包含命令行和离线导出入口。

## 当前在线预览

预览页的地图列表增加 `Esri Wayback`（内部标识 `arcgis.wayback`）。
首次选择时后台查询官方目录，按发布日期降序排列，默认选最新发布版本；
日期栏明确标为“发布日期”，不把版本日期当成影像拍摄日期。
可刷新目录；加载失败会显示错误，保留已选版本并允许重试。
切换图源或日期保持中心、层级和视野倍率。

实现复用已有 Web Mercator 投影、curl、系统代理检测、瓦片队列和图片解码，
没有新增 SDK。目录只在用户选择图源或点击刷新时获取；
目录工作线程在控制器析构时通过 `std::jthread` 的停止令牌取消并等待退出，
结果经互斥锁交给 UI 线程，后台线程不访问地图和 Runtime。
瓦片请求继续使用队列内的请求快照及已有取消机制。

`cache/Esri Wayback/preview/<release number>/z/x/y.png` 按版本隔离缓存。
扩展名沿用预览缓存约定，解码按文件内容识别 JPEG/PNG。
日期菜单只在目录实际变化时重建，常规状态刷新不重置展开菜单。
UI 保留 Esri 及影像贡献者署名；导出来源目录未增加 Wayback。

最小实现当前提供完整发布版本列表，没有接入按位置筛选实际变化版本、
拍摄日期/分辨率面板或目录磁盘缓存。日期切换后画面可能相同，
因为一次全球发布不保证当前地点发生更新。

## 结论

技术上具备接入条件。Wayback 是 Esri World Imagery 的历史发布版本档案，
提供版本目录、按版本请求影像的 WMTS 模板，以及影像来源元数据。
本项目已经通过 `ArcGISDown` 接入 World Imagery 当前版，可复用投影与栅格请求基础能力。

需要区分在线查看与离线导出：

- 官方 `wayback-core` 已提供版本列表、指定位置的实际更新筛选和拍摄信息查询。
- 本轮查询到的公开图层说明明确表示，该图层不面向离线瓦片导出。
- 官方另有 Wayback Export，要求 ArcGIS 组织账号，导出结果限定在 ArcGIS 内使用。
  不能据此默认获得本项目大图、GeoTIFF、FBX 贴图或其他独立离线结果的使用授权。
- 因此建议先按适用条款设计历史在线预览与元数据查询；普通下载和导出能力需另行确认授权。
  SDK 的 Apache 2.0 软件许可证不等于影像内容授权。

## 已取得的接口证据

### 版本目录

官方库 README 给出的生产配置地址：

```text
https://s3-us-west-2.amazonaws.com/config.maptiles.arcgis.com/waybackconfig.json
```

本机不携带账号、Cookie 或 API Key 请求成功，HTTP 200。
本次响应包含 197 个版本，按标题中的日期排序后：

| 项目 | 最早版本 | 最新版本 |
| --- | --- | --- |
| 发布日期 | 2014-02-20 | 2026-09-24 |
| release number | 10 | 63116 |
| ArcGIS item ID | 903f0abe9c3b452dafe1ca5b8dd858b9 | ab83ada1bd3145978eecfb115aaa10c3 |

以上是调查时实际取得的目录，不是固定支持范围。接入后应动态读取。
配置对象的键为版本编号，不能依赖 JSON 键的枚举顺序判断最新日期。

每条记录包括 `itemID`、`itemTitle`、`itemURL`、`metadataLayerUrl`、
`metadataLayerItemID` 和 `layerIdentifier`。
应使用目录返回的 URL 与版本编号，不通过年份或月份猜测版本。

### 影像请求

最新版本目录返回的实际模板：

```text
https://wayback.maptiles.arcgis.com/arcgis/rest/services/World_Imagery/WMTS/1.0.0/default028mm/MapServer/tile/63116/{level}/{row}/{col}
```

`level/row/col` 对应项目的 `z/y/x`。版本编号是路径的一部分；
不同日期必须隔离缓存，不能直接复用当前版 `cache/ArcGIS地图/z/x/y`。

官方库还列出了 `wayback-a`、`wayback-b` 两个生产子域名。
初次直连主域名与这两个子域名均超时；后续复查使用本机启用的手动代理，
北京样点 z18、z19 请求均返回 HTTP 200，分别取得 19938、16718 字节的图片。
服务可能先返回 301，再转到小写路径及另一版本编号的瓦片；
应保留目录的版本身份并跟随 HTTP 重定向，不把 301 响应体当图片。
正式预览的现有瓦片队列已开启 `CURLOPT_FOLLOWLOCATION`。
这只验证了样点请求，不代表所有地点、版本或层级都提供相同覆盖。

### 拍摄日期与分辨率

最新版记录关联的元数据服务：

```text
https://metadata.maptiles.arcgis.com/arcgis/rest/services/World_Imagery_Metadata_2026_r09/MapServer
```

服务根目录及下列点查询均返回 HTTP 200，无登录参数。
服务声明空间参考 `102100 / 3857`，提供不同显示尺度对应的元数据图层。
本次只对图层 6 做 z17 样点查询；正式接入应按显示层级选择相应图层，
不能对所有缩放固定使用图层 6。

```text
/6/query?f=json&geometry=116.42%2C39.89&geometryType=esriGeometryPoint&inSR=4326&spatialRel=esriSpatialRelIntersects&outFields=*&returnGeometry=false&resultRecordCount=1
```

北京样点 `116.42, 39.89` 的返回记录：

| 字段 | 实际值 |
| --- | --- |
| SRC_DATE | 20260126 |
| SRC_DATE2 | 1769385600000 |
| SRC_RES | 0.34 |
| SAMP_RES | 0.3 |
| SRC_ACC | 8.47 |
| NICE_DESC | Vantor |
| NICE_NAME | Vivid Advanced |
| SRC_DESC | LG06 |
| MinMapLevel / MaxMapLevel | 12 / 19 |

结合官方元数据定义，该记录的拍摄日期为 2026-01-26，源分辨率为 0.34 米。
这是单点、单图层的查询结果，不能推及整个城市、整个发布版本，
也不能代替相应影像瓦片的画面验证。

版本发布日期和拍摄日期必须分别展示。例如本次样点属于
2026-09-24 的发布快照，实际拍摄日期为 2026-01-26。
同一范围也可能拼接多个拍摄日期和不同来源的影像。

### 指定位置的历史更新

官方 `@esri/wayback-core` 提供：

- `getWaybackItems()`：全部版本，按发布日期降序。
- `getWaybackItemsWithLocalChanges(point, zoom)`：指定点、指定缩放层级有实际更新的版本。
- `getMetadata(point, zoom, releaseNumber)`：拍摄日期、来源、分辨率和精度。

上述局部更新能力本轮只核实官方 API 文档，未在本机完成调用验收。
指定中心点的变化不能代表整个导出范围的变化。
如果只按响应长度去重，也不能保证不同长度相同的图像不会被错误排除；
官方文档对该性能选项已有说明。

## 与本项目的对应关系

| 现有位置 | 接入时的职责 |
| --- | --- |
| `src/ArcGISDown.cpp` | 当前版 World Imagery URL，保持现有图源独立 |
| `include/GoogleDownBase.h`、`src/GoogleDownBase.cpp` | 可复用 Web Mercator XYZ 选块、像素与经纬度换算 |
| `src/preview/MapRenderer.cpp` | 注册独立历史影像源并在切换版本时更新请求和缓存 |
| `src/preview/PreviewController.cpp` | 历史版本选择、加载、取消、错误状态及拍摄信息 |
| `src/preview/ExportComposer.cpp` | 若获得相应授权，增加带版本的导出来源 |
| `include/preview/ExportTaskManager.h` 及任务序列化代码 | 若开放导出，保存所选版本身份，避免任务恢复后变成另一个日期 |
| `src/main.cpp` | 若开放命令行下载，增加独立图源及版本选择参数 |
| `tests/TileProviderUrlTest.cpp` | 版本路径、行列顺序、坐标和缓存隔离验证 |

建议的实现边界：

1. 先建立独立 Wayback 来源及版本目录查询，不覆盖现有 `arcgis.satellite`。
2. 版本身份同时保存 release number、item ID 和发布日期，
   界面明确日期类型；缓存至少包含 release number。
3. 预览按位置和显示层级查询元数据；局部更新列表与全部版本列表分开。
4. 网络请求复用已有 curl、代理、取消与重试能力，无需因为接口是 Esri 就新增 ArcGIS SDK。
5. 获得对应授权后再设计离线结果及任务记录；不能把“接口公开”作为离线授权依据。

任务持久化格式和离线导出来源没有变更；本轮只实现在线预览所需接口。

## 验证产物与未验证项

本机复查文件位于 `tmp/wayback-research/`：

- `probe.ps1`：少量公开接口与影像请求探针，下载数据用于本次复查。
- `report.json`：目录统计、接口成功状态以及影像请求失败记录。
- `waybackconfig.json`、`latest-item.json`：版本目录和图层使用说明。
- `metadata-service.json`、`beijing-metadata.json`：元数据服务与单点查询响应。

本机直连瓦片曾连接超时，显式使用已启用的 HTTPS 手动代理后成功，
随后也观察到代理 CONNECT 返回 200、后续 TLS 握手仍超时的间歇性故障。
IPv4 与 TLS 1.2 探针均出现过超时，不能据此把根因归结为 IPv6 或某一 TLS 版本。
正式预览的目录与瓦片复用 `GetSystemHttpsProxy()`；
目录经代理失败时额外尝试直连，不写死本机代理端口，不修改系统代理设置。
单次目录请求最多 12 秒，连接超时 6 秒；停止令牌阻止取消后的回退。
该检测不执行 PAC/WPAD；
修改系统代理后应重启程序重新创建图源实例。

自动回归包括 `WaybackTest` 的目录解析、日期排序、异常响应、
版本缓存隔离、异步失败重试、关闭取消；`PreviewRangeEditorTest` 增加
真实控件的图源/日期选择、展开菜单期间刷新、失败恢复和宽窄窗口截图。
手动网络验收命令 `WaybackTest --live` 检查生产目录，
`PreviewRangeEditorTest --wayback-live` 使用生产控制器加载北京 18 级整屏并生成截图。

本轮构建通过，8 项相关 CTest 最终均通过；已有范围编辑测试曾出现一次
“绘制矩形没有生成独立的平整区域”，不改代码复跑通过，原因未确认。
真实目录验收返回 197 个版本，最新为 2026-09-24。
两次真实画面检查在 50 秒期限内分别加载 47/63、50/63 张可见及预取瓦片，
图片已经解码并显示，但部分瓦片持续请求失败；因此两次
`--wayback-live` 的完整加载断言均未通过，不能据此声明整屏网络验收成功。
画面保留在 `tmp/preview-range-editor-test/wayback-live-desktop.png`，
宽窄窗口控件截图在同目录的 `wayback-desktop.png`、`wayback-narrow.png`。

未接入或未全面验收：局部更新筛选、元数据面板、离线任务恢复与导出、
不同国家和大范围覆盖、请求限流与长期稳定性。

后续快速检查顺序：目录是否可达、版本编号是否正确、瓦片域名连接、
代理配置、HTTP 状态与实际图片解码、所选层级的元数据、
缓存版本隔离，以及用途是否在授权范围内。

SkiaUI 控件需要 `data-action` 才会成为点击动作目标。
新增按钮不能只绑定控制器中的 `ElementEventType::Click`；
回归应通过真实鼠标/键盘触发控件，直接调用控制器方法无法发现这类缺失。

## 官方参考

- [Wayback 应用](https://livingatlas.arcgis.com/wayback/)
- [Esri wayback-core API 与软件/影像许可说明](https://github.com/Esri/wayback-core/blob/main/README.md)
- [官方 Wayback Export 说明](https://www.esri.com/arcgis-blog/products/arcgis-living-atlas/imagery/wayback-export)
- [本轮最新图层项目](https://www.arcgis.com/home/item.html?id=ab83ada1bd3145978eecfb115aaa10c3)

官方 Export 文章发布日期为 2023-11-16，介绍的导出格式为 `.tpkx`；
本轮未登录执行该流程，没有把文章中的容量说明当作本机已验证能力。
