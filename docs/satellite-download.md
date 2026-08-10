# 卫星图和栅格地图命令行下载

`DomDownTool.exe` 用于下载卫星图、电子地图或透明路网，并按需输出原始瓦片、标准
Web Mercator 金字塔、合并大图或 FBX。程序不依赖已经暂停维护的 UI 模块。

## 构建

在已配置 MSVC 和 Ninja 的开发终端中执行：

```powershell
cmake --preset x64-release
cmake --build ..\out\DomDownTool\build\x64-release --config Release
```

可执行文件位于：

```text
..\out\DomDownTool\build\x64-release\DomDownTool.exe
```

在该目录运行 `.\DomDownTool.exe --help` 可以查看内置参数说明。所有相对缓存和输出
路径都以启动程序时的当前工作目录为基准。

## 快速开始

下载高德 z16 卫星图，中心为 WGS84 坐标 `116.41, 39.92`，半径 2 公里，只输出合并
PNG 大图：

```powershell
.\DomDownTool.exe `
    --provider gaode `
    --lon 116.41 --lat 39.92 `
    --radius 2000 --zoom 16
```

这些参数也是程序默认值。增加 `--dry-run` 可以只检查并打印配置，不发起下载。

## 地图源

| 参数值 | 内容 | 默认原始缓存目录 |
| --- | --- | --- |
| `gaode` | 高德卫星，也支持电子地图和透明路网 | `cache/高德地图` |
| `baidu` | 百度卫星 | `cache/百度地图` |
| `tencent` | 腾讯卫星 | `cache/腾讯地图` |
| `tianditu` | 天地图影像 | `cache/天地图` |
| `google` | Google 中国卫星图，默认使用项目已有本地代理 | `cache/Google地图` |
| `bing` | Bing 卫星图 | `cache/Bing地图` |
| `arcgis` | ArcGIS World Imagery | `cache/ArcGIS地图` |

所有输入中心坐标统一使用 WGS84。百度、高德、腾讯和 Google 中国图层的坐标纠偏仍由
各自下载器处理。

高德可通过 `--map-type` 切换类型：

```powershell
# 电子地图
.\DomDownTool.exe --provider gaode --map-type electronic

# 透明路网
.\DomDownTool.exe --provider gaode --map-type road-network
```

`--map-type` 只适用于高德。其他地图源目前都是固定卫星影像。

## 输出模式

使用 `--output-mode` 选择结果：

| 模式 | 输出内容 |
| --- | --- |
| `raw` | 只保留地图源原始瓦片 |
| `merged` | 原始瓦片和合并大图，默认模式 |
| `pyramid` | 原始瓦片和标准 Web Mercator 金字塔 |
| `both` | 原始瓦片、标准金字塔和合并大图 |
| `fbx` | 原始瓦片以及 FBX 贴图和网格 |

`--merged-only` 等同于 `--output-mode merged`，`--raw-only` 等同于
`--output-mode raw`。

例如输出 JPG 合并大图：

```powershell
.\DomDownTool.exe `
    --provider baidu `
    --lon 116.74 --lat 39.95 `
    --radius 20000 --zoom 16 `
    --merged-only --merged-format jpg
```

PNG 保留透明度；JPG 不支持透明度。`--merged-format` 只影响合并大图，原始瓦片和
金字塔仍使用下载器既有格式。

生成标准金字塔和合并大图：

```powershell
.\DomDownTool.exe --provider tencent --output-mode both
```

生成平面 FBX：

```powershell
.\DomDownTool.exe `
    --provider arcgis `
    --output-mode fbx `
    --fbx-texture-size 4096 `
    --fbx-shape plane
```

## 缓存和输出目录

默认沿用已有目录结构：

```text
cache\地图源目录\
├─ z\x\y.png
└─ Google_墨卡托\
   ├─ z\x\y.png
   ├─ merged.png 或 merged.jpg
   └─ FBX 输出文件
```

百度等地图源的原始瓦片可能使用自己的内部目录规则，但后处理结果始终采用标准
Web Mercator XYZ。

自定义路径：

```powershell
.\DomDownTool.exe `
    --provider gaode `
    --cache "D:\map-cache\gaode" `
    --output "D:\map-output\gaode"
```

`--cache` 只修改原始瓦片缓存，`--output` 只修改后处理输出。原始瓦片继续跨运行复用；
后处理结果根据本次配置重新生成。

## 网络和并发

默认并发数来自硬件线程数，也可以手动指定：

```powershell
.\DomDownTool.exe --provider bing --concurrency 12
```

`--concurrency 0` 表示自动。Google 下载器保留项目现有的
`http://127.0.0.1:10090` 默认代理；可以覆盖或关闭：

```powershell
.\DomDownTool.exe --provider google --proxy "http://127.0.0.1:7890"
.\DomDownTool.exe --provider google --no-proxy
```

## 参数表

| 参数 | 说明 |
| --- | --- |
| `--provider <地图源>` | `gaode/baidu/tencent/tianditu/google/bing/arcgis` |
| `--map-type <类型>` | 高德的 `satellite/electronic/road-network` |
| `--lon <经度>` | WGS84 中心经度，默认 116.41 |
| `--lat <纬度>` | WGS84 中心纬度，默认 39.92 |
| `--radius <米>` | 下载半径，必须大于 0，默认 2000 |
| `--zoom <层级>` | 下载和后处理层级，默认 16，最大 30 |
| `--output-mode <模式>` | `raw/merged/pyramid/both/fbx` |
| `--merged-only` | 等同于 `--output-mode merged` |
| `--raw-only` | 等同于 `--output-mode raw` |
| `--cache <目录>` | 原始瓦片缓存目录 |
| `--output <目录>` | 金字塔、大图或 FBX 输出目录 |
| `--concurrency <数量>` | 下载和后处理并发数，0 表示自动 |
| `--proxy <地址>` | 覆盖地图源默认代理 |
| `--no-proxy` | 强制直连 |
| `--merged-resolution <像素>` | 合并大图最大边，默认 16384 |
| `--merged-format <png\|jpg>` | 合并大图格式，默认 PNG |
| `--fbx-texture-size <像素>` | FBX 贴图边长，默认 4096 |
| `--fbx-shape <形态>` | `plane/sphere/both` |
| `--fbx-subdivision <数量>` | 球面网格细分数，默认 64 |
| `--dry-run` | 打印最终配置但不下载 |
| `-h`, `--help` | 显示内置帮助 |

多时相天地图需要额外的年份、行政区、时间线 Key 和登录 Cookie，不属于这组常规卫星
下载参数。
