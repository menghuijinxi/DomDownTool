# 第三方库与服务接入

## 腾讯和百度电子地图栅格底图

### 接口与坐标边界

2026-10-23 对照两家的官方 JavaScript SDK 并下载真实样本，确认普通电子地图
可以沿用已有栅格下载、预览和后处理链路，不需要矢量 GIS 解码器或新增依赖。

- 腾讯参考 `https://map.qq.com/api/js?v=2.exp` 返回的 JSAPI 2.4.160 配置，
  及其 `mods/map.js` 的普通底图请求生成逻辑。电子地图使用
  `https://rt{s}.map.gtimg.com/tile?z={z}&x={x}&y={y}&styleid=0&scene=0&version=114`。
  本地仍按 GCJ-02 XYZ 处理，在请求边界转换 `y = 2^z - 1 - xyzY`。
- 腾讯普通底图样本返回 `application/octet-stream`，内容实际为 256×256 JPEG；
  不应仅凭 Content-Type 或缓存的 `.png` 扩展名判断无效。复用现有图片解码器识别内容。
- 百度参考 `https://api.map.baidu.com/api?v=3.0` 和其 `getscript` 中的普通底图逻辑，
  使用 `https://maponline{s}.bdimg.com/tile/?qt=vtile&x={x}&y={y}&z={z}&styles=pl&scaler=1`。
  样本为 256×256 PNG。该请求中的 `vtile` 返回栅格图片，不能仅根据参数名推断响应格式。
- 百度继续使用既有 BD09 经纬度、百度墨卡托和向北递增的源瓦片坐标。
  负索引按 SDK 规则转换为 `M` 前缀，如 `-2` 转为 `M2`；
  先扩展为 64 位再取反，避免最小 32 位整数溢出。
- 电子地图使用各自的 `电子地图` 子缓存目录；原卫星缓存路径保持兼容。
  百度不使用腾讯的 Y 轴翻转，也不直接套标准 Web Mercator 原始瓦片索引。

### 验证与维护

`TileProviderUrlTest` 覆盖默认卫星 URL、地图类型切换、缓存隔离、腾讯 Y 轴边界
及百度负索引。`PreviewRangeEditorTest` 验证预览来源、导出菜单选择与配置读取。
命令行检查覆盖两家的 `electronic` 和其他来源的非法类型组合。

本次 Release 构建及 16 项相关回归通过。两家均使用全新缓存，在北京样区
z16、半径 300 米条件下完成真实下载、XYZ 金字塔和 512×768 合并 PNG 导出。
首次联网还暴露了命令行 CURL 全局状态早于下载器销毁的问题，详见
[命令行 HTTPS 下载完成后退出崩溃](crash-and-exception.md)。

排查空白或错位时，先检查响应是否为可解码图片、请求是否误用了矢量参数、
源瓦片索引与 Y 轴方向，以及缓存是否混入其他图层。SDK 接口和版本可能变化；
失效时重新核对官方 SDK，并用真实图片确认，不能只检查 HTTP 200。
下载和图片后处理继续使用现有 CURL/OpenCV 链路，线程生命周期和取消方式未增加。

## CDT 约束 Delaunay 三角剖分用于 FBX 地形 TIN

### 选型与约束

FBX 高度地形从已同步的块边界开始，按高度插值误差逐步加入内部采样点。
2026-09-29 起使用 hmm 的逐点局部细化，不再批量插点并反复全量重建。
初始化边界时的二维约束 Delaunay 三角剖分使用
[artem-ogre/CDT](https://github.com/artem-ogre/CDT) 的头文件实现；该库为 MPL-2.0，完整许可证位于
`third_party/CDT/LICENSE`。CDT 只负责二维拓扑，第三维高度仍由本项目的 DEM 样本提供。

第三方头文件必须作为完整集合引入。`CDT.h` 会间接依赖 `KDTree.h` 与
`portable_nth_element.hpp`；缺少其中任一文件时，默认 `LocatorKDTree` 模板无法编译。当前目录保留了
这两个依赖以及原始许可证，更新 CDT 时应同时更新其全部 `CDT/include` 头文件，不能只复制四个主头文件。

### 运行与验证

- `BuildFbxTerrainTin` 接收有限且非负的几何误差、有限高度样本、顺序排列的矩形边界。
  面数预算为 0 时不限；正数必须容纳边界初始网格。导出器会先统一简化共享边界以适配预算。
- 运行 `FbxTerrainTriangulatorTest` 覆盖平面、孤立峰值、非法输入、预算、全样点误差、
  随机矩形及细长网格的拓扑；`PostProcessFbxTest` 验证最终 FBX 的面数与跨块接缝。

## hmm 地形渐进近似

- 上游 `fogleman/hmm`，固定提交 `5ea611461e9f8b40f3d97e9f10fcf0f40592030b`，MIT。
  完整许可和派生说明位于 `third_party/hmm/`。
- `src/FbxTerrainTriangulator.cpp` 适配其半边插点、Delaunay 局部换边和索引最大堆，
  使用已有 CDT 初始化固定边界。不引入上游 GLM、图片读取或命令行模块，无新增链接依赖。
- 每次插入当前误差最大的内部样点，局部换边后立即重算受影响三角形的全部栅格样点误差。
  不移动已有样点、不做三维焊接或边折叠。临时状态和高度引用仅活在同步函数调用期间，
  没有共享可变状态。
- 候选点禁止落在边界，边界点由相邻块共同确定；内部插点每次增加两个三角面。
  奇偶性可能使结果比上限少一面，平坦区域也不会为了凑预算强行增面。
- 内圆测试使用整数谓词，接口限制网格单边至 4096 段以防乘积溢出；生产采样最多 1024 段。
  此限制是采样网格尺寸，不是隐藏的三角面预算。
- 非法输入返回空结果；内部拓扑/堆不变量异常明确抛错。导出器不再静默回退到另一种算法。
- `maximumErrorMeters` 包含未细化的固定边界误差；它是输入栅格样点误差，不是连续真实地表、
  球面投影或任意低预算下的形变保证。完整对照见 `terrain-simplification-study.md`。

## Assimp 与 Skia 的本地 FBX 预览

导出任务预览通过 Assimp 读取 FBX 的节点变换、三角面、UV 和 diffuse 贴图路径，再由 Skia 在本地
画布中绘制贴图、线框或两者叠加。项目导出的 FBX 将 PNG 贴图与模型放在同一目录，且材质使用相对
diffuse 路径，因此预览必须从 FBX 所在目录解析贴图，不能假定当前工作目录。

预览的主路径是原生 DX12：Assimp 读取网格和材质，Skia 解码 PNG 后在 CPU 侧编码为 BC1/DXT1，
再通过 D3D12 上传到 GPU；每张纹理限制为 4096 像素边长，描述符预算为 1024 张。这样 9 张
4096 贴图的预览显存约为 48 MiB，正常不会再因为“只取前四张”而出现大面积无贴图区域。当前
BC1 编码器使用 4 色块端点和最近颜色索引，属于预览用途的轻量实现；若后续需要更高质量或法线/
透明通道，再接入 DirectXTex 等成熟编码库。Skia 路径仅作为 DX12 初始化失败时的回退，最多加载
16 张纹理。渲染 UV 时需将 `v` 反转，因为导出器已按 FBX 坐标约定调用 `ConvertImageVToFbxV`。

验证时应使用 `PostProcessFbxTest` 生成的含外部 PNG 的 FBX，并确认任务列表入口、贴图、线框和
贴图加线框三种模式均可用；拖动旋转、滚轮缩放不应改变导出文件。

## OpenCV 读取中文贴图路径

### 现象与影响

建筑散图 FBX 导出时，如果墙体/屋顶贴图目录使用默认 `贴图/墙体`、`贴图/房顶` 中文路径，
直接调用 `cv::imread(PathToUtf8String(path))` 会在 Windows 下读取失败。导出器会误判目录里
没有可读取的 JPG/PNG，任务记录中表现为“建筑贴图目录没有可读取的 JPG/PNG 贴图”。

### 根因与解决方案

OpenCV 的窄字符串文件路径在 Windows 下不能稳定处理 UTF-8 中文路径。项目内读取图片时应先用
`std::ifstream` 和 `std::filesystem::path` 打开文件、读入字节，再调用 `cv::imdecode()`；写入
图片则先 `cv::imencode()`，再通过文件流落盘。不要在中文缓存目录、用户导出目录或素材目录上
直接使用 `cv::imread(path.string())` / `cv::imwrite(path.string())`。

### 验证方式

`AmapVectorPngTileTest` 的建筑 FBX 后处理用例使用中文 `贴图/墙体` 和 `贴图/房顶` 临时目录，
覆盖无后缀 JPEG 墙体贴图、`.jpg` 屋顶贴图、FBX 材质贴图复制和高度顶点生成。

## WebView2 同会话访问高德网页搜索

### 现象与影响

直接通过 libcurl 请求高德网页联想接口时，可能返回 Baxia 风控页面而不是 JSON。
Web JSAPI Key 也不能用于高德 Web 服务 REST API，会返回 `USERKEY_PLAT_NOMATCH`。
因此，地点搜索不能复用矢量瓦块的 `auth_key` 或 Web JSAPI Key。

### 根因

高德网页搜索依赖浏览器会话中的 Cookie 和风控状态。WebView2 的用户数据默认按可执行文件
profile 持久化，不同 EXE 的验证结果不会自动共享。人工完成一次验证后，同一个 profile
通常可以继续执行后续搜索，但这是服务端行为，不应假定永久有效。

### 当前方案

- `AmapWebViewSearch` 在独立 UI 线程中维护隐藏的 WebView2 实例。
- 联想请求从 `https://www.amap.com/` 页面上下文发起，并携带当前会话凭据。
- 非 JSON 响应或验证码页面触发可见验证窗口；用户确认后回到主页并重放原请求。
- 完整搜索加载 `/search` 页面，从 `.poibox[data-poiinfo-x][data-poiinfo-y]`
  提取 GCJ-02 坐标，再统一转换为 WGS84。
- 取消新请求、关闭验证窗口和退出程序都会唤醒等待线程，避免后台搜索阻塞析构。
- 非验证状态下的页面请求有原生超时；不能只依赖页面 JavaScript 回调，否则脚本注入失败时
  搜索界面会永久停留在加载状态。
- 搜索链路日志写入运行目录的 `logs/amap-search.log`，每次启动会清空；日志不记录 Cookie
  和响应正文。

### 验证方式

1. 首次搜索触发风控时完成人工验证，并点击重试按钮。
2. 连续执行多次联想搜索，确认不再重复弹出验证窗口。
3. 提交完整搜索，确认候选包含坐标且地图定位标记与目标地点一致。
4. 验证期间关闭窗口，再退出主程序，确认进程不会挂起。

### 快速检查清单

- 搜索一直停留在加载状态：检查 WebView 启动、页面 ready 回调和取消唤醒路径。
- 验证后仍反复风控：确认验证发生在 `DomDownToolPreview.exe` 使用的 profile 中。
- 联想正常但完整搜索为空：检查 `.poibox` 选择器及 `data-poiinfo-*` 属性是否变化。
- 坐标存在但定位偏移：确认网页坐标先按 GCJ-02 转换为 WGS84。
- 页面接口或 DOM 属于高德网页内部实现，可能随时变化；正式分发前还需确认服务条款和授权范围。

### 网页搜索坐标不能使用首条结果兜底

高德搜索页的网络响应不只包含搜索结果，还包含当前地图范围、道路、行政区和其他页面状态对象。
递归扫描 JSON 时，这些对象也可能同时具有名称和坐标，而且出现顺序会随页面加载时序变化。
如果目标 POI 匹配失败后直接使用第一条坐标，地图就会随机跳到与用户选择无关的位置。

当前实现采用两层校验：页面采集脚本只返回名称与本次查询词匹配的对象；控制器优先要求 POI ID
精确相等，只有一边缺少 ID 时才允许名称精确匹配。两层均未匹配时必须保持地图中心和搜索输入不变，
显示无法确认坐标的错误，并记录 `location_resolution_rejected`，禁止退化为任意带坐标对象。

验证时应在日志中确认成功路径的 `location_applied` 名称与用户点击项一致；失败路径只能出现
`location_resolution_rejected`，不能出现无关地点的 `location_applied`。

## WebView2 同会话访问天地图多时相接口

### 现象与影响范围

用户在天地图多时相登录窗口完成登录后，预览页点击“刷新年份”仍提示“当前视窗没有返回可用历史时相”。
该问题影响预览界面的历史时相列表，不影响公开最新影像 WMTS 加载。

### 根因与排查路径

天地图多时相的年份源是 `/api/multidate/basic/GetRegion`，官方请求会同时携带
`gb`、`lbx`、`lby`、`rux`、`ruy`、`level`，并把响应按自定义 4 字节编码解成 JSON。
早期实现误把 `/api/multidate/basic/list` 当作年份源，并且只把 `GetRegion` 当普通文本读取，
因此即使页面已登录，也会得到空列表或乱码。
缺少有效网页登录态时，多时相接口仍可能跳转到 OAuth 登录页；排查时不能把登录页、解析失败和真实空列表折叠成同一种状态。
排查时应查看运行目录 `logs/amap-search.log` 中的 `TianDiTuWebView` 和
`tianditu_timeline_response` 记录，确认状态码、最终 URL、响应字节数和错误信息，日志不要记录 Cookie。

### 最终解决方案

- 年份列表查询从 `https://image.tianditu.gov.cn/multidate` 页面上下文发起，调用
  `/api/multidate/basic/GetRegion`，使用
  WebView2 内部 `fetch(..., credentials: 'include')` 复用网页登录态。
- `GetRegion` 返回体不是普通 JSON 文本；官方会把每 4 个字节拼成整数并右移 2 位，再按 UTF-8
  解码。解出的根对象形如 `{"status":200,"msg":"ok","data":"[...]"}`，其中 `data` 字段仍需二次解析。
- 多时相预览源不应强制限制到 17 层以上；年份记录里的 `minlevel` / `maxlevel` 只用于提示
  单个历史时相的大致覆盖层级，预览缩放边界仍应与天地图影像源保持一致。
- Cookie 仍同步给瓦片下载请求，供选中历史时相后加载 `/api/multidate/basic/tile` 使用。
- 年份列表能刷新不代表历史瓦片也能通过 libcurl 下载。年份列表请求运行在 WebView 登录页上下文里，
  自动携带浏览器完整登录态；历史瓦片如果改用普通 libcurl，即使回灌 Cookie，也可能缺少浏览器会话
  约束或被服务端判为未登录。预览侧历史瓦片应复用同一个 WebView `fetch(..., credentials: 'include')`
  请求图片字节，再交给统一下载队列做 `.part` 临时文件、图片解码校验和原子替换。
- 官网多时相不是直接用历史瓦片替换整张底图，而是先加载最新影像 WMTS，再把选中的历史时相作为
  覆盖层叠加。历史覆盖层没有返回瓦片时，应保留最新底图，不能画缺失瓦片占位。
- 导出历史时相也应保持同一语义：先导出最新 WMTS 底图，再导出历史覆盖层，最后按 alpha 合成。
  历史覆盖层的中间产物必须使用 PNG 保留透明通道；用户选择 JPG 时只影响最终合成图，否则空白
  历史瓦片会在合成前丢失 alpha 并变成黑块。
- `/api/multidate/basic/tile` 在旧年份或局部无覆盖区域可能返回 HTTP 200 但响应体为 0 字节；这是
  “该位置无历史瓦片”，不是登录态失效。预览侧应把这类空响应缓存为透明瓦片，导出后处理也应在
  明确启用历史覆盖层语义时把 0 字节、缺失或被判无效的目标历史瓦片转换成透明占位，避免把局部
  无覆盖误判为整次导出失败。
  状态栏或面板需要单独展示历史叠加的可见、已显示和空白瓦片数，避免把“服务器没有覆盖”和
  “程序没有加载叠加层”混为同一种现象。
- 多时相历史瓦片接口在客户端只有 `image.tianditu.gov.cn` 一个入口，不像普通 WMTS 或高德卫星那样
  暴露 `t0` 到 `t7` 之类的客户端负载均衡域名。大批量导出时历史覆盖层应使用较低并发，并把 0 字节
  响应当成可重试的临时状态，而不是立即永久固化到缓存。
- 预览侧不能无限重试同一个历史覆盖瓦片。对连续请求失败或连续空响应的瓦片，应在旁路 marker 中
  记录尝试次数；未达到上限时刷新仍可重新请求，达到上限后停止自动入队，并在瓦片诊断面板明确标记
  为“请求失败”。用户手动刷新瓦片时应清除这些失败 marker，让该瓦片从 0 次重新计数，避免持续消耗
  服务端配额，同时保留明确的人工重试入口。
- 年份记录里的 `key` 是十六进制编码的真实瓦片模板，但生产逻辑仍应走官方
  `/api/multidate/basic/tile`。实测直接访问解码后的北京历史瓦片域名可能返回 502，且官网前端也没有
  直接使用解码 URL。
- 瓦片响应头里的 `content-type` 不完全可信；实测可能标为 `image/png`，实际字节是 JPEG。判断瓦片
  是否有效应依赖图片解码或魔数，而不是只看响应头。
- `changeCity` 返回带国家码前缀的 9 位行政区编码，例如北京 `156110000`；官方 `GetRegion` 和
  `/basic/tile` 都使用这个 9 位编码。`/basic/list` 曾实测只接受 6 位 `110000`，但它不是当前年份源。
- `changeCity(level=4)` 对直辖市通常返回城市本身；对普通地级市会同时返回 `parentGbcode` 省级编码和
  `gbcode` 市级编码。多时相 `GetRegion` 和瓦片接口应优先使用 `parentGbcode`，同时保留市级
  `name`。实测廊坊点 `gb=156131000` 会返回服务异常或空列表，`gb=156130000&name=廊坊市` 才能
  返回 2022、2023、2024 等历史瓦片。
- 多时相年份 `key` 是区域相关的，不是全局年份 ID。搜索或手动输入经纬度跳转到新区域后，不能继续
  使用旧区域的年份列表和 key；应刷新当前视窗年份，必要时按年份字符串重新匹配新区域 key。
- 历史瓦片缓存目录不能只按年份命名。相同年份在不同区域、不同 `key` 或旧接口算法下都可能对应
  不同瓦片内容；缓存层级应包含年份、区域编码和 key 指纹，否则会把旧区域的合法 256x256 错图命中为
  “已加载”，删除缓存后才恢复正常。
- 天地图关键登录态可能绑定服务端一次性会话。即使把 WebView2 Cookie 回灌成功，重启后页面仍可能
  立即跳回 OAuth；预览程序只承诺“本次运行内登录态”有效，不应依赖跨重启恢复。
- 控制器分别处理登录失效、HTTP 非 200、响应为空、JSON 解析失败和真实空列表，避免误报。
- 时间线解析器接受官方 `key`、`time`、`projection`、`minlevel`、`maxlevel` 字段，同时保留对
  `uuid`、`title` 和 `data.page.records` 的兼容。

### 验证方式与快速检查清单

- 运行 `TianDiTuMultidateDownloadTest`、`ExportTaskManagerTest`、`MapViewportTest`、`PreviewSearchUiTest` 和
  `TileDownloadQueueTest`。
- 多时相导出失败且记录里只有“卫星合并大图未生成”时，先检查历史年份缓存目录是否缺少目标范围
  瓦片、存在 0 字节瓦片，或临时历史覆盖目录没有生成 `merged.png`。若命中，应确认导出流程正在走
  “最新底图 + PNG 历史覆盖层 + alpha 合成”，并在历史覆盖语义下把不可用目标瓦片补成透明占位，
  而不是把历史覆盖层当作完整卫星底图直接后处理。
- 手工验证时选择“天地图多时相”，点击“登录”，在弹窗完成登录后点击同步按钮，确认年份下拉框出现
  历史时相；若仍为空，先检查日志中的 `final_url` 是否指向 OAuth 登录页。
- 开发接口参数时不要每次改 C++ 后重新登录。优先运行 `tmp/tianditu-probe/probe.mjs`，在同一个
  已登录浏览器上下文里批量测试 `gb`、`level` 和 bounds 组合，再把结论集成回 C++。
- JS 探针验证历史瓦片时，需要同时记录 `fetch` 的字节数、魔数和 `<img>` 的 `naturalWidth` /
  `naturalHeight`。HTTP 200 本身不能证明该年份有可显示瓦片。
- 对年份切换问题必须做确定性验证：同一坐标、同一瓦片坐标、同一区域参数下，分别请求 2022、2023、
  2024 等候选年份，保存原始图片字节并比较 SHA-256。若三张图均可解码且哈希不同，才说明接口参数
  和数据源可用；否则不能进入 C++ 集成。
- 若年份能列出但历史瓦片不显示，先确认年份列表是不是当前视窗刷新得到的；再确认同一位置同一年份
  是否返回 0 字节空瓦片；若不是空瓦片，再继续检查瓦片请求的 Cookie、`gb`、`name` 和 `key` 参数。
  普通地级市尤其要确认 `gb` 是省级 `parentGbcode`；不要从旧区域年份列表推断新区域瓦片链路一定有效。

## Skia 路径迭代器会忽略仅含移动命令的点路径

### 现象与影响

GIS 解码日志中的 `point` 数量正常，但选择“点”或“全部”时看不到任何兴趣点。线和面仍能
正常绘制，因此问题容易被误判为兴趣点分类或下载失败。

### 根因与解决方案

路径缓存把点坐标保存为只包含 `moveTo` 的 `SkPath`。`SkPath::Iter` 会忽略这种没有线段或曲线
的退化轮廓，导致点绘制循环直接结束，`drawCircle()` 从未执行。点要素应直接遍历准备好的
点几何坐标；线和面仍可使用普通路径绘制。

### 验证与快速检查

- 构造位于视口中心的单个点要素，调用 `GisOverlayRenderer::Draw()`，断言中心像素非透明。
- 日志中 `point` 大于零但画面无点时，依次检查缩放可见性、视口边界过滤和点坐标读取方式。
- 不要使用会过滤退化轮廓的路径迭代器枚举独立点；应保留并直接遍历原始点坐标。

## 不要为 CPL JSON 引入完整 GDAL 静态依赖

### 现象与影响

项目只使用 `cpl_json.h` 读写 JSON，但链接 `GDAL::GDAL` 后，Release EXE 同时带入 GDAL、
PROJ、SpatiaLite、HDF5、NetCDF、GEOS 和 SQLite 等静态库。预览程序因此从约 24 MiB
增长到约 74 MiB，而这些地理数据接口没有任何业务调用。

### 根因与排查路径

vcpkg 的 GDAL 默认特性包含多种数据格式和数据库支持，`GDAL::GDAL` 又通过 CMake 接口目标
传播完整链接依赖。排查时不能只搜索 `GDAL`，还应搜索 `cpl_json.h`、`CPLJSONDocument`、
`CPLJSONObject` 和 `CPLJSONArray`，并检查最终 `build.ninja` 或链接响应文件。

### 解决方案与验证

- JSON 统一使用 header-only 的 `nlohmann-json`，在 `vcpkg.json` 中声明直接依赖，并链接
  `nlohmann_json::nlohmann_json`。
- 删除 `gdal` 和没有直接 API 调用的 `libspatialite` manifest 依赖。
- 解析外部响应时使用非抛异常解析，并在读取字段前检查对象、数组和标量类型。
- 迁移后运行 GeoJSON、任务持久化、地点搜索、XYZ、大图和 FBX 相关测试。
- 检查最终链接命令不再包含 `gdal.lib`、`proj.lib`、`spatialite.lib`、`libhdf5.lib`、
  `netcdf.lib`、`geos.lib` 和 `sqlite3.lib`，再对比最终 EXE 大小。

后续新增第三方库前，应先确认代码是否真的使用其核心能力。若只需要 JSON、摘要、图片编码等
基础功能，应优先依赖对应的轻量成熟库，避免为了附属工具类引入大型静态依赖树。

## Cesium 地形剔除与 Assimp GLB 属性边界

日期：2026-10-10。验证环境：CesiumJS 1.146.0、Assimp 6.0.4。

### 现象和影响

新 terrain 服务能够返回根瓦块，但 Cesium 不请求子块，地球显示空白，只有建筑可见。
另外，GLB 在 Cesium 能显示，但 Khronos glTF Validator 报 `ACCESSOR_MIN_MISMATCH`。

### 根因和排查路径

- 先排除请求 404、gzip 响应头、TMS 可用范围和包围球，再检查剔除头部。
  terrain 的零 horizon occlusion point 不是“禁用剔除”，而是地球内部的点，
  会被 `EllipsoidalOccluder.isScaledSpacePointVisible()` 判为不可见。
- 高度全零不代表地球表面是平面。粗层只按高度误差简化，会生成跨越大范围的弦面，
  即使文件索引和包围球正确，也可能无法贴合地球和影像。
- Assimp `ExportToBlob(scene, "glb2")` 的 float accessor 最小值对全正数属性输出 0，
  与 BIN 中真实 POSITION/UV 最小值不一致。通过独立校验器而非肉眼定位。

### 解决方案

terrain 写入椭球缩放空间有效的 horizon 点；半球根层使用保守远点。
粗层保持曲率网格，细层在 TIN 细化参考高度中加入球面弧度项。
`src/CesiumTerrainExporter.cpp` 是协议和剔除边界实现位置。

GLB 仍交给 Assimp 序列化，只对已有 float accessor 边界依据 BIN 重算，
JSON/BIN 长度及四字节对齐重新写入，顶点/索引不变。
`src/CesiumGlbWriter.cpp` 实现此窄范围兼容修正，不是通用 glTF 编解码库。
ExportToBlob 返回对象由 exporter 持有，必须在 exporter 生命周期内处理；
每次调用使用本地 exporter，不共享跨线程实例。

### 验证和快速检查

- terrain 读取器断言 horizon 有限且位于有效缩放空间，索引合法、三角形不反向。
- 浏览器确认实际请求并渲染子层，不只检查 root 请求或画布非空。
- GLB 使用全正数坐标回归测试；真实导出交给 Khronos glTF Validator。
- 显示不等于协议正确，结构校验也不等于模型形状正确，必须两者结合。
- 网络来源失败应记录具体 URL；不要把在线影像的连接失败误判为本地格式错误。

参考：CesiumGS quantized-mesh README、Cesium EllipsoidalOccluder 源码、
KhronosGroup glTF-Validator；来源地址记录在 `docs/design/cesium-3d-tiles-export.md`。

## 建筑 KTX2 / ETC1S 与 Assimp 的 BasisU 引用

日期：2026-10-10。版本：KTX-Software 4.4.2、Assimp 6.0.4、CesiumJS 1.146.0。

### 目标和影响范围

3D Tiles 建筑散图和各级基础色图集改为 KTX2 / BasisU ETC1S，质量参数 128，
对齐目标项目 OSGB 的 mipless ETC1S 压缩方案。Mipmap 是组件选项且默认关闭。
不改变 XYZ、FBX、GeoTIFF 或地形纹理逻辑；编码器不额外强制 512 最大边。

ETC1S 是有损压缩，质量参数不是质量百分比，也不能保证所有内容肉眼无差异。
GPU 转码后的格式由客户端支持情况决定，不可仅由 KTX2 文件大小推断显存或帧率。

### 排查路径和关键约束

- 核对目标项目后期 OSGB 转换流程、实际输出 GLB 和 KTX2 层数，
  不用早期单模型测试的 512 + mip 设计替代当前配置。
- `ktxTexture2_Create` 创建 RGBA8 sRGB、2D、单层单面纹理。
  基础尺寸按 KHR_texture_basisu 要求对齐到 4 的倍数，不必扩成二次幂。
- `generateMipmaps` 必须为 false；无 mip 时 `numLevels=1`，有 mip 时先填完
  全部层，再调用 `ktxTexture2_CompressBasisEx`。压缩块不能依赖 WebGL 现场生成 mip。
  不要事后剥离 BasisLZ 层，否则容易破坏共享码本和层索引。
- 输入 OpenCV BGR/BGRA 转为 RGBA，基础色标记 sRGB，KTXorientation 为 `rd`。
  mip 在解码 sRGB 后的线性预乘 RGBA 空间缩小，再转回非预乘 sRGB。
- ETC1S 参数使用默认 compressionLevel、qualityLevel=128、uastc=false，
  每张图片最多 4 个压缩线程；每次调用拥有自己的 KTX 对象，不共享可变编码状态。
- 检查每个 KTX API 返回码。纹理由 RAII 调用 `ktxTexture_Destroy`，
  `ktxTexture_WriteToMemory` 分配的序列化内存由 `free` 释放；
  使用 C++ filesystem 流写出，避免第三方窄字符文件 API 的中文路径问题。
- 取消在编码前、各 mip 写入和单张压缩完成后检查。
  libktx 的本次同步 API 不能中断内部一张图片的压缩；不会在取消后继续写出纹理。

### Assimp 兼容修正

Assimp 6.0.4 遇到 `.ktx2` URI 会声明 KHR_texture_basisu，但写出 texture 时仍使用
普通 `source`，未写 `texture.extensions.KHR_texture_basisu.source`。
仅检查 extensionsUsed 会误判为已兼容。`CesiumGlbWriter` 对已有 JSON 做窄范围修正：
纹理引用改为标准 BasisU 扩展、标记 image/ktx2、声明 required extension，
并按是否包含 mip 选择 LINEAR 或 LINEAR_MIPMAP_LINEAR。BIN 顶点/索引保持原样。

### 验证、许可证和快速检查

- `CesiumExportTest` 读取 DFD 确认 ETC1S、sRGB、BasisLZ 和层数，
  再调用 `ktxTexture2_TranscodeBasis(..., KTX_TTF_RGBA32, 0)` 检查各层像素。
  覆盖颜色通道、透明度、线性缩小、非二次幂尺寸及失败/取消。
- 完整 GLB 使用 Khronos glTF Validator，网页检查实际 KTX2 请求、
  Cesium 加载成功及近远选层；不能只看扩展字段或 HTTP 200。
- 本机 glTF Validator 2.0.0-dev.3.10 不支持 KHR_texture_basisu，
  因而产生 KTX2 MIME/图片格式警告。保留警告报告，不把零错误表述为纹理完整验收；
  补充 `ktx validate --gltf-basisu --warnings-as-errors` 和实际 Cesium 转码显示。
- 首次 WASM 转码和 shader 构建可能超过固定截图等待时间；
  浏览器回归等待 tilesLoaded、实际叶层选择和 terrain 渲染就绪，
  CDN 不稳定时可指定同版本本地 Cesium build，避免外网波动掩盖格式结果。
- 看不到贴图时先查 BasisU texture 引用、相对 URI、MIME 和全部共享纹理是否部署，
  再检查客户端转码器加载和 GPU 压缩格式支持。
- KTX-Software 主体为 Apache-2.0，内置组件还包含 MIT、BSD、Zlib 和
  Ericsson ETC decoder 的专用许可等；不能将整个软件包笼统标为仅 Apache。
  CMake 复制 vcpkg 包内 copyright 与 LICENSES 到发行目录 `licenses/ktx/`。

官方参考：<https://github.com/KhronosGroup/KTX-Software/tree/v4.4.2>、
<https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_texture_basisu>。
本地入口：`src/CesiumTextureEncoder.cpp`、`src/CesiumGlbWriter.cpp`、
vcpkg 安装包的 `include/ktx.h`、`share/ktx/copyright` 和 `share/ktx/LICENSES/`；
Assimp 参考 `code/AssetLib/glTF2/glTF2AssetWriter.inl`。

## Shapelib 导出、中文路径与 DBF 限制

日期：2026-10-20。版本：Shapelib 1.6.3，由 vcpkg `shapelib` 提供。
采用核心库提供的 MIT 许可选项；发行目录保留包内 `copyright`。
使用 `shapelib::shp` 静态链接目标，不额外依赖 GDAL。

### 目标与约束

为导出组件新增 SHP，复用 GeoJSON 的 PBF 获取、分类与空间过滤流程。
SHP 单个文件只能保存一种几何类型，因此点、多段线和多边形分别写出；
多边形通过 `SHPRewindObject` 规范外环、孔洞方向。
高德输入为 GCJ-02，写入前调用现有坐标转换为 WGS84，并补写 `.prj`。
Shapelib 自身不负责坐标转换或投影文件生成。

### API 与平台问题

- SHP、DBF 和几何对象分别通过 `SHPClose`、`DBFClose`、
  `SHPDestroyObject` 管理；各次调用使用独立句柄，不在线程间共享。
- Windows 中文路径使用 `SASetupUtf8Hooks` 配合 `SHPCreateLL`、
  `DBFCreateLL`，其他配套文件使用 `std::filesystem::path` 流。
  不将中文路径先转为本地窄字符串再拼接扩展名。
- `DBFCreateLL` 指定 `UTF-8`；先确认 `.cpg` 可创建，规避该版本在 CPG
  创建失败后继续操作空文件句柄的路径。
- DBF 字段名最多 10 字符、文本采用 254 字节上限；截短时保留完整 UTF-8 字符。
  属性直接写入 DBF；不生成额外的属性 JSON 副本，避免重复序列化和磁盘写入。
- `uid` 用文本保存；数值整数用 `DBFWriteAttributeDirectly` 写十进制文本，
  避免写入前先转换为 double 丢失精度。空高度用 DBF NULL。
- 检查创建句柄、添加字段、写几何及属性的返回值；失败返回任务错误。
  每个要素和坐标循环检查取消，暂存文件由 RAII 清理；完成后同目录重命名，
  避免大文件重复复制。重新下载沿用任务层的暂存目录及旧结果替换流程。
- VS18 的 19.5x 编译器被当前包的 CMake 版本检查误判为不兼容；
  `find_package` 沿用 GeographicLib 的工具集版本兼容处理，不更改实际编译器。

### 验证与快速检查

`AmapVectorShapefileTest` 写出后用 Shapelib 重读，检查中文路径/属性、
大整数、孔洞、多点、多段线、坐标系、空结果、取消和错误路径，
并确认不生成属性 JSON、清单不再引用此类文件。
`ExportTaskManagerTest` 用本地 PBF 缓存跑完整 SHP/GeoJSON 组件任务，
验证 GIS 排除区和任务持久化；`PreviewRangeEditorTest` 点击生产菜单及图层开关，
检查桌面和窄窗口的实际渲染。

遇到乱码先查 `.cpg` 和 UTF-8 路径钩子；错位先查 GCJ-02 转换与 `.prj`；
长字段截短先核对 DBF 字段宽度和 `manifest.json` 的字段映射。

官方参考：<https://github.com/OSGeo/shapelib/tree/v1.6.3>。
代码入口：`src/AmapVectorShapefileWriter.cpp`；库源码参考
`shapefil.h`、`safileio.c`、`shpopen.c`、`dbfopen.c`。

### WGS84 SHP 导入与内部高德坐标边界

日期：2026-10-22。继续复用 Shapelib 1.6.3，不引入 PROJ 或 GDAL。

- 接入风险：原渲染、范围过滤、FBX 和 3D Tiles 都默认 GIS 几何为 GCJ-02；高德高度
  是内部层数，直接输入 WGS84 或米制高度会造成错位及建筑比例错误。
- 本地要素显式标记 WGS84，公共坐标转换入口直接返回原始经纬度；米制高度单独保存，
  不经整数层数换算。范围掩膜按数据坐标系生成，混合数据分别处理后合并。
- Shapelib 负责 SHP/SHX/DBF 二进制解析，`SHPOpenLL`、`DBFOpenLL` 使用 UTF-8 路径
  hooks；读取对象用 `SHPDestroyObject` 释放，文件句柄用 `SHPClose`、`DBFClose`
  的 RAII 包装管理。导入前检查 DBF 记录数，逐记录检查空几何、删除标记、分段和坐标。
- Shapelib 不提供 PRJ 解释器。本轮明确不增加投影依赖，因此仅对有长度和递归深度限制的
  WKT 元数据做保守识别：二维地理坐标、WGS84 基准及椭球、度单位、格林尼治零子午线。
  投影、其他基准、未知定义和缺失 PRJ 全部报“暂不支持”，不根据数值范围猜测坐标系。
- DBF 按声明编码解码，属性保留到 GeoJSON；重新导出 SHP 时扫描额外字段，为与内置字段
  重名的原字段分配独立名称，整数 ID 按文本写出。多点、多段线、多外环及孔洞保留。
- 读取和范围处理在现有导出工作线程运行，逐记录及顶点批次检查取消；窗口拖入只读取
  元数据。任务仅保存源路径和映射，执行时重新验证源文件，避免在 UI 线程加载整份数据。
- 地图预览另用一个现有 GIS 叠加器实例，在其工作线程读取本地文件，不请求高德 PBF；
  预处理也必须经过 `VectorCoordinateWgs84`，避免把本地经纬度再次当作 GCJ-02 转换。
  删除或隐藏图层时增加请求代次、取消旧读取并立即清理绘制缓存，旧读取和网格结果不能
  重新挂回地图。预览列表独立于导出组件，只有用户点击导出才构造新任务草稿。

验证入口为 `Wgs84ShapefileReaderTest`、`ExportTaskManagerTest` 和
`PreviewRangeEditorTest`，覆盖错误 PRJ、缺失 PRJ、中文路径和属性、精确 ID、米制高度、
无网络读取、范围排除、现有组件执行、配置恢复、真实 `WM_DROPFILES`、地图像素与孔洞、
异步删除、多文件移除以及窄窗口和高 DPI 布局。
排查错位先看 `VectorCoordinateWgs84` 和要素坐标标记；排查建筑比例先检查高度字段单位，
不要调整全局楼层高度来抵消错误。
