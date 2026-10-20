# Cesium 地形与 3D Tiles 导出调研

调研日期：2026-10-10。
状态：用户已确认标准 terrain 路线，已实现独立地形和建筑组件、金字塔和验证页。
本文保留规范调研，并在“当前实现”中记录实际范围及限制。
经用户确认接入 KTX-Software 静态库，不新增外部运行时或 DLL。

## 已确认的产品边界

- 新增两个独立组件，界面名称为 `Cesium 地形` 和 `3D Tiles 建筑`，
  区分 terrain 与普通 3D Tiles 协议。
- 不要求用户创建、选择或引用已有 FBX 底板/建筑组件。
  网格生成和数据下载只在底层复用，不把代码依赖变成操作依赖。
- `Cesium 地形` 输出地形网格，不烘焙或绑定 DOM 图片。
- 现有 XYZ DOM 的输出格式、投影、目录和生成逻辑不变。
- `3d tile建筑` 最精细层输出建筑网格和墙体/屋顶散图。
  较粗层简化模型，并把多种建筑外观烘焙到瓦块级低分辨率合并贴图，
  降低材质、纹理、绘制批次及几何开销。此前“所有层固定散图”的建议取消。
- 两个组件都要生成真实的多级模型金字塔，不接受只有叶模型的空间索引树
  冒充多级简化。先前“首版不额外生成多级 LOD”的建议已由本要求取代。
- 测试网页分别加载 XYZ DOM、底板和建筑，验证定位、影像铺设及金字塔切换。
- 底板的核心目标是兼容 Cesium、保持多级地形，并独立接入本地 XYZ 和在线底图，
  不要求影像预先绑定到导出的地形文件。

## 地形协议分支与开源工具

### CesiumLab 参考指向标准 terrain

CesiumLab 官方使用手册的搜索索引明确将“地形切片格式以及标准”指向
CesiumGS 的 quantized-mesh 规范。手册 PDF 本轮未获取到正文，因此这里只引用
能够核实的索引段落，不推断它的内部网格算法或全部导出模式。
用户实际使用的是哪个模式尚无输出目录或文件作为证据。

quantized-mesh-1.0 是多分辨率四叉树地形金字塔，常见文件为：

- `layer.json`：地形层描述、级别、投影、瓦块可用范围等。
- `{z}/{x}/{y}.terrain`：各层的量化三角网。

它通过 `CesiumTerrainProvider.fromUrl()` 加载，而不是
`Cesium3DTileset.fromUrl()`。建筑仍适合 `tileset.json` 加 GLB 的 3D Tiles。
两者都能在 Cesium 中使用，但文件协议和加载接口不同。

根据用户“独立在线底图接入、参考 CesiumLab 地形”的目标，推荐底板优先采用
标准 terrain，而不是把它限定为普通 GLB 3D Tiles。
用户已经确认这一方向，当前实现采用此协议并以 `Cesium 地形` 命名。

### 参考项目与适用范围

| 工具 | 已核实能力 | 适合参考的部分 | 不应直接沿用的部分 |
| --- | --- | --- | --- |
| `ahuarte47/cesium-terrain-builder` 的 `master-quantized-mesh` 分支 | C++/GDAL，`Mesh` 输出、层级生成、`layer.json`、根瓦块补齐、顶点法线 | 地形金字塔组织、重采样、可用性和接缝相关实现 | README 留有旧版描述；NoData、垂直基准、现代 GDAL/MSVC 兼容性需核对源码并实测 |
| HERE `tin-terrain` | `dem2tintiles` 将 GeoTIFF 生成不同 zoom 的 TIN/quantized-mesh 金字塔，可设置高度误差 | 地形误差控制与多层生成对照 | 官方已归档且声明实验性，不能作为维护活跃的现成依赖 |
| OpenDroneMap `Obj2Tiles` | 网格 LOD、空间分块、材质合并、图集重排、粗层贴图降分辨率、3D Tiles 输出 | 建筑 HLOD 的几何/纹理联合降载流程 | 其空间切割会切模型，不符合本项目整栋归块；图集重排不是任意材质着色器或高低模投射烘焙 |

许可证初查：CTB 分支 README 声明 Apache-2.0；Obj2Tiles 仓库标为 AGPL-3.0。
引用工具的技术思路不等于复制其代码或直接集成依赖。依赖、许可证和构建方案
必须另外确认，本轮未下载或安装这些工具，也未以其运行结果宣称性能收益。

### 标准 terrain 路线的实现边界

- 不给 terrain 文件绑定 DOM 图片；影像由 Cesium 的 globe 影像层独立接入。
- 金字塔网格之外，需正确写入包围信息、量化顶点/索引、边界索引及可用范围，
  并验证服务端 gzip、Content-Encoding、CORS 和静态文件加载方式。
- 协议支持地理和 Web Mercator 分块；默认地理分块在第 0 级为 2 x 1，
  Web Mercator 第 0 级为 1 x 1。不能直接把现有 XYZ 图片目录改后缀当作地形。
- terrain 默认 TMS 的 Y 方向与常见 XYZ 相反。
  Cesium 1.146 源码同时识别 `slippyMap`；为工具兼容建议先按标准 TMS 验证。
- `layer.json` 的 `available` 或瓦块内 metadata 可表达可用性；
  不能照搬旧 issue，把 metadata 扩展说成唯一可用方式。
- 不只生成最细层或把 `minzoom` 写成较大值就期待客户端自动找到局部地形；
  标准提示许多客户端从第 0 层遍历，应测试根层和祖先层回退。
- 局部范围/KML 的细层按选中瓦块生成；祖先瓦块覆盖范围会更大，
  需明确 DEM 外部、NoData 和缺失子块的补齐/回退策略。
  客户端也可能上采样父网格或生成 fill tiles，不能把 terrain 瓦块可用性
  等同于严格多边形裁切。
- 地形的采样误差控制和 terrain 客户端选层指标分别记录；
  不直接照搬 3D Tiles 每节点的 `geometricError` JSON 字段。

## 规范规则

本节描述适用于建筑的 3D Tiles；地形以 terrain 协议和 TerrainProvider 规则为准。

### HLOD、切换与误差

3D Tiles 的树描述空间和 Hierarchical Level of Detail。树可以是四叉树、
八叉树、k-d tree 或其他满足空间一致性的结构；不是强制 Google XYZ 网格。
使用四叉树时可省略空子瓦块。

父瓦块包含大范围的粗略模型，子瓦块提供该范围的更精细模型。
为本项目建议使用 `REPLACE`：细层替换粗层，不同时保留两套完整表面。
`ADD` 的语义是同时显示父子内容，适合增量内容，不适合把同一地形/建筑的
完整粗细模型直接叠加。

`geometricError` 为非负的米制选择指标，Cesium 将它转换成屏幕空间误差 SSE，
结合相机距离和视口决定是否下钻。它不是三角面数、DEM 采样间距或目录层号。
规范允许根据网格/贴图简化等数据特征制定指标，不能任意写数值并声称是实测误差。

建议同时保留：

- 用于选层的 `geometricError` 和所用策略。
- 相对共同参考数据测得的几何误差，记录采样方式及是否为严格上界。
- 实际三角面、索引顶点、拆分后的渲染顶点、primitive 数和资源字节数。

父层误差需保守反映粗模及后代范围，不可只按“面数减半”猜测。
最细层的参考数据误差和调度终止语义也必须区分，不能把 DEM 近似残差隐藏成零。

### 包围体、坐标与内容

建筑建议采用 3D Tiles 1.1、glTF 2.0 内容和 `.glb` 网格文件。
地形不采用这一协议，不由组件旧名称决定。
`tileset.json` 描述树、包围体、误差、内容路径和变换。

每个 tile 的包围体需包含自身内容，父包围体需包含子内容。
规范不要求子包围体本身严格落在父包围体里；本项目可以采用更保守的
包围体并集策略，避免裁掉跨格建筑。

glTF 坐标为右手、Y-up，3D Tiles 的局部约定为 Z-up，需处理轴向转换。
WGS84 地理定位应通过局部坐标与 ECEF 变换明确表达，不能把 EPSG:3857
平面米数直接当作地球 ECEF 坐标。
建议每块使用局部原点保存 float 顶点，以 double 变换定位，降低大坐标精度损失。

高度也不能复用旧版 FBX 的“范围最低点归零”后直接放到地球：
Copernicus DEM 的垂直基准为 EGM2008，WGS84 地理包围体使用椭球高。
实现前需明确转换方式和 geoid 数据来源；缺少转换数据时不得静默把正高
宣称为椭球高。现有 GeographicLib 可作为转换能力参考，但运行所需 geoid
数据部署不随程序内置；导出参数接受 Geoid PGM 路径及手动高度偏移。

### 显式树与隐式树

显式 `children` 和隐式 `implicitTiling` 都能表达金字塔。
隐式格式额外规定子树 availability、固定空间细分和误差细分规则。
它不会自动创建粗层模型，也不会自动简化输入网格。

实现建议先使用显式稀疏树，原因是：

- KML 范围中存在空格和缺失区域，需要显式管理有效内容。
- 建筑整栋跨格，实际包围体可能超过名义方格。
- 地形误差、楼群密度不均匀，需要按内容设置各节点指标。
- 相比一开始维护隐式 subtree 位图，显式树便于检查错误和验证父子范围。

这是实现建议，不是说显式树优于所有场景。极大范围下的 JSON 索引体积、
外部 tileset 拆分和隐式格式是否必要，需要测量后决定。

## 两类金字塔的建议

### 地形

1. 按最终协议建立多层规则分块，并正确描述选中范围和祖先层。
2. 所有层引用同一套高程基准和地理网格，不逐层累积中心点偏移。
3. 每层从共同 DEM 参考生成对应误差目标的 TIN。
   可以复用现有 hmm 渐进细化模块，不把子层三角形简单拼接成越来越大的父模型。
4. 保留真实多级粗细差异；简单地块可提前达到误差目标，不强行凑面。
5. 若底板使用 GLB 3D Tiles，KML 父层有效覆盖应与子层选中范围一致，
   不能补成整块外接矩形；现有矩形 TIN 接口需适配稀疏遮罩。
   若使用标准 terrain，按上节验证规则瓦块、较大祖先层及缺失子块回退，
   不以普通模型的遮罩规则代替 terrain 可用性语义。

现有 `BuildFbxTerrainTin` 已返回最大样点误差，且现有 FBX 流程已处理同层
共享边界，但这不等于具有跨层无缝保证。

多级选择时可能同时显示粗邻块和细邻块，独立锁定各层边界仍可能产生裂缝。
需要研究嵌套边界/跨层约束、接缝三角形或裙边等路线，并实测其顶点开销与变形。
Cesium quantized-mesh 规范明确提到裙边用于遮挡不同 LOD 的裂缝；
当前输出完整边界索引，由 Cesium TerrainProvider 在客户端生成裙边；
裙边不能用来掩盖块内破洞。

不要把 quantized-mesh 与 3D Tiles 1.1 GLB 内容混成同一个文件协议。
MARTINI/RTIN 可作为层次地形对照，当前 hmm 也能按不同容差生成粗细模型；
是否替换现有算法需要用真实地形比较，不因需要金字塔就默认推翻已有实现。

### 建筑

1. 使用名义方格和松散四叉树，沿用整栋按中心点归块、不切开楼体的规则。
   父包围体按实际楼体和后代范围计算，允许跨格内容。
2. 叶层保存详细轮廓挤出的墙体/屋顶和散图材质。
3. 父层表示同一楼群的较粗近似，保留主要轮廓、高度和地理位置；
   不能只把四个子块完整网格合并，却仍输出相同或更多的面。
4. 粗层按瓦块重建 UV，将多种建筑墙面、屋顶的最终基础色外观
   烘焙/重采样到一张低分辨率合并贴图，合并可兼容的材质和 primitive。
   不是全范围共享一张巨图，也不是缩小原有散图后仍保留全部材质槽。
   更粗层继续降低纹理细节；各级图集尺寸/纹素密度先用真实场景验证，
   不把父块覆盖更大但仍使用高分辨率图集算作纹理降载。
5. 在轮廓简化、孔洞保留和通用约束减面路线之间先做原型比较。
   CGAL 的法线变化及包络过滤可作为约束参考，不据此声称低面数永远无自交。
6. 各层使用稳定的建筑身份和外观来源，不能因分块/遍历顺序改变而随机换图。
   最细层散图按来源、寻址和输出版本去重；粗层只引用本级合并贴图，
   不再携带全套原始散图。避免每个 GLB 重复打包整份图库。
7. 除非另外确认语义，不把随机删掉小建筑作为“简化”。
   建筑已是轮廓挤出模型，极低预算保留每栋楼的能力有下限；需要诚实报告
   达不到预算的原因，不能错误连接不同建筑顶点。

3D Tiles 规范中的 loose quadtree 示例明确允许通过重叠分块避免切开模型，
与本项目整栋归块的需求匹配。

目前仓库没有专用建筑 HLOD 简化器。新增 CGAL、meshoptimizer 或其他依赖前，
需要说明许可证、构建影响和实测收益并取得确认；本调研未安装它们。

#### 烘焙质量和性能验证

现有墙面可能包含重复寻址、窗格/楼层分区等 UV 外观逻辑。
粗层合并时需把最终可见外观展开到新 UV，不只是把源图片排进图集。
几何简化改变表面/UV 后，还需比较原模型与简模的映射误差。
若仅进行保持 UV 的纹理裁剪和图集重排，应明确这不是高低模投射烘焙。

图集需测试 mipmap 下的边缘扩展、留白和串色，避免远景出现黑边或颜色污染。
基础色烘焙不应默认把固定太阳光或高光写入，防止运行时再次光照。
是否生成法线等辅助贴图需依据原始材质和实测决定，不能用新增通道抵消降载收益。

验收既比较轮廓/外观，也记录每级：

- 三角面、索引顶点和按 UV/法线拆分后的实际渲染顶点。
- 材质槽、primitive、实际绘制批次。
- 图集尺寸、贴图数量、下载字节和纹理显存估算。
- 远景选择粗层时，原始高分辨率散图是否仍被请求或保留引用。

“每块一张贴图”不保证一次 draw call；需核对 GLB 的 primitive 和材质组织，
并用 Cesium 的实际运行统计验证。

## XYZ DOM 与无贴图地形的兼容性

用户已有独立在线底图接入经验。实现重点是正确导出 Cesium 兼容地形，
不修改现有 XYZ DOM，也不把某一种在线服务绑定进地形生成器。

标准 terrain 路线通过 globe 的 `terrainProvider` 和 `imageryLayers` 分别
配置地形与影像。影像供应方可以是本地 XYZ 或 Cesium 已支持的在线 provider，
两套资源不必使用同一目录或相同层号。
测试时仍需核对 provider 的投影、Y 方向、有效级别和地理基准。

下面记录前次普通 GLB 3D Tiles 底板的替代路线，不再把其实验性铺设接口
作为标准 terrain 的兼容前提：

截至 2026-10-10，官方最新发布为 CesiumJS 1.146，发布日期 2026-10-01。
CesiumJS 1.130（2025-06-02）已增加 3D Tiles 影像铺设功能。
1.146 源码和当前 API 均提供 `Cesium3DTileset.imageryLayers`：
可以把独立 `UrlTemplateImageryProvider` 接入底板 tileset，而非修改或烘焙 XYZ。
仅添加到 globe 的影像集合不等于已给普通 3D Tiles 底板铺设影像。
建筑 tileset 使用自身材质：最细层散图、粗层合并贴图，不往建筑集合追加 DOM 层。

该接口仍标为 experimental。官方 #12643 仍开放，讨论了以下风险：

- 大范围 primitive 的影像会偏低分辨率；高分辨率覆盖需要更细的几何分组。
- 影像级别不足或请求失败、图层删除/可见性、跨日期变更线和极区等边界情况。
- 多影像层和纹理单元限制。

1.146 的铺设实现未证明这些风险已全部解决，应以固定版本源码和实际加载验证，
不能仅根据旧论坛帖子宣布不支持，也不能仅根据出现 `imageryLayers` 就承诺
任意粗地形都能显示最高分辨率 DOM。

实现时必须覆盖平坦地形：纯高度误差很小可能使粗层长期不下钻，
影像粒度与几何 SSE 的协调策略需用原型确认，并明确记录指标含义。
倾向让地形分组与现有 Web Mercator XYZ 级别对齐，但这不改变地理定位规则，
也不要求建筑金字塔与 DOM 的层号一一对应。

## 验收计划

- 官方 `3d-tiles-validator` 校验 tileset 和 GLB；其 GLB 检查使用 glTF Validator。
  校验结构通过不能替代几何质量审计。
- 数值验证包围体、坐标/高度/轴向、父子覆盖、层级误差、稀疏 KML 范围。
- 地形验证块内覆盖、反向/退化三角形、同层接缝、跨一层及跨多层接缝。
- 建筑验证整栋身份、跨格建筑、孔洞、轮廓/高度误差、材质和 UV。
- 每层统计三角面、索引和渲染顶点、primitive、GLB 和贴图大小；
  以及粗层材质合并、图集烘焙、远景绘制批次，记录粗层是否真正降低了负载。
- 测试网页固定 CesiumJS 1.146，分别开关 XYZ、底板和建筑，
  并切换至少两个独立影像来源，验证地形与影像解耦。
  底板选择标准 terrain 时，使用 `CesiumTerrainProvider` 加入 globe；
  若仍选择 GLB 3D Tiles，则将 XYZ 加入底板的 `imageryLayers`，
  不混用两种加载方式。
- terrain 路线用独立读取器核对量化顶点、索引、边界和压缩，并检查
  `layer.json` 的 availability、Y 方向、局部祖先层回退；不能用
  `3d-tiles-validator` 代替 terrain 格式检查。
- 网页显示/记录选中层级、请求 URL、加载错误、内存和线框；
  以远景、中景、近景及网络延迟测试验证替换、过渡和按需加载。
- 在桌面/移动视口检查截图和画布非空；平坦地形也需检查 DOM 是否够清晰。
- 测试输入、报告、截图放在项目 `tmp/`，正式下载器和业务导出不被覆盖。

以上是完整验收清单，不代表所有规模和边界案例均已覆盖。
已经执行离线格式/网格回归、glTF 校验和真实数据网页验证。

## 当前实现

### 地形输出

- 独立 `layer.json` 和 gzip `{z}/{x}/{y}.terrain`，EPSG:4326、TMS、根层 2 x 1。
- 采样间距决定最细级别，各层分别从 DEM 采样；细层复用已有 hmm TIN，
  增加球面曲率约束，防止平地被简化成穿入地球的巨大三角形。
- 大于半球尺度的粗层使用规则网格。细层的样点误差统计包含曲率修正，
  不是 DEM 数据源精度、量化误差或严格三维 Hausdorff 上界。
- 范围/KML 选择相交细瓦块，并补齐祖先和两个根瓦块。
  缺少 DEM 的祖先区域采用零高程，报告记录缺失样点数。
  这不是全球真实地形，也不是沿 KML 边缘严格裁切。
- 同级边界来自相同地理样点，量化可能造成小幅高度差；
  跨级接缝使用 TerrainProvider 的裙边机制。
- 写入有效球体和 horizon occlusion point，不能用零向量表示禁用剔除。

### 建筑输出

- 3D Tiles 1.1 显式松散四叉树、`REPLACE`、GLB 2.0。
- 在 ENU 地面米制方格中按整栋中心归块，默认叶块边长 2200 米，默认 4 层。
- 各层从同一批建筑重建。粗层使用 OpenCV `approxPolyDP` 简化单环轮廓，
  保持高度、身份和主要形状；多环楼体不简化，避免错误连接孔洞。
  不合并不同楼体、不随机删楼。简单四边形可能没有额外减面空间。
- 在原始轮廓上复用 FBX 的楼高候选、墙长权重、材质桶及屋顶比例规则，
  一次性确定墙体和屋顶散图。分块与各级简化保留源建筑索引，复用同一份选图；
  不以可能缺失的 UID 取余，不在各瓦块或各级重新随机选图。
  叶层纹理仍按摘要去重；未简化层的几何、UV 和材质构造与 FBX 共用。
- 粗层展开墙体重复寻址，重采样基础色到本瓦块图集，留白扩边并重写 UV，
  合并为单个网格/材质。离叶层越远，图集分辨率越低，最低 256。
  这是基础色重采样与图集烘焙，不是任意着色器或高低模投射烘焙。
- 叶层散图和粗层图集统一编码为 KTX2 / BasisU ETC1S，质量参数 128，
  与目标项目 OSGB 的 mipless ETC1S 路线一致。图集直接编码，不先中转 JPG。
  编码只对齐到 4 像素块，不强制 512 或其他额外最大边；
  原有各层图集尺寸策略保留。散图在整个 tileset 内按内容摘要共享。
- 建筑组件提供“生成 Mipmap”，默认关闭，随导出配置保存。
  关闭时只有基础层，min/mag filter 为 LINEAR；开启时编码完整 mip 链，
  min filter 为 LINEAR_MIPMAP_LINEAR。基础色采用 sRGB，
  mip 缩小在线性颜色、预乘透明度空间完成，不影响 XYZ、FBX 或 GeoTIFF。
- 实际 ENU 包围体包含内容和子体；root 通过 double ENU -> ECEF 变换定位。
  GLB 顶点相对导出中心，特别巨大范围的 float 精度和索引体积仍需专项测试。
- 选层误差使用 `max(轮廓容差, 名义瓦块边长 / 128)`；叶层调度误差为 0。
  它是调度策略，不声称是测得的严格三维最大误差。
  报告记录各层三角面、输入渲染顶点、primitive 和图集尺寸。

### 高度与兼容

Copernicus DEM 默认正高没有被静默宣称为椭球高。提供正确的 GeographicLib
Geoid PGM 时，使用“正高 + 大地水准面起伏 + 手动偏移”；未提供时仅加显式偏移，
报告明确写入 `geoid correction not applied`。精确对齐其他来源需要配置模型。

现有依赖没有 quantized-mesh 写出器，所以新增只实现当前所需的标准基础块，
复用 Zlib 压缩、GeographicLib 坐标及现有 TIN；不手写压缩、重投影或图片编码。
建筑由现有场景构造器和 Assimp 写出，额外修正 Assimp 6.0.4 的 float accessor
边界和 KHR_texture_basisu 纹理引用，二进制顶点/索引不变。
经用户确认新增 KTX-Software 4.4.2，由 vcpkg 管理并静态链接；
不依赖外部压缩命令、Node.js 或新增 DLL。发行目录附带 KTX 和内置库许可证。

### 验证入口

- `CesiumExportTest`：独立 terrain 读取器核对头部、量化、索引、朝向和边界，
  建筑各层资源、粗层 primitive 合并和 UV、失败/取消路径；
  用 libktx 实际转码 ETC1S 为 RGBA，校验颜色、透明度、完整/单层 mip、
  非 512 贴图、奇数尺寸对齐、写入失败及编码前后取消。
  建筑增加真实模式的全零 UID 回归，对照 FBX 散图摘要；
  验证选图预计算、分块重排、轮廓简化、无效记录对齐、越界输入及取消。
- `ExportTaskManagerTest`：组件参数合法性、Mipmap 重启后恢复和旧配置默认关闭。
- 在 Release 目录运行 `ExportTaskManagerTest.exe --real-cesium-task`：
  通过正式任务管理器执行 DEM 准备和两类组件导出，包含真实任务记录和中文输出目录。
- 在 Release 目录运行 `CesiumExportTest.exe --real-data`：
  真实 Copernicus DEM、高德建筑和 XYZ，建筑同时生成无 mip / 完整 mip 两套，
  产物留在 `tmp/postprocess-tests/cesium-real-data/`。
- `tools/cesium-preview/server.mjs`：本地静态服务、terrain gzip 响应头和请求日志。
- `tools/cesium-preview/verify.mjs`：Playwright 桌面/移动截图、画布像素、
  近远建筑选层和地形选层、实际 KTX2 请求/MIME；
  报告默认位于 `tmp/cesium-browser-check/`。

尚未完成全量山区 KML、大范围压力、跨日期变更线及不同 Cesium 版本专项验收；
不得据小范围测试推断所有级差、数据缺口或极区都绝对无缝。

选图修复前的协议验证记录（2026-10-10）：中心 116.4 / 39.9、半径 400 米、
地形采样 30 米，
建筑叶块 200 米、3 层。391 栋建筑在各层完整保留；粗层共 4 个 primitive /
9795 输入渲染顶点 / 5095 三角面，叶层为 60 / 10480 / 5506。
44 个 GLB 经 Khronos glTF Validator 校验无错误。
1440 x 900 与 390 x 844 的浏览器检查均选到近景叶层和远景粗层，
本地 XYZ 和 Esri 在线影像切换无请求/渲染错误。OpenStreetMap 在本机出现连接关闭，
不能把该网络来源宣称为已通过。

选图修复前的 KTX2 复测：同一批真实建筑的两种 mip 模式分别为 44 个 GLB、18 张纹理，
全部纹理通过 `ktx validate --gltf-basisu --warnings-as-errors`；
原生转码、15 项相关回归和静态运行库检查通过。
glTF Validator 2.0.0-dev.3.10 的 GLB 错误数为零，但由于不支持 BasisU
产生图片 MIME/格式警告，不能据此声称纹理完整校验无警告。
两种模式的桌面/手机 CesiumJS 1.146.0 实际加载、近远 LOD 和 XYZ 测试通过。
保留的测试产物与报告位于 `tmp/`；本次未测量真机 GPU 显存或帧率收益。

2026-10-11 选图修复复测：同一批 391 栋建筑，各层仍完整保留。
叶层为 331 个 primitive / 10480 输入渲染顶点 / 5506 三角面；
粗层仍为 4 / 9795 / 5095。面数和顶点数未变，叶层 primitive 增加来自
恢复不同建筑的匹配散图，不能把修复前错误共用首张纹理当作正确性能基线。
两种 mip 模式分别包含 44 个 GLB、68 张共享散图和 16 张粗层图集，
全部 168 张 KTX2 通过严格 BasisU 格式校验。
目标墙面在 50、5、0.5 米处均由屏幕中心实际拾取确认使用叶层散图，
源贴图由错误的 64 x 64 恢复为按原规则选中的 2048 x 256。
清理临时诊断代码后的 Release 构建和 11 项相关 CTest 回归通过。
两套共 88 个 GLB 校验零错误；校验器不支持 BasisU 的警告仍保留。
清理后重新执行完整真实导出，两种 mip 模式的桌面/手机本地浏览器检查通过，
包含 XYZ、地形、近景叶层、远景粗层、画布非空和 KTX2 响应。
本机截图、选层和格式报告保留于 `tmp/cesium-texture-reuse-*`。

## 主要来源

- 3D Tiles 1.1 规范：
  <https://github.com/CesiumGS/3d-tiles/blob/main/specification/README.adoc>
- 隐式四叉树与子树规则：
  <https://github.com/CesiumGS/3d-tiles/blob/main/specification/ImplicitTiling/README.adoc>
- CesiumJS 发布记录：
  <https://github.com/CesiumGS/cesium/releases>
- CesiumJS 1.146 源码与发布历史：
  <https://github.com/CesiumGS/cesium/blob/1.146/packages/engine/Source/Scene/Cesium3DTileset.js>
  <https://github.com/CesiumGS/cesium/blob/1.146/CHANGES.md>
- 原生影像铺设接口与局限：
  <https://cesium.com/learn/cesiumjs/ref-doc/Cesium3DTileset.html#imageryLayers>
  <https://github.com/CesiumGS/cesium/issues/12643>
- 裙边及地形层次格式参考：
  <https://github.com/CesiumGS/quantized-mesh>
  <https://github.com/CesiumGS/quantized-mesh/blob/main/README.md>
  <https://cesium.com/learn/cesium-native/ref-doc/structCesiumGltfContent_1_1SkirtMeshMetadata.html>
- 地形加载及独立影像接口：
  <https://cesium.com/learn/cesiumjs/ref-doc/CesiumTerrainProvider.html>
  <https://cesium.com/learn/cesiumjs/ref-doc/Globe.html>
  <https://cesium.com/learn/cesiumjs/ref-doc/UrlTemplateImageryProvider.html>
- CesiumLab 手册的地形标准索引（本轮未获取到 PDF 正文）：
  <https://m.cesiumlab.com/CesiumLab%E5%9C%B0%E7%90%86%E4%BF%A1%E6%81%AF%E5%9F%BA%E7%A1%80%E6%95%B0%E6%8D%AE%E5%A4%84%E7%90%86%E5%B9%B3%E5%8F%B0%E4%BD%BF%E7%94%A8%E6%89%8B%E5%86%8C.pdf>
- 开源转换工具：
  <https://github.com/ahuarte47/cesium-terrain-builder/tree/master-quantized-mesh>
  <https://github.com/heremaps/tin-terrain>
  <https://github.com/OpenDroneMap/Obj2Tiles>
- RTIN 对照：
  <https://github.com/mapbox/martini>
- 建筑约束减面参考：
  <https://doc.cgal.org/latest/Surface_mesh_simplification/index.html>
- Copernicus DEM 高度基准：
  <https://dataspace.copernicus.eu/explore-data/data-collections/copernicus-contributing-missions/collections-description/COP-DEM>
- 官方校验器和格式工具：
  <https://github.com/CesiumGS/3d-tiles-validator>
  <https://github.com/CesiumGS/3d-tiles-tools>

本地参考：`include/FbxTerrainTriangulator.h`、`src/FbxTerrainTriangulator.cpp`、
`src/AmapBuildingFbxExporter.cpp`、`docs/knowledge/terrain-simplification-study.md`。
