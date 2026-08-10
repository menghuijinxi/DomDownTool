# 高德 GIS 下载与 PNG 渲染

`AmapGisDownload.exe` 用于下载高德 Web 矢量数据，将选定类别渲染为透明背景 PNG
瓦片，并继续使用项目已有的金字塔和合并大图后处理。生成的标准 XYZ 瓦片也兼容已有
FBX 后处理，但当前命令行工具没有直接启用 FBX 的参数，需要通过 C++ 接口配置。本工具
不依赖已暂停维护的 UI 模块。

## 构建

在已配置 MSVC 和 Ninja 的开发终端中执行：

```powershell
cmake --preset x64-release
cmake --build ..\out\DomDownTool\build\x64-release --config Release
```

可执行文件位于：

```text
..\out\DomDownTool\build\x64-release\AmapGisDownload.exe
```

进入该目录后可查看程序内置帮助：

```powershell
.\AmapGisDownload.exe --help
```

调用方应使用自己有权使用的高德 Web API Key，并通过 `--key` 传入。输入中心坐标使用
WGS84，经纬度顺序分别为 `--lon`、`--lat`。

## 快速开始

以下命令以 WGS84 坐标 `116.41, 39.92` 为中心，下载半径 2 公里的 z16 水体，使用蓝色
渲染，只保留合并大图：

```powershell
.\AmapGisDownload.exe `
    --lon 116.41 `
    --lat 39.92 `
    --radius 2000 `
    --zoom 16 `
    --types water `
    --color "water=#2D8CFF" `
    --key "<你的 Web API Key>" `
    --no-geojson `
    --merged-only
```

如果省略中心、半径、层级和类型，程序默认使用北京中心、半径 2000 米、z16，并且只
渲染水体。

后续示例为突出功能参数省略了 `--key`，实际联网下载时仍应追加自己的 Web API Key。

## 常用命令

### 输出 JPG 合并大图

```powershell
.\AmapGisDownload.exe `
    --types water --zoom 16 --radius 2000 `
    --no-geojson --merged-only `
    --merged-format jpg
```

`--merged-format` 支持 `png` 和 `jpg`，默认是 `png`。PNG 保留透明背景；JPG 不支持
透明度，透明区域会变为黑色背景。该参数只影响合并大图，临时瓦片和金字塔瓦片仍使用
PNG。

### 建筑按高度着色

```powershell
.\AmapGisDownload.exe `
    --lon 116.41 --lat 39.92 `
    --radius 2000 --zoom 16 `
    --types building `
    --building-height-colors `
    --no-geojson --merged-only
```

相同原始高度值始终使用相同颜色，不同高度使用不同颜色。高度为 0 或没有高度属性的
建筑使用普通建筑颜色。可以同时传入 `--color "building=#808080"` 设置这类建筑的
回退颜色。高德没有公开 `height` 的单位，程序不会擅自换算为米或楼层。

### 同时渲染道路、建筑和水体

```powershell
.\AmapGisDownload.exe `
    --lon 116.41 --lat 39.92 `
    --radius 5000 --zoom 16 `
    --types road,building,water `
    --color "road=#F0C641" `
    --color "building=#808080" `
    --color "water=#2D8CFF"
```

`--color` 可以重复。格式为 `类型=#RRGGBB` 或 `类型=#RRGGBBAA`，末尾两位是透明度。

### 渲染全部类别

```powershell
.\AmapGisDownload.exe `
    --lon 116.41 --lat 39.92 `
    --radius 5000 --zoom 16 `
    --types all
```

`--types all` 表示选择全部类别，但仍遵守每个要素的 `minZoom/maxZoom`。只有明确需要
忽略高德的显示层级规则时，才额外增加：

```powershell
--render-all-zooms
```

该参数可能渲染在线地图在当前层级不会显示的要素，因此不作为默认行为。
它只改变已下载要素的过滤方式，不会请求额外的 PBF 数据层级。

### 下载 POI 等标注数据

```powershell
.\AmapGisDownload.exe `
    --lon 116.41 --lat 39.92 `
    --radius 2000 --zoom 16 `
    --types point `
    --labels
```

标注图层默认不下载。需要 POI 等标注数据时必须增加 `--labels`。

### 自定义输出和缓存目录

```powershell
.\AmapGisDownload.exe `
    --types building --zoom 16 --radius 2000 `
    --output "D:\map-output" `
    --raw-cache "D:\map-cache\pbf" `
    --png-cache "D:\map-cache\temporary-png"
```

`--output` 只修改最终输出根目录，不会同时修改 PBF 和临时 PNG 目录。需要移动缓存时，
必须分别设置 `--raw-cache` 和 `--png-cache`。

## 输出目录

未覆盖路径参数时，目录结构如下：

```text
cache\高德地图\gis数据\
├─ pbf缓存\
│  ├─ 基础图层\数据层级\x\y.pbf
│  └─ 标注图层\数据层级\x\y.pbf
├─ 临时\显示层级\x\y.png
└─ 输出\
   ├─ 数据.geojson
   ├─ 合并大图.png 或 合并大图.jpg
   └─ 瓦片\显示层级\x\y.png
```

所有相对路径都以启动程序时的当前工作目录为基准。如果在构建输出目录中运行 EXE，默认
`cache` 也会创建在构建输出目录；需要固定位置时应传入绝对路径。

- `pbf缓存` 是唯一会跨运行复用的数据。缓存损坏时会重新下载。
- `临时` 中的 PNG 每次运行都会重写，不能作为最终输出使用。
- 金字塔和合并大图会根据本次输入重新生成，不复用旧结果；通过 C++ 启用 FBX 时同样
  每次重新生成。
- `--merged-only` 不生成最终 `瓦片` 目录，只保留所选格式的合并大图；合并过程仍会在
  临时目录生成采样瓦片。
- `--no-geojson` 关闭 `数据.geojson`。当前命令行工具始终执行 PNG 渲染和合并大图，
  暂不提供只导出 GeoJSON 的参数。

## 类型与默认颜色

`--types` 支持以下值，多个值使用英文逗号分隔：

| 类型 | 含义 | 默认颜色 |
| --- | --- | --- |
| `water` | 水面和已识别的水系线 | 绿色 |
| `building` | 建筑面 | 灰色 |
| `road` | 道路线 | 黄色 |
| `point` | 点要素和 POI | 红色 |
| `line` | 非道路普通线 | 蓝灰色 |
| `transit` | 公交、轨道交通相关要素 | 紫色 |
| `other` | 未映射到上述类别的要素 | 蓝灰色 |
| `all` | 选择全部类别 | 按各类别颜色 |

高德没有公开稳定的 `mainKey/subKey` 分类码表。程序内置了当前已验证的水体编码；如果
实际数据发生变化，可以重复使用 `--water-code <mainKey:subKey>`，把其他编码显式加入
水体分类：

```powershell
.\AmapGisDownload.exe `
    --types water `
    --water-code 30001:2 `
    --water-code 20014:1
```

可以先保留 GeoJSON，检查要素属性中的 `layer`、`mainKey` 和 `subKey`，再决定是否使用
精确水体编码。

## 层级规则

`--zoom` 表示最终 PNG、金字塔和合并大图的显示层级。高德 PBF 只在部分数据层级提供
有效内容，程序会自动映射：

| PNG 显示层级 | PBF 数据层级 |
| --- | --- |
| z2-z5 | z3 |
| z6-z7 | z6 |
| z8-z9 | z8 |
| z10-z12 | z10 |
| z13-z14 | z12 |
| z15 及以上 | z14 |

因此 z16、z17、z18 输出会下载 z14 PBF，再按指定显示层级渲染，不应直接把高显示层级
当作 PBF 请求层级。控制台完成信息会同时打印显示层级和实际 PBF 数据层级。

下载半径用于选择与范围相交的完整瓦片，不会把几何严格裁剪成圆形，所以最终图片边界可能
略大于设定半径。

## 参数说明

| 参数 | 说明 |
| --- | --- |
| `--lon <经度>` | WGS84 中心经度 |
| `--lat <纬度>` | WGS84 中心纬度 |
| `--radius <米>` | 下载半径，不限制最大范围 |
| `--zoom <层级>` | 最终 PNG 显示层级，PBF 层级自动映射 |
| `--types <列表>` | 输出类别，使用英文逗号分隔 |
| `--color <类型=颜色>` | 类别颜色，可重复 |
| `--building-height-colors` | 建筑按原始高度值稳定着色 |
| `--render-all-zooms` | 忽略所有要素的 `minZoom/maxZoom` |
| `--water-code <main:sub>` | 将指定内部分类码显式映射为水体，可重复 |
| `--labels` | 同时下载 POI 等标注图层 |
| `--key <Key>` | 高德 Web API Key |
| `--api-version <版本>` | 覆盖高德 Web JSAPI 版本 |
| `--output <目录>` | 最终 GeoJSON、合并大图和瓦片根目录 |
| `--geojson <文件>` | 覆盖 GeoJSON 输出文件 |
| `--no-geojson` | 不输出 GeoJSON |
| `--merged-only` | 不生成最终金字塔目录 |
| `--raw-cache <目录>` | 覆盖原始 PBF 缓存目录 |
| `--png-cache <目录>` | 覆盖每次重写的临时 PNG 目录 |
| `--concurrency <数量>` | PBF、PNG 和后处理并发数；0 表示按硬件线程数自动选择 |
| `--merged-resolution <像素>` | 合并大图最大边，默认 16384，另一边保持比例 |
| `--merged-format <png\|jpg>` | 合并大图格式，默认 PNG；JPG 不保留透明度 |
| `-h`, `--help` | 显示内置帮助 |

## C++ 调用

命令行工具内部使用 `AmapVectorPngDownloader`。直接从 C++ 调用时，最小配置如下：

```cpp
#include <filesystem>

#include "AmapVectorPngDownloader.h"

DownTool::AmapVectorPngDownloadOptions options;
options.vectorDownload.centerWgs84 = { 116.41, 39.92 };
options.vectorDownload.radiusMeters = 2000.;
options.vectorDownload.zoom = 16;
options.vectorDownload.webApiKey = "<你的 Web API Key>";
options.vectorDownload.includeBaseLayers = true;
options.vectorDownload.includeLabels = false;
options.vectorDownload.geoJsonOutputPath =
    std::filesystem::u8path("cache/高德地图/gis数据/输出/数据.geojson");

options.render.categories = {
    DownTool::AmapVectorRenderCategory::Building
};
options.render.colorBuildingsByHeight = true;
options.render.pngTileCachePath =
    std::filesystem::u8path("cache/高德地图/gis数据/临时");

DownTool::AmapVectorPngDownloader downloader;
downloader.SetPostProcessEnabled(false);
downloader.SetPostProcessOutputPath("cache/高德地图/gis数据/临时/大图采样");
downloader.SetPostProcessMergedImageEnabled(true);
downloader.SetPostProcessMergedImageResolution(16384);
downloader.SetPostProcessMergedImageOutputPath(
    "cache/高德地图/gis数据/输出/合并大图.png");
downloader.SetMaxDownQueue(options.render.maxConcurrency);

const DownTool::AmapVectorPngDownloadResult result =
    downloader.DownloadPngTiles(options);
if (!result.success)
{
    // result.error 包含下载、解码、渲染或后处理错误。
}
```

C++ 接口通过输出路径扩展名选择格式。需要 JPG 时，将上面的路径改为
`cache/高德地图/gis数据/输出/合并大图.jpg`。

应用程序应在调用下载器前完成 libcurl 全局初始化，并在所有下载器对象销毁后执行清理。
如果只需要解码数据或 GeoJSON，可以直接使用 `AmapVectorDownloader::Download()` 或
`DownloadGeoJson()`，不经过 PNG 渲染器。

需要同时生成 FBX 时，在 `DownloadPngTiles()` 之前增加：

```cpp
downloader.SetPostProcessFbxEnabled(true);
downloader.SetFbxOutputPath("cache/高德地图/gis数据/输出/fbx");
downloader.SetFbxTextureSize(4096);
downloader.SetFbxShape(DownTool::FbxPlanetShape::Plane);
```

FBX 会读取本次重新渲染的 PNG，不会复用上一次运行留下的贴图。

更底层的协议、分类和缓存设计见[高德 Web 矢量瓦片](knowledge/amap-vector-tile.md)。
