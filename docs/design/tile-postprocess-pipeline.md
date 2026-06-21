# 瓦片后处理、去偏移与 UE 网格生成方案

## 背景

下载器完成最高层级瓦片下载后，需要增加两类后处理能力：

1. 重新整理瓦片金字塔，生成低层级瓦片、`meta.json` 和 `tilemapresource.xml`。
2. 将高德、腾讯等 GCJ02 源瓦片转换成去火星偏移后的 Google Mercator 瓦片，并按 4096 或 8192 大纹理生成 FBX 网格。

这里的“转换成 WGS84”定义为：输出瓦片仍使用 Google/XYZ 瓦片编号规则，瓦片代表的经纬度语义为 WGS84，不再带 GCJ02 火星偏移。元数据字段以 `GaoDeTiffTileDownloadComponent::StartDownloadGenerateGoogleTile` 为准。

## 参考实现

主要参考 `E:\Project\LunixTest\Plugins\GISToolLibrary` 中的实现：

- `GaoDeTiffTileDownloadComponent`：下载、去 GCJ02、合成 UE 大纹理、生成网格。
- `DomTilesGenerateComponent`：从 GeoTIFF 切 Google Mercator/TMS 瓦片、生成 `meta.json` 和 `tilemapresource.xml`。
- `GISToolGeoReferencing`：UE 世界坐标与经纬高之间的转换，支持 `Plane` 和 `Sphere`。
- `Tile` / `Point`：Google/TMS 瓦片和经纬度边界计算。

## 核心原则

### 分层架构

这一套能力必须兼容所有地图源，所以实现上应采用“父类工作流 + 子类地图适配”。

父类只负责通用流程：

- 任务初始化、目录组织、并发调度、缓存管理。
- 目标瓦片范围计算、低层级回推、4096/8192 纹理分组。
- 统一的 FBX 导出流程。
- 统一的 `meta.json` / `tilemapresource.xml` 写入流程。

子类只负责地图差异：

- 源瓦片矩阵是否是标准 Google/XYZ。
- 源服务是否带 GCJ02 / BD09 / 其他偏移。
- 占位图检测方式。
- 源瓦片 URL 规则和请求参数。
- 坐标反算、源瓦片采样、缺图修复策略。
- 元数据字段的最终映射方式。

如果服务器是标准 Google/XYZ 且没有火星偏移，父类可以直接走复制和 4 子瓦片合成路径。
如果服务器不是标准 Google/XYZ，或者带 GCJ02 偏移，则交由子类自行处理坐标和采样逻辑。

### 固定目标瓦片网格

后处理流程以目标瓦片网格为准，不以大图裁剪为准。这里的“目标瓦片网格”默认按 Google/XYZ / WGS84 这套口径描述，具体到某个地图源时，可以由子类提供自己的网格适配。

默认目标网格固定为：

- 瓦片矩阵：Google/XYZ。
- 元数据投影字段：按 `GaoDeTiffTileDownloadComponent` 写 `proj = 4326`。
- 经纬度语义：WGS84。
- 默认瓦片大小：256。
- 最高层级：下载任务指定的最高层级。

每个输出瓦片都从自己的 Google/XYZ 坐标反算 WGS84 经纬度范围，再反查源瓦片采样。这样输出范围和像素边界完全由目标网格决定，不依赖 GeoTIFF 裁剪时的四舍五入。

注意：这里的 `proj = 4326` 是现有高德组件的元数据约定，表示 `bounds` / `latLonBounds` 使用 WGS84 经纬度；它不改变瓦片编号仍使用 Google/XYZ 的事实。

### GeoTIFF/VRT 只作为辅助路径

大 GeoTIFF 或 VRT 可以作为调试输出或后续导出能力，但不作为主切片路径。原因是 GDAL 自动 warp/translate 时，如果 `extent`、`resolution`、像素外边界和瓦片矩阵没有完全对齐，容易产生半像素误差、边缘透明洞或少一行/一列。

如果未来提供 GeoTIFF/VRT 路径，必须显式指定：

- 输出范围 `te`。
- 输出分辨率 `tr`。
- 与瓦片矩阵一致的 origin。
- nodata/alpha 策略。
- 是否使用 target aligned pixels。

## 功能一：下载后重新切片

### 适用场景

源瓦片已经是 Google/XYZ 瓦片编号规则，并且不需要去 GCJ02 偏移。

### 流程

1. 读取下载任务元数据，得到最高层级、瓦片范围、瓦片大小、输出格式和下载区域 WGS84 bounds。
2. 将最高层级瓦片直接复制到新目录。
3. 从 `maxZoom - 1` 逐级向下生成低层级：
   - 父瓦片 `(x, y, z)` 对应子瓦片 `(2x, 2y)`, `(2x + 1, 2y)`, `(2x, 2y + 1)`, `(2x + 1, 2y + 1)`。
   - 子瓦片存在则参与合成，缺失则按透明像素处理。
   - 四个子瓦片都缺失时跳过父瓦片。
   - 使用 `INTER_AREA` 缩放到 256。
4. 生成 `meta.json`：
   - `bounds` / `latLonBounds` 使用 WGS84 经纬度。
   - `proj = 4326`，和 `GaoDeTiffTileDownloadComponent` 保持一致。
   - `tiletrans = "google"`。
   - `minzoom = 0`，`maxzoom = maxZoom`。
5. 生成 `tilemapresource.xml`：
   - `GaoDeTiffTileDownloadComponent::StartDownloadGenerateGoogleTile` 当前只写 `meta.json`，没有写 `tilemapresource.xml`。
   - 本工具如果补充生成 XML，应作为兼容文件单独处理，不能反向修改 `meta.json` 的 `proj = 4326` 约定。
   - 对 Google/XYZ 输出，XML 按 WMTS 兼容语义写 `SRS = EPSG:3857`、`profile = global-mercator`，BoundingBox 写 Web Mercator 米制坐标。

### 边界策略

最高层级范围以下载任务记录的 tile range 为准，不重新通过经纬度推算一遍，避免不同算法造成范围偏差。

低层级范围由最高层级范围按父瓦片回推，保证只覆盖实际存在的瓦片范围。

## 功能二：GCJ02 去偏移并生成 WGS84 语义的 Google Mercator 瓦片

### 适用场景

源瓦片来自高德、腾讯等 GCJ02 服务。源 URL 使用 Google/XYZ 或服务自己的瓦片路径，但瓦片内容带 GCJ02 偏移。

### 输出定义

输出仍为 Google/XYZ 瓦片：

- 文件组织：`{z}/{x}/{y}.png`。
- 瓦片边界：按 `Tile::fromGoogle(...).bounds(TileSize)` 计算。
- 经纬度：WGS84。
- 图像内容：从源 GCJ02 瓦片反采样得到。

### 采样流程

对每个目标瓦片 `(x, y, z)`：

1. 根据 Google/XYZ 瓦片坐标计算目标瓦片 WGS84 经纬度范围。
2. 对目标像素生成采样点。默认使用像素中心：
   - `u = (px + 0.5) / tileSize`
   - `v = (py + 0.5) / tileSize`
3. 将目标像素点按 Google/XYZ 瓦片边界反算为 WGS84 经纬度。
4. 如果源是 GCJ02，将 WGS84 转为 GCJ02。
5. 根据源经纬度定位源瓦片和源像素。
6. 从源瓦片缓存读取像素，使用双线性或邻近采样写入目标瓦片。
7. 源瓦片缺失或占位时，按现有低层级修复逻辑先修复源缓存，再参与采样。

### 性能策略

不逐像素重复解码图片。每个目标瓦片先计算需要访问的源瓦片集合，再批量读取到内存缓存。

OpenCV 可使用 `remap`：

- 先为目标瓦片生成 source map。
- 源瓦片按需要拼成一个临时小图。
- `remap` 一次完成采样。

同一源瓦片在多个目标瓦片中复用时，走磁盘缓存和进程内 LRU 缓存。

## 功能三：请求范围合并大图

### 适用场景

当用户只需要当前下载半径覆盖范围的一张整图时，不需要扫描缓存目录里的全部瓦片，也不需要按圆形半径裁剪。输出范围固定为本次任务计算出的 `MinTile..MaxTile` 方形瓦片范围。

### 输出定义

- 默认输出路径：`GetPostProcessRootPath()/merged.png`。
- 默认输出目标最大边：`16384`。
- 可通过 `SetPostProcessMergedImageEnabled(true)` 启用。
- 可通过 `SetPostProcessMergedImageResolution(...)` 设置输出目标最大边。
- 输出保持瓦片范围原始宽高比；真实范围像素数小于目标最大边时不放大。
- 可通过 `SetPostProcessMergedImageOutputPath(...)` 覆盖输出路径。

### 生成流程

1. 根据 `MinTile..MaxTile` 的最高层级范围和目标最大边，自动选择一个采样层级。
   - 采样层级的像素规模尽量贴近最终输出分辨率。
   - 例如最高层级是 19，但 16K 输出只需要 16 或 17 级精度时，不再遍历 19 级全部瓦片。
2. 使用 `BuildRequestedTilesAtZoom(sampleZoom)` 取得采样层级瓦片，不遍历缓存目录。
3. 只有完整落在本次最高层级请求范围内的采样瓦片才写入后处理目录，避免边缘部分瓦片污染后续请求的通用缓存。
4. 如果可缓存的采样层级后处理瓦片已经存在且可解码，直接复用硬盘缓存。
5. 如果可缓存的采样层级瓦片不存在，则从下载任务的最高层级源缓存反采样生成该层级后处理瓦片。
   - 标准 Google/XYZ 且无偏移时，同层级可以直接复制。
   - GCJ02 源仍通过子类的 `ConvertTargetLonLatToSourceLonLat()` 把目标 WGS84 经纬度转换到源瓦片坐标后采样。
6. 合并阶段以采样层级瓦片为单位写入最终大图。
   - 每个采样瓦片按它和最高层级请求范围的交集裁剪。
   - 边缘采样瓦片只在内存中临时生成，不写入通用后处理缓存。
   - 裁剪结果直接 `cv::resize` 到最终大图 ROI，缩小时使用 `INTER_AREA`，放大时使用 `INTER_LINEAR`。
7. 不做圆形 mask，范围外缓存瓦片不会参与合并。

## 4096/8192 大纹理生成

### 纹理分组

`TextureTileSize` 必须是 `TileSize` 的整数倍：

- 4096 对应 `16 x 16` 个 256 瓦片。
- 8192 对应 `32 x 32` 个 256 瓦片。

生成方式：

1. 在最高层级输出瓦片目录上按 group size 分组。
2. 每组生成一张大纹理。
3. 大纹理的经纬度范围由组内瓦片外边界决定。
4. 缺失瓦片填透明，整组都缺失则不生成。

分组可以提供两种模式：

- `TileRangeAligned`：按瓦片范围从左上角对齐分组，结果稳定，适合离线输出。
- `CenterAligned`：按下载中心向四周分组，兼容 `GaoDeTiffTileDownloadComponent` 现有思路，适合和 UE 旧流程对齐。

默认建议使用 `TileRangeAligned`，旧工程兼容场景再使用 `CenterAligned`。

### 大纹理元数据

每个大纹理记录：

- 文件路径。
- 覆盖的最高层级瓦片范围。
- WGS84 经纬度范围：west、south、east、north。
- zoom。
- texture size。
- tile size。

这些信息同时用于 FBX 网格生成和后续高度图绑定。

## FBX 网格生成

FBX 网格生成不直接写死平面或球面算法，而是抽象为：

```text
WGS84 lon/lat/alt -> FGISToolGeoReferencingData -> UE position
```

也就是说，大纹理只提供 WGS84 bounds 和 UV；具体落到 UE 中是平面还是球面，由 `FGISToolGeoReferencingData::PlanetShape` 决定。

### GeoReferencing 参数

需要保存或传入以下字段：

- `OriginLongitude`
- `OriginLatitude`
- `OriginAltitude`
- `PlanetShape`
- `ProjectedCRS`
- `WorldOriginLocation`

`Plane` 模式：

1. WGS84 经纬高通过 `ProjectedCRS` 反向转换到投影坐标。
2. 减去投影坐标原点。
3. 米转 UE 厘米，Y 取反。

`Sphere` 模式：

1. WGS84 经纬高转 ECEF，EPSG:4978。
2. 用中心点构建 East/North/Up 局部坐标框架。
3. ECEF 转局部世界坐标。
4. 米转 UE 厘米，Y 取反。
5. 减去 `WorldOriginLocation`。

### 平面模式网格

平面模式默认可以用四角 quad：

- NW -> UV `(0, 0)`
- NE -> UV `(1, 0)`
- SE -> UV `(1, 1)`
- SW -> UV `(0, 1)`

如果未来绑定高度图，或单张纹理覆盖范围较大，也可以开启细分网格。细分后每个顶点仍通过 `FGISToolGeoReferencingData` 转换，避免和球面路径出现两套坐标算法。

### 球面模式网格

球面模式不建议只用四角 quad。四角 quad 的边在 UE 空间中会变成直线弦，覆盖范围大时会明显偏离椭球曲面。

球面模式默认生成细分网格：

- 默认细分数可按 `MeshTileSize` 或配置项指定，例如 64。
- 对每个顶点按经纬度范围插值：
  - `lon = lerp(west, east, u)`
  - `lat = lerp(north, south, v)`
  - `alt = 0`，后续可由高度图替换。
- 顶点位置统一调用 `LongitudeLatitudeAltitudeToUELocation`。
- UV 使用同一个 `u/v`。

高度图兼容时，只需要在顶点生成阶段把 `alt = 0` 替换为高度采样值。纹理分组和经纬度范围不需要改变。

## 输出结构建议

```text
output/
  tiles/
    0/
    ...
    {maxZoom}/
  textures/
    4096/
      {groupX}_{groupY}.png
      {groupX}_{groupY}.json
    8192/
      {groupX}_{groupY}.png
      {groupX}_{groupY}.json
  mesh/
    plane/
      tiles.fbx
    sphere/
      tiles.fbx
  meta.json
  tilemapresource.xml
```

如果只生成一种 `PlanetShape`，则 `mesh/` 下只输出对应结果。

## 验证计划

1. 用小范围下载结果验证最高层级复制和低层级生成。
2. 对 GCJ02 源图执行去偏移，检查道路/建筑是否和 WGS84 底图对齐。
3. 对输出瓦片做完整性扫描：
   - 文件存在。
   - 图片可解码。
   - 不是已知占位图。
   - 尺寸等于 tile size。
4. 用 4096 和 8192 分别生成大纹理，检查边界无缝。
5. 生成 Plane 和 Sphere 两套 FBX：
   - 平面模式检查局部尺寸是否和投影米制距离一致。
   - 球面模式检查大范围边缘是否贴合曲面，不出现直线跨越。
6. 和 `GaoDeTiffTileDownloadComponent` 在同一范围下对比：
   - tile range。
   - texture bounds。
   - mesh bounds。

## 待确认项

1. FBX 是否需要每张大纹理一个 mesh，还是合并成单个 FBX 多个 mesh node。
2. Plane/Sphere 是否一次都生成，还是按参数选择。
3. 大纹理分组默认使用 `TileRangeAligned` 还是兼容旧逻辑的 `CenterAligned`。
4. 高度图接口先预留字段，还是这次只保留设计不落代码。

## 参考资料

- GDAL Geotransform: https://gdal.org/en/stable/tutorials/geotransforms_tut.html
- GDAL gdalwarp: https://gdal.org/en/stable/programs/gdalwarp.html
- OSGeo TMS Specification: https://wiki.osgeo.org/wiki/Tile_Map_Service_Specification
