# 高德 Web 矢量瓦片

## 现象

高德 Web 矢量瓦片不是现有栅格下载器使用的 Web Mercator 网格。服务返回的 PBF
还带有四字节大端长度和 gzip 信封，直接按标准 MVT 或裸 Protobuf 解析都会失败。

## 影响范围

`AmapVectorTileGrid`、`AmapVectorDownloader` 和 `AmapVectorDecoder` 处理高德 Web
矢量数据。调用方必须提供在自己授权范围内使用的 Web API key；`auth_key` 是每次
授权请求返回的短期签名，不应保存、硬编码或由经纬度推导。

## 解决方案

- 输入中心和半径采用 WGS84。先用大地线计算半径边界，再转换为 GCJ-02。
- 矢量瓦片索引为 `x=floor((lon+180)/360*2^z)`、
  `y=floor((90-lat)/180*2^z)`，不是 Web Mercator 的纬度投影。
- 半径只在客户端用于选取相交的完整瓦片。请求中的 `contain_range=2` 是服务端
  内容范围参数，不是米制半径，也不负责选择相邻瓦片。
- PBF 采用 Protobuf schema 解码，保留为 `AmapVectorDecodedData` 中间模型；GeoJSON
  仅为当前写出器，未来 Shapefile 或模型写出器应复用该模型。
- 输出坐标为 GCJ-02，并在 GeoJSON 顶层明确标注 `coordinateSystem: GCJ-02`。

## 验证方式

`AmapVectorGeoJsonTest` 以 25 米 WGS84 半径构造合成 gzip/PBF 信封，覆盖单瓦片选择、
POI 与建筑多边形解码、闭环和 GeoJSON 写出。实现阶段还用本地临时真实样本验证了当前
schema 能解析实际 PBF；该样本不作为正式测试依赖。

## 快速检查清单

- 新增图层字段时先更新 `proto/amap_vector_tile.proto`，再扩展中间要素模型。
- 若授权失败，检查 Web API key 和 `webApiVersion`，不要尝试复用过期 `auth_key`。
- 若位置整体偏移，先确认调用方没有把 GCJ-02 当作 WGS84 输入，也没有把输出误标为 WGS84。
- 若需要严格圆形边界，必须在中间要素模型层实现裁剪；当前行为是完整选中瓦片输出。
