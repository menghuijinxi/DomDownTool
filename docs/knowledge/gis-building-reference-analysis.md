# GIS 建筑生成参考程序分析

本文档只整理参考程序当前逻辑和迁移风险，不把未确认的优化方案当作结论。

## 参考范围

已阅读的核心文件：

- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Public/MeshGenerator/GisBuildArrayTextureGenerator.h`
- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Private/MeshGenerator/GisBuildArrayTextureGenerator.cpp`
- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Public/MeshGenerator/GisBuildMultiTextureGenerator.h`
- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Private/MeshGenerator/GisBuildMultiTextureGenerator.cpp`
- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Public/Renderer/BuildTextureDataAsset.h`
- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Private/Renderer/BuildTextureDataAsset.cpp`
- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Private/Renderer/GisStaticMeshRenderer.cpp`
- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Private/GeoServerBPLibrary.cpp`
- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Public/MeshGenerator/GisMeshData.h`
- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Public/MeshGenerator/GisMeshGenerator.h`
- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Private/MeshGenerator/GisMeshGenerator.cpp`
- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Public/MeshGenerator/GisAsyncMeshGenerator.h`
- `D:/Project/LunixTest/Plugins/GeoServer/Source/GeoServer/Private/MeshGenerator/GisAsyncMeshGenerator.cpp`

## 本项目当前贴图资源

本项目 `D:/CMakeProject/DomDownTool/贴图` 下当前资源：

- 墙体贴图 505 张，当前均已补为 `.jpg` 扩展名；此前重复的 4 张 `_duplicate1.jpg` 已删除。
  - 4 张带 `.jpg` 后缀。
  - 尺寸高度离散，常见组合包括 `512x64`、`256x64`、`512x128`、`512x1024`、`512x2048`、`1024x2048`、`512x4096`、`1024x4096` 等，也有非 2 次幂尺寸。
- 房顶贴图 40 张，均为 `.jpg`，尺寸也不统一。

结论：当前资源天然是“散图素材库”。不适合走 `Texture2DArray`，因为 array 要求同一组 slice 尺寸、格式、mip 兼容；为了 array 统一尺寸会强行重采样，既浪费显存，也破坏原素材比例。后续讨论应排除 `Texture2DArray`。

## 总体调用链

参考程序不是“生成一栋楼就创建一个组件”。它的主要链路是：

1. `UGisAsyncMeshGenerator::StartMeshGenerator` 把生成任务放进线程池。
2. 建筑生成器遍历所有 `UGisParser` 的 `FGISObjectData`。
3. 通过 polygon 类型筛选器只处理 `POLYGON / POLYGONZ / POLYGONM`。
4. 每个 GIS polygon 转换为 UE 坐标。
5. 每栋建筑生成一份墙体 mesh data 和一份屋顶 mesh data。
6. 通过 `CallMeshGeneratorEvent` 把 mesh data 发给 renderer。
7. `UGisStaticMeshRenderer` 按 `MeshIndex + QuadtreeIndex` 聚合为一个 `MeshRenderData`。
8. 在每个聚合 mesh 内，再按 `MaterialName` 建 polygon group / material slot。
9. `CallMeshGeneratorEndEvent(0/1)` 后，renderer 把每个 quadtree 分块构建成 `UStaticMesh` / `UStaticMeshComponent`。

所以 draw call 的增长关键不是建筑数量，而是：

- 可见 quadtree chunk 数量；
- 每个 chunk 内的 material slot / polygon group 数量；
- 墙体和屋顶分别是不同 `MeshIndex`，通常至少分成两个 mesh 集合。

## 两套建筑生成器

参考程序里实际有两套相关生成器：

### `UGisBuildArrayTextureGenerator`

这个是用户最初给出的入口。名字里有 `ArrayTexture`，注释也写了“纹理数组”，但代码实现更准确地说是“合并贴图 / atlas + 额外 UV 通道”。

特征：

- 墙体使用一个 `WallMaterial`。
- `WallMaterial` 的 `MainTex` 参数指向一张合并大图。
- 合并大图的 `UTexture2D` 上挂 `UBuildMergeTextureDataAsset`。
- `UBuildMergeTextureDataAsset::SourceTexture` 保存每张源贴图的：
  - `Name`
  - `Tag`
  - `UVRange`
  - `PixelSize`
- 墙体 mesh 使用：
  - `UV0`：墙面自身的横向重复/纵向楼层 UV。
  - `UV1`：所选 atlas 子图的 `UVRange.Min`。
  - `UV2`：所选 atlas 子图的 `UVRange.Max`。
- 这要求材质根据 `UV0 + UV1 + UV2` 做采样重映射。模型本身并不是通过 material slot 区分每张墙体贴图。

屋顶有两条路径：

1. 如果存在 `RoofMaterial + RoofMergeTextureData`，则走屋顶 atlas。
   - 先算建筑 footprint 的包围盒长宽比。
   - 找房顶贴图中长宽比最接近的一组。
   - 再在接近最佳比例的候选里随机选一张。
   - 屋顶 UV 直接映射到 atlas 子图范围。
2. 如果没有屋顶 atlas，则扫描 `RoofMaterialPath` 下的材质。
   - 每个屋顶材质通过 `MinX / MinY / MaxX / MaxY` 标量参数声明自己覆盖的世界坐标包围盒。
   - 建筑屋顶点落入哪个包围盒，就使用哪个材质。
   - 如果一个屋顶跨多个包围盒，会尝试按包围盒边界切分 polygon，避免一整块屋顶 UV 被拉伸。

### `UGisBuildMultiTextureGenerator`

这个文件更贴近我们现在的“散图贴图”需求。它的注释明确写着：类似 `UGisBuildArrayTextureGenerator`，但使用多个独立墙体/屋顶贴图，而不是一个 atlas。

特征：

- 支持直接传入 `WallMaterials / RoofMaterials`。
- 也支持传入 `WallTextures / RoofTextures`，再通过 `WallParentMaterial / RoofParentMaterial` 自动生成材质实例。
- 每张散图最终对应一个材质实例和一个 `MaterialName`。
- 建筑 mesh data 通过 `MaterialInterface + MaterialName` 告诉 renderer 使用哪张贴图。
- renderer 把同一 quadtree chunk 内相同 `MaterialName` 的面归到同一个 polygon group。

这回答了“模型怎么知道用的是哪个贴图”的问题：

- 散图模式：靠 face / polygon group 的 material slot。导出成普通 StaticMesh/FBX 时也能表达，不依赖自定义组件。
- Atlas 模式：靠 atlas UV。参考程序当前用 `UV1/UV2` 辅助材质重映射；如果追求通用 FBX，可在导出时把 atlas 结果烘到最终 `UV0`，避免依赖自定义材质逻辑。

## 墙体生成细节

两套生成器的墙体逻辑基本一致：

1. 从 GIS 数据读高度字段，默认字段名是 `height`。
2. 如果 `TestLayer > 0`，用测试层数覆盖 GIS 高度。
3. `BuildHeight = LayerHeight * HeightValue`，默认 `LayerHeight = 45cm`。
4. 过滤无效建筑：
   - 高度 <= 0；
   - 点数 < 3；
   - 已存在 `FGisRoofMetadata`。
5. 对每个 polygon part：
   - 原始点作为底部顶点；
   - 复制一份并加 `BuildHeight` 作为顶部顶点；
   - 每条边生成两个三角形；
   - 最后一条闭合边单独处理；
   - 最后反转三角形顺序。
6. 计算墙体周长 `WallLength`。
7. 根据建筑高度和墙体长度选择墙体贴图。
8. 计算墙体 UV：
   - `UV0.X` 沿墙体周长累计；
   - `UV0.Y` 底部为 1，顶部为 0；
   - 最后一面重新修正 UV，避免闭合边从 1 跳回 0 造成贴图断裂；
   - 根据贴图宽高比和建筑高度缩放横向重复次数。

墙体贴图选择算法：

- `CurrentUnit = HeightValue / UnitLayer`，默认 `UnitLayer = 5` 层。
- 每张贴图的高度单位是 `TexturePixelHeight / UnitPixelSize`，默认 `UnitPixelSize = 64`。
- 候选贴图不是全部随机，而是找：
  - 高度单位大于当前建筑的最小贴图组；
  - 高度单位小于等于当前建筑的最大贴图组。
- `cachedTextureIndex` 缓存每个高度单位对应的候选贴图索引。
- `currentMaterialCount[QuadtreeIndex][CurrentUnit]` 限制同一个 quadtree、同一个高度单位最多使用多少种贴图，默认 `MaxMaterialCount = 1`。
- 如果还允许引入新贴图，则按墙体长度与贴图物理宽度的匹配度加权随机。
- 如果同 quadtree + 同高度单位已经达到 `MaxMaterialCount`，则只从已用贴图里随机。

这套逻辑的目的不是单纯随机，而是同时控制：

- 建筑高度和墙体贴图高度匹配；
- 墙体长度和贴图横向比例匹配；
- 同一区块不要出现过多不同墙体材质，避免视觉太杂，也减少散图模式材质槽数量。

## 屋顶生成细节

参考程序里屋顶不是单一逻辑，应该分成三类正式模式理解：

1. 卫星图屋顶模式。
2. 合并贴图屋顶模式。
3. 散贴图屋顶模式。

这三种模式解决的问题不同：卫星图屋顶模式把原始卫星图中对应位置的屋顶色彩贴回建筑顶部，颜色最接近真实影像；合并贴图屋顶模式把多张 roof texture 合成 atlas，用较少材质槽渲染；散贴图屋顶模式直接使用独立 roof texture，资产和导出表达最直观。

### 卫星图屋顶模式

这条路径在 `UGisBuildArrayTextureGenerator` 中表现为扫描 `RoofMaterialPath` 下的材质，并读取材质参数 `MinX / MinY / MaxX / MaxY`。

它的意图不是随机选一张 roof texture，而是复用已有卫星图切片：

- 每个 roof material 对应一张卫星图或卫星图材质。
- `MinX / MinY / MaxX / MaxY` 描述这张卫星图材质在世界 XY 坐标中覆盖的范围。
- 建筑顶面根据自己的世界 XY 位置，选择覆盖该位置的卫星图材质。
- 顶面 UV 用 `(WorldXY - Bounds.Min) / Bounds.Size` 计算，让建筑屋顶采样卫星图中原本对应的位置。
- 这样建筑顶部可以显示真实卫星图上的屋顶颜色、道路边缘、阴影和地物变化，色彩丰富度通常高于随机 roof texture。

跨卫星图切片处理：

- 如果一个建筑顶面只落入一个卫星图 bounds，就直接生成一份 roof mesh。
- 如果一个建筑顶面跨多个卫星图 bounds，程序会尝试按 bounds 边界把 polygon 切成多份。
- 每一份 roof mesh 使用对应的卫星图材质和对应 bounds 的 UV。
- 这一步的目的不是提高随机性，而是避免跨卫星图切片时整块屋顶 UV 被错误拉伸。

### 合并贴图屋顶模式

这条路径对应 `UGisBuildArrayTextureGenerator` 中 `RoofMaterial + RoofMergeTextureData` 成立时的 atlas 屋顶逻辑。

逻辑：

- 计算建筑 footprint 的 XY 包围盒。
- 根据包围盒长宽比匹配房顶贴图长宽比。
- 先找最接近的长宽比差异。
- 再把差异在 `bestDiff * 1.3` 以内的贴图作为候选。
- 从候选里随机选一张。
- 顶面点加 `BuildHeight`。
- `UV0` 先映射到建筑 footprint 包围盒的 `[0, 1]` 范围。
- 再根据选中子图的 `UVRange` 把 `[0, 1]` 映射到合并贴图的局部区域。
- `Earcut` 三角化顶面，反转三角面，发出 `MeshIndex = 0` 的 roof mesh。
- 所有屋顶共用一个 `RoofMaterial`，通过 atlas UV 表达具体使用哪张房顶子图。

这个模式比散贴图更省材质槽和 draw call，但需要合并贴图和 JSON 元数据，并且要处理 atlas padding / mipmap 串色问题。

### 散贴图屋顶模式

这条路径对应 `UGisBuildMultiTextureGenerator` 的屋顶逻辑。

逻辑：

- 每张屋顶散图对应一个材质实例或已有材质。
- 计算建筑 footprint 的 XY 包围盒。
- 根据包围盒长宽比匹配房顶贴图长宽比。
- 先找最接近的长宽比差异。
- 再把差异在 `bestDiff * 1.3` 以内的贴图作为候选。
- 从候选里随机选一张。
- 顶面点加 `BuildHeight`。
- 屋顶 `UV0` 映射到 `[0, 1]`。
- mesh data 的 `MaterialInterface / MaterialName` 指向选中的屋顶材质。
- 模型通过 material slot 知道顶面用哪张屋顶散图。

这个模式通用性最好，FBX/material slot 可以直接表达，但每个 chunk 内 roof material slot 数量会随实际使用的屋顶贴图数量增长。

## 合并贴图资产与脚本

`UBuildMergeTextureDataAsset` 是 atlas 模式的元数据资产：

- 可以挂在合并后的 `UTexture2D` 上。
- 也可以挂在材质上；代码会从材质的 `MainTex` 参数反查合并贴图。
- 通过 `JsonPath` 导入 JSON，填充每个子贴图的 `Name / PixelSize / UVRange`。
- 编辑器里还能从合并大图按 `UVRange` 反向裁出散图 JPG。

`UGeoServerBPLibrary::MergeFolderTextureToOneTexture` 是插件里提供的合并脚本：

- 只递归扫描 `*.png` 和 `*.jpg`。
- 用 OpenCV decode 图片。
- 从 `2x2` 开始扩 atlas 尺寸，X/Y 轮流翻倍。
- 大图优先放，小图后放，试图填空隙。
- 每张图尝试放在已有图右侧或上方。
- 输出：
  - `Merged/MergedTexture.png`
  - 同名 `.json`，包含 `Name / UVRange / PixelSize`

迁移风险：

- 墙体素材已补齐 `.jpg` 扩展名；如后续再加入无扩展名 JPEG，仍需要按文件头识别或先补扩展名。
- 代码用 `cv::IMREAD_UNCHANGED`，但写合并图时按 `Vec4b` 读源像素。JPEG 通常是 3 通道，这里对 JPG 可能存在通道假设风险。
- atlas 子图之间基本只有几何间隔，没有 mipmap gutter / 边缘扩展；远距离或斜视角可能串色。正式工具应加 padding + edge dilation。

## Renderer 合批机制

`UGisStaticMeshRenderer` 是 draw call 结论的关键：

- `RenderData` 第一层 key 是 `MeshIndex`。
  - `0` 通常是 roof。
  - `1` 通常是 wall。
- 第二层 key 是 `QuadtreeIndex`。
- 同一个 `MeshIndex + QuadtreeIndex` 会合成一个 `MeshDescription` / `UStaticMesh`。
- 每遇到一个新的 `MaterialName`，就创建一个 polygon group 和 static material slot。
- `FGisMeshDataAddUV` 会让 mesh builder 开 3 层 UV。
- 普通 `FGisMeshData` 只开 1 层 UV。

因此：

- Atlas 墙体：每个 chunk 的墙体通常只有一个 `Wall` material slot。
- 散图墙体：每个 chunk 的墙体 material slot 数量等于该 chunk 实际用到的墙体贴图数量。
- 由于 `MaxMaterialCount` 限制的是“同 quadtree + 同高度单位”的贴图种类，散图模式的材质槽数量通常远小于 505，但不一定接近 1。
- 散图和 atlas 的 draw call 差距取决于：chunk 大小、可见 chunk 数、建筑高度分布、`UnitLayer / UnitPixelSize / MaxMaterialCount` 和屋顶贴图策略。不能只凭 505 张图推断差距很大，也不能假定差距很小，需要用目标数据统计。

## 参考程序中不能丢的功能

迁移时需要保留或明确替代这些功能：

- polygon 类型过滤：只处理建筑 polygon，不误处理点线数据。
- 高度字段配置：默认 `height`，可测试覆盖 `TestLayer`。
- `LayerHeight`：把“楼层数/高度字段”映射到 UE 厘米高度。
- 随机种子：支持固定 seed，保证生成结果可复现。
- quadtree 元数据：用于按空间块限制材质种类，也用于 renderer 合批。
- 墙体高度匹配：按贴图高度像素和建筑层数找候选贴图。
- 墙体宽度匹配：按墙体周长和贴图宽高比例做加权随机。
- `MaxMaterialCount`：控制同区块同高度的贴图复杂度。
- 墙体闭合边 UV 修正：避免最后一面贴图断裂。
- Atlas 模式的 `UVRange` 元数据。
- 散图模式的自动材质实例创建。
- 卫星图屋顶模式：用世界 XY bounds 把卫星图对应区域贴到建筑顶面。
- 合并贴图屋顶模式：按建筑 footprint 长宽比匹配 atlas 子图，并用 `UVRange` 映射。
- 散贴图屋顶模式：按建筑 footprint 长宽比匹配独立 roof texture，并通过 material slot 表达。
- 屋顶跨卫星图切片时的切分逻辑，避免顶面 UV 被拉伸。
- renderer 的 `MeshIndex + QuadtreeIndex + MaterialName` 合批逻辑。
- 生成过程的取消检查 `bStopAsync`。

## 已发现的问题和风险

### `UGisBuildArrayTextureGenerator` 周长计算疑似错误

array/atlas 版本计算墙体周长时，循环条件是 `index < pointsArray.size() / 2`，但循环体读取 `pointsArray[index + 1]`。

当 `index == bottomPointCount - 1` 时，`index + 1` 会读到顶部第一个点，而不是底部下一个点。后面又额外加了一次“最后底点到第一个底点”的距离。

这会让 `wallLength` 偏大，并影响：

- 墙体贴图选择权重；
- 横向 UV 缩放；
- 贴图重复次数。

`UGisBuildMultiTextureGenerator` 里同一段逻辑已经改成 `index < PointCount - 1`，然后单独闭合最后边，逻辑更合理。

### `GetPointCount(iPart, true)` 与闭合点

两套生成器都使用 `GetPointCount(iPart, true)`。如果这个 `true` 会包含闭合重复点，那么需要确认：

- 墙体是否生成了零长度面；
- 屋顶 Earcut 是否接受重复闭合点；
- UV seam 修正是否重复处理首尾点。

参考程序可能依赖 GIS 数据/解析器的具体行为，迁移时不能盲目复制。

### Atlas 模式的可移植性

参考程序 atlas 墙体不是把最终 atlas UV 烘进 `UV0`，而是用 `UV0 + UV1 + UV2` 让材质重映射。

这在 UE 插件内部可用，但如果导出通用 FBX：

- 普通 DCC/引擎未必知道如何使用 UV1/UV2 重映射。
- 如果希望不依赖插件和自定义材质，应在导出时直接把最终 atlas UV 烘进 `UV0`，或导出配套材质说明。

### 散图模式不是“每栋楼一个 draw call”

散图模式使用 material slot 表达贴图选择。它会增加材质槽，但 renderer 已经按 quadtree 和 MaterialName 合批。

所以散图模式的性能不能按“建筑数 × 贴图数”估算，应该按“可见 chunk 内实际材质槽数量”估算。

### 卫星图屋顶切分逻辑偏脆弱

`UGisBuildArrayTextureGenerator` 的多卫星图 bounds 屋顶切分是手写线段求交，不是完整 polygon clipping。

复杂 footprint、洞、多 part、多次进出 bbox 时可能出现：

- 点序异常；
- 交点选择不稳定；
- Earcut 失败或生成畸形三角形；
- UV seam 不连续。

如果我们需要复用卫星图屋顶投影，应考虑换成明确的 polygon clipping 算法，而不是直接照搬。

## 对我们项目的准确认知

当前不应把 `Texture2DArray` 作为方案。真正需要比较的是：

1. 散图材质槽模式。
   - 模型通过 material slot 知道每个面用哪张贴图。
   - 通用性最好，FBX/UE 都容易表达。
   - draw call 取决于 chunk 内实际材质槽数量。
2. Atlas 模式。
   - 理论 draw call 更低。
   - 需要合并贴图和 JSON 元数据。
   - 如果不烘最终 UV，就依赖自定义材质重映射。
   - 如果烘最终 UV，可以导出更通用，但 atlas 需要 padding/mipmap 处理。
3. 分块粒度和贴图选择策略。
   - 这是决定散图模式性能的关键。
   - `MaxMaterialCount` 不只是视觉参数，也影响 material slot 数量。
4. 屋顶模式需要独立选择。
   - 卫星图屋顶：色彩和真实感更丰富，但需要保留卫星图 tile bounds，并处理跨 tile 切分。
   - 合并贴图屋顶：draw call 更低，但需要 atlas cache、元数据和 padding。
   - 散贴图屋顶：通用、稳定，顶面素材来自 roof texture 库，但材质槽更多。

## 本项目首版散图 FBX 实现边界

首版建筑导出先只实现散贴图模式，输出到 GIS FBX 目录下的独立 `buildings.fbx`，不合并进已有地形 `plane.fbx`。这样可以保持地形后处理逻辑稳定，也方便单独预览或导入建筑资产。

当前实现原则：

- 只在 GIS 建筑类别参与导出、且启用 FBX 后处理时生成 `buildings.fbx`。
- 只导出 polygon、带有效高度且高度大于 0 的建筑；不对缺失高度的数据硬编码默认楼高。
- 高度语义沿用参考程序：`height * LayerHeight`，默认 `LayerHeight = 45cm`。
- 墙体素材当前均为 `.jpg`；导出器仍兼容无后缀 JPEG，会按文件头识别 `FFD8FF` 并复制为 `building_wall_*.jpg`。
- 屋顶首版使用散贴图模式：按建筑 footprint 长宽比匹配 `贴图/房顶` 下的 roof texture。
- 模型通过 FBX material slot / diffuse texture 表达“哪一面用哪张贴图”，不依赖自定义 UE 组件或 `Texture2DArray`。
- 墙体贴图选择保留参考程序的核心约束：按建筑层数匹配贴图高度单位，并用每 1km 空间桶 + 高度单位限制局部墙体材质种类。
- 顶面使用 earcut 三角化，墙体按边挤出成 quad；贴图复制到 FBX 同目录，便于 DCC/UE 直接解析相对路径。

暂未实现的内容：

- atlas / 合并贴图模式。
- 卫星图屋顶模式和跨卫星图 tile 的 roof polygon 切分。
- 与地形 `plane.fbx` 合并为单个 FBX。
- 导出 UI 中暴露建筑贴图路径、LayerHeight、UnitLayer、MaxMaterialCount 等参数。

## 建议下一步验证

在写正式功能前，建议先做一个统计型原型，不急着改 mesh 生成：

1. 读取我们的 GIS 建筑 footprint 和高度字段。
2. 按目标 tile/quadtree 分块。
3. 用参考程序的墙体贴图选择算法跑一遍，不生成模型，只统计：
   - 每个 chunk 建筑数量；
   - 每个 chunk 使用的墙体贴图数量；
   - 卫星图屋顶模式下，每个 chunk 涉及的卫星图材质/tile 数量，以及跨 tile 建筑数量；
   - 合并贴图屋顶模式下，每个 chunk 的 roof material slot 数量和 atlas 估算尺寸；
   - 散贴图屋顶模式下，每个 chunk 使用的屋顶贴图数量；
   - `MaxMaterialCount = 1/2/4` 时 material slot 变化；
   - 预估 wall/roof section 数。
4. 再决定默认导出模式：
   - 如果散图模式的 material slot 数量可控，优先散图，因为模型更通用。
   - 如果某些区域材质槽爆炸，再加 atlas cache 模式。
5. 如果做 atlas，必须补：
   - 无扩展名 JPEG 扫描；
   - 3/4 通道统一处理；
   - padding + edge dilation；
   - 可选择“烘最终 UV0”，避免导出后依赖插件材质。
