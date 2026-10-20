# 模型坐标与高程基准

## 不同范围任务的模型高度错层

### 现象与影响

相同中心先导出内圈高精度地形，再扩大范围导出外围低精度地形时，
即使使用相同 DEM，模型也可能整体上下错位。地形、带高程建筑、
旧版平铺导出和分组组件导出均受影响；负海拔还可能被截平。

### 根因与定位

水平坐标已经使用中心经纬度定位，但预览任务为 FBX 指定了当前范围最低高程作为高度基准。
范围扩大后最低点可能变化，导致相同地理点的局部 Z 不同。
地形采样和建筑基础采样又使用 `max(0, elevation - reference)`，把负值截为 0。

排查时沿 `ExportTaskManager::RunExport` 和 `RunGroupedExport` 查找
`SetFbxElevationReferenceMeters`、`ApplyBuildingFbxElevation`，
再追踪 `DownToolBase` 地形高度场及 `SampleBuildingTerrainHeightRange`。
同时检查 `高度/meta.json` 的 `referenceMode` 和 `referenceElevationMeters`，
不能只看水平中心是否一致。

### 修正与兼容

- 预览 FBX 固定使用海拔 0，模型原点由中心经纬度与海拔 0 决定。
- DEM 使用原有 EGM2008 海拔，地形和跟随地形的建筑保留有符号高程。
- 元数据写出 `referenceMode: sea-level`、`referenceElevationMeters: 0`，
  `verticalDatum` 仍为 `EGM2008`。
- 球面模型继续在同一中心的零高程曲面上叠加高程，保持既有曲率处理。
- 底层显式指定参考高程的 API 保留，允许调用方自行指定偏移；预览任务不再自动取最低值。
- Cesium 继续使用已有的高程偏移和大地水准面转换配置；不能把 EGM2008 海拔与
  WGS84 椭球高混为一谈。
- 已导出的模型不会自动改写。旧模型若元数据为 `referenceMode: minimum`，
  应重新导出后再与新模型拼接。

统一原点解决的是整体坐标偏移。不同层级的 DEM 采样间距、网格简化误差仍可能造成
接缝处的局部高度差，不能据此认为低精度与高精度网格的边界顶点必然重合。

### 验证与复查

`ExportTaskManagerTest` 使用离线 DEM，分别验证 20 公里 / z18 与 40 公里 / z15。
两种范围的最低高程分别为 150 米和 25 米，旧式任务与分组任务仍须输出相同的 0 米基准。
测试缓存隔离在 `tmp` 内，不使用真实缓存或在线数据。

`PostProcessFbxTest` 与 `AmapBuildingChunkTest` 对平面和球面分别检查
0、150、-35.5 米高程，验证顶点相对零高程模型的高度偏移。
三个测试均先在旧实现上失败，再验证修复通过。

再次遇到多任务错位时，依次检查中心、模型形态、高程基准、垂直基准、
旧产物元数据，最后检查边界采样与简化误差。
