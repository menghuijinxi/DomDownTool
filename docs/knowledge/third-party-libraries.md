# 第三方库与服务接入

## CDT 约束 Delaunay 三角剖分用于 FBX 地形 TIN

### 选型与约束

FBX 高度地形采用 HERE `tin-terrain` 的误差驱动细化思路：从已同步的块边界开始，按高度插值误差
逐步加入内部采样点，再重建三角网。二维约束 Delaunay 三角剖分使用
[artem-ogre/CDT](https://github.com/artem-ogre/CDT) 的头文件实现；该库为 MPL-2.0，完整许可证位于
`third_party/CDT/LICENSE`。CDT 只负责二维拓扑，第三维高度仍由本项目的 DEM 样本提供。

第三方头文件必须作为完整集合引入。`CDT.h` 会间接依赖 `KDTree.h` 与
`portable_nth_element.hpp`；缺少其中任一文件时，默认 `LocatorKDTree` 模板无法编译。当前目录保留了
这两个依赖以及原始许可证，更新 CDT 时应同时更新其全部 `CDT/include` 头文件，不能只复制四个主头文件。

### 运行与验证

- `BuildFbxTerrainTin` 只接收有限且非负的几何误差；三角形预算是硬上限。预算不足以容纳边界网格时返回空结果，FBX 导出会回退到原有四叉树简化路径。
- 预算按每加入一个内部点最多增加两个三角形计算，并在每轮重建后用 CDT 的实际三角数确认，避免细化过程中超限。
- 运行 `FbxTerrainTriangulatorTest` 覆盖平面、孤立峰值、非法输入和三角预算；运行 `PostProcessFbxTest` 验证最终 FBX 的地形网格与接缝。

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
