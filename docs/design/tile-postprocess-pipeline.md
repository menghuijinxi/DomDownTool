# 瓦片后处理、去偏移与 UE 网格生成方案

## 背景

下载器完成最高层级瓦片下载后，需要增加两类后处理能力：

1. 重新整理瓦片金字塔，生成低层级瓦片、`meta.json` 和 `tilemapresource.xml`。
2. 将高德、腾讯等 GCJ02 源瓦片转换成去火星偏移后的 Google Mercator 瓦片，并按目标贴图大小生成中心锚定贴图和 FBX 网格。

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
- 目标瓦片范围计算、低层级回推、中心锚定 FBX 纹理分组。
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

## FBX 贴图生成

### 输入参数

FBX 导出使用独立的贴图参数，不复用请求范围合并大图的 `PostProcessMergedImageResolution`。

- `FbxTextureSize`：目标贴图大小，默认 `4096`。
- `FbxTextureSize` 必须是 2 的次幂，且不能大于 `16384`。
- `FbxTextureSize` 必须大于等于 `TileSize`，并且应是 `TileSize` 的整数倍。
- `TextureDistanceOffsets`：距离贴图降级参数，兼容 `GaoDeTiffTileDownloadComponent` 里的 `FTiffTileLoad_TextureDistanceOffset` 语义。
- `MeshTileSize`：网格细分数，默认沿用现有设计里的 64。

如果用户传入非法贴图大小，流程应直接报错并停止 FBX 导出，不隐式向上或向下修正。显卡兼容性以 16K 为硬上限。

### 前置瓦片生成

FBX 贴图依赖后处理后的最高层级瓦片，原因是这一步已经完成地图源适配和 GCJ02 去偏移：

1. 构建下载任务指定最高层级 `Zoom` 的目标瓦片范围。
2. 调用后处理最高层级生成逻辑，生成 `Zoom/x/y.png`。
3. FBX 导出只需要最高层级后处理瓦片，不生成低层级金字塔。
4. 如果用户同时启用了普通金字塔瓦片输出，再由普通后处理流程单独生成低层级。

这样 FBX 贴图合并可以复用标准地图、GCJ02 地图、腾讯/高德占位图修复等已有逻辑，同时避免为了 FBX 额外生成低层级瓦片造成大量耗时。

### 中心锚定分组

FBX 贴图分组不按下载范围左上角对齐，而是以项目中心为第一张贴图的锚点。参考 `GaoDeTiffTileDownloadComponent::GetGenerateTextureInfo()`，但这里要明确采用中心像素语义：

1. 将项目中心 WGS84 经纬度转换到最高层级 Google/XYZ 全局像素坐标：
   - `centerPixelX`
   - `centerPixelY`
   - `centerTileX = floor(centerPixelX / TileSize)`
   - `centerTileY = floor(centerPixelY / TileSize)`
2. 第一张贴图的逻辑坐标为 `(0, 0)`。
3. 第一张贴图覆盖的全局像素范围为：
   - `minPixelX = centerPixelX - FbxTextureSize / 2`
   - `minPixelY = centerPixelY - FbxTextureSize / 2`
   - `maxPixelX = minPixelX + FbxTextureSize`
   - `maxPixelY = minPixelY + FbxTextureSize`
4. 因此项目中心点落在第一张贴图的中心像素位置。
5. 其他贴图从中心向外辐射，逻辑坐标 `(gx, gy)` 的像素范围为：
   - `minPixelX = centerPixelX + gx * FbxTextureSize - FbxTextureSize / 2`
   - `minPixelY = centerPixelY + gy * FbxTextureSize - FbxTextureSize / 2`
6. 生成顺序按 `ring = max(abs(gx), abs(gy))` 从小到大遍历，便于后续按距离提前停止或输出进度。

这里不强制贴图边界对齐 256 瓦片边界。`centerPixelX/Y` 可以是小数，采样时以全局像素坐标为准，不先取整到瓦片边界。每张 FBX 贴图从最高层级后处理瓦片中按全局像素范围采样，边缘可以跨瓦片裁剪。这样才能保证项目中心严格落在中心贴图的中心像素，而不是只落在中心瓦片附近。

### 覆盖范围

FBX 贴图覆盖下载任务的方形瓦片范围，不做圆形裁剪：

1. 先用下载半径和最高层级得到 `MinTile..MaxTile`。
2. 将 `MinTile..MaxTile` 转换为最高层级全局像素范围。
3. 从中心贴图开始向外枚举贴图组。
4. 与下载范围没有交集的贴图组跳过。
5. 与下载范围有交集但超出下载范围的部分填透明。
6. 整张贴图没有任何有效像素时不输出。

贴图的 WGS84 bounds 由它覆盖的全局像素范围反算，而不是由参与采样的瓦片外边界推导。这样纹理、网格和 UV 的边界完全一致。

### 距离降级

`TextureDistanceOffsets` 用来减少远距离贴图的尺寸，降低硬盘和显存占用。语义沿用参考脚本：

- `Distance`：距离阈值，单位厘米，使用贴图网格 bounds 的 UE 空间中心到项目中心的距离。
- `Offset`：贴图降级级别，`0` 表示不降级，`1` 表示边长减半，`2` 表示边长变为四分之一。

处理规则：

1. 对每张贴图，用其四角 WGS84 bounds 通过 `FGISToolGeoReferencingData` 转换到 UE 坐标。
2. 计算四角 bounds 的中心点到项目中心的距离。
3. 在所有 `Distance <= 当前距离` 的规则中选择 `Distance` 最大的一条。
4. 输出贴图边长为 `FbxTextureSize >> Offset`。
5. 输出边长最低不小于 `128`，且必须保持 2 的次幂。
6. 降级只改变输出 PNG 的像素尺寸，不改变该贴图覆盖的地理范围、UV 和 mesh bounds。

### FBX 贴图元数据

每张 FBX 贴图记录：

- 文件路径。
- 逻辑贴图坐标：`gridX`、`gridY`，中心贴图为 `(0, 0)`。
- `ring = max(abs(gridX), abs(gridY))`。
- 项目中心瓦片：`centerTileX`、`centerTileY`。
- 最高层级全局像素范围。
- 覆盖的最高层级瓦片范围。
- WGS84 经纬度范围：west、south、east、north。
- `zoom`。
- `tileSize`。
- `sourceTextureSize`，即 `FbxTextureSize`。
- `outputTextureSize`，即距离降级后的实际 PNG 边长。
- `distanceOffset`。
- 是否含透明边缘或部分超出下载范围。

这些信息同时用于 FBX 网格生成、后续高度图绑定和增量重跑。

## FBX 网格生成

FBX 网格生成不直接写死平面或球面算法，而是抽象为：

```text
WGS84 lon/lat/alt -> FGISToolGeoReferencingData -> UE position
```

也就是说，FBX 贴图只提供 WGS84 bounds 和 UV；具体落到 UE 中是平面还是球面，由 `FGISToolGeoReferencingData::PlanetShape` 决定。

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

### FBX 输出组织

默认输出单个 FBX，内部每张贴图对应一个 mesh node：

- node 名称使用 `tile_{gridX}_{gridY}`。
- 每个 node 绑定自己的材质和纹理路径。
- 中心贴图 node 为 `tile_0_0`。

如果后续发现 DCC 或 UE 导入单个超大 FBX 的性能较差，可以增加“每张贴图一个 FBX”的输出模式，但不作为默认方案。

## 输出结构建议

```text
output/
  tiles/
    0/
    ...
    {maxZoom}/
  textures/
    fbx/
      4096/
        manifest.json
        {gridX}_{gridY}.png
        {gridX}_{gridY}.json
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
4. 用 4096、8192、16384 分别生成 FBX 贴图，检查中心贴图锚定、边界无缝和 16K 上限校验。
   - `0_0` 贴图中心像素反算经纬度应等于项目中心。
   - `centerTileX/Y` 必须是包含项目中心的最高层级瓦片。
5. 生成 Plane 和 Sphere 两套 FBX：
   - 平面模式检查局部尺寸是否和投影米制距离一致。
   - 球面模式检查大范围边缘是否贴合曲面，不出现直线跨越。
6. 检查 `TextureDistanceOffsets`：
   - 距离阈值命中规则是否正确。
   - 降级贴图尺寸是否为 2 的次幂。
   - 降级不改变 mesh bounds 和 UV。
7. 和 `GaoDeTiffTileDownloadComponent` 在同一范围下对比：
   - tile range。
   - texture bounds。
   - mesh bounds。

## 待确认项

1. FBX 默认单文件多 mesh node；是否还需要补充“每张贴图一个 FBX”的兼容模式。
2. Plane/Sphere 是否一次都生成，还是按参数选择。
3. 高度图接口先预留字段，还是这次只保留设计不落代码。

## 参考资料

- GDAL Geotransform: https://gdal.org/en/stable/tutorials/geotransforms_tut.html
- GDAL gdalwarp: https://gdal.org/en/stable/programs/gdalwarp.html
- OSGeo TMS Specification: https://wiki.osgeo.org/wiki/Tile_Map_Service_Specification
