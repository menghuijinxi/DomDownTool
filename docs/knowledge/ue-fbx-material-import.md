# UE FBX 材质导入约定

## 2026-09-29：统一材质实例与建筑导入

现象：普通 UE FBX 导入为每个地块创建独立材质，统一调整 Roughness、Specular 等很繁琐。
建筑 atlas 也不能仅靠把 Diffuse 贴图接入 BaseColor 正确显示。

排查确认：

- 底板 `CreateAssimpMaterial` 保存 Diffuse 相对路径。
- 建筑导出按散图材质或 atlas 合批；高度、贴地、墙体/屋顶几何、球面变换以及卫星屋顶切分已烘入 FBX。
- atlas 的 UV0 保存重复坐标，UV1/UV2 保存所选子图范围。
- UE 5.5 的传统 FBX importer 会对所有 UV 通道翻转 V，连承载范围值的 UV1/UV2 也会翻转。
- UE 可以在 FBX 导入过程中生成材质实例，但 FBX 本身不包含 UE 母材质图及实例资产。
  自动贴图参数映射也不等于理解建筑 atlas shader。

在 `AssetEditorTool_MHJX` 插件增加专用导入入口，读取现有 FBX 信息创建母材质、
总调节实例、底板/墙体/屋顶调节实例及各贴图子实例。不改下载器几何，不增加重复清单 JSON。

## 两侧必须保持一致的约定

| 名称 | 导出信息 |
| --- | --- |
| `tile_*` | 底板，UV0 直接采样 |
| `building_wall_*` | 散图墙体，UV0 横向重复 |
| `building_roof_*` | 散图屋顶或卫星屋顶，UV0 直接采样 |
| `building_wall_atlas` / `building_roof_atlas` | atlas，UV0 重复坐标 + UV1 min + UV2 max |

这些名称是本项目的导出约定，不是 FBX 标准语义。修改名称或 UV 编码时需同步插件；
不能仅通过额外 UV 通道数量识别 atlas。
材质实例数量仍与贴图分配对应，共享父实例解决的是集中调参，不是减少 draw call。

插件禁止自动光照 UV 覆盖 UV1/UV2，并使用全精度 UV；
开启 Nanite 时对 atlas 禁用 UV 插值。保留 FBX 法线及三角拓扑，不在本次修改中处理旧高光问题。

## 排查清单

1. 普通贴图丢失：检查 FBX Diffuse 路径以及 MainTex 参数绑定。
2. atlas 错位：检查 UV 通道顺序、三路 V 翻转与整数重复边界。
3. 材质串用：检查 ImportedMaterialSlotName，而非数组下标或导入后的网格名称。
4. 重导入修改丢失：更新叶实例的 MainTex，兼容旧图时设置 AtlasMode，
   保留用户修改的父材质图。
5. 多任务同名冲突：按完整 UTF-8 来源路径摘要隔离，不能使用有损 ANSI 路径摘要。

测试在独立 UE 5.5 项目中运行 `MHJX.DomDown`，使用 `tmp` 中原有的下载器导出样例；
不修改用户的业务关卡、项目配置及现有材质。具体入口、限制和测试命令参数见插件
`Docs/DomDownImport.md`。

UE 源码参考：`UnrealEd/Private/Fbx/FbxMaterialImport.cpp`、
`UnrealEd/Private/Fbx/FbxStaticMeshImport.cpp`、
`AssetTools/Private/AssetTools.cpp`。未引入新第三方依赖，沿用 UE 5.5 自带 FBX SDK、
AssetTools 和 MaterialEditor。

## 自动化验证注意事项

- UE FBX 原生重导入保留已有网格的 Nanite 开关。新建网格应用导入选项，
  重导入不可再次显式套用面板开关，否则会覆盖用户的资源设置。
- 自动截图前必须提交并等待材质实际需要的 shader。仅等待当前编译队列可能遗漏延迟编译，
  导致截图拍到 WorldGridMaterial。测试检查渲染线程是否使用 fallback，并检查彩色像素，
  不把「成功写出 PNG」当作渲染正确。
- 本机 `Automation Test Queue Empty` 退出方式可能在断言失败时仍返回进程退出码 0。
  必须检查报告里的 `failed`、`notRun` 和各测试状态，不能只看退出码。

## 2026-09-30：父材质拆分与平滑组提示

现象：旧导入器让地面与建筑共享带 atlas 逻辑的父材质，而且仅导入地面也创建
墙体、屋顶调节实例。表面参数默认值与本项目导出未提供 PBR 信息的语义不符。

解决方案：按实际材质绑定创建 `M_DomDownTerrain` 和 `M_DomDownBuilding`，
前者只采样 UV0，后者支持墙体/屋顶 atlas。贴图实例直接继承对应父材质，
不生成总调节或分类调节实例。Specular、Roughness、Metallic 使用常量 0、1、0，
不自动暴露表面参数；后续效果由用户编辑父材质图，重导入不重建已有父材质。
旧层级只迁移本次重导入的贴图实例，不删除旧资产，也不覆盖用户另外指定的父材质。

平滑组提示排查：

- `DownToolBase.cpp::CreateAssimpMesh` 和建筑导出都提供法线；底板、建筑现有样例含
  `LayerElementNormal`，但没有 `LayerElementSmoothing`。
- UE 5.5 `FbxMainImport.cpp::CheckSmoothingInfo` 单独检查平滑组层；
  缺少它就产生提示，并不检查法线是否已有。
- 该检查带 `!GIsAutomationTesting` 条件。自动化没有此提示不是交互导入问题已解决的证据。
- 导入器使用 `FBXNIM_ImportNormals` 且关闭 `bRecomputeNormals`，本次不修改
  下载器法线、几何或静默屏蔽警告。提示本身不能证明之前的高光问题根因是平滑组。

验证入口：`MHJX.DomDown.MaterialPolicy` 与 `MHJX.DomDown.RealExports`，
覆盖按需生成、简单地面 shader、表面常量、旧层级迁移、父材质编辑保留和实际贴图渲染。
今后排查先分别确认 FBX 的法线层、平滑组层及 UE 导入法线选项，不能把三者混为一谈。

### 后续修正：只按 atlas 模式分配父材质

用户指出：散图墙体和普通屋顶与地面一样只需 UV0 采样，复杂逻辑只用于合并图。
按 `Wall/Roof` 选择复杂材质仍然会给散图和卫星屋顶增加不必要的 shader。

当前规则改为按每个材质绑定的 `bAtlas` 分配：
非 atlas 的地面、散图墙体/屋顶及卫星屋顶共用简单父材质，
只有墙体/屋顶 atlas 使用复杂父材质。简单墙体仍通过贴图 U Wrap 支持横向重复。
资产路径继续沿用 Terrain/Building 名称以保留已有父材质及用户修改，但不是按表面分类。
新 atlas shader 去掉散图分支和 AtlasMode；旧复杂父材质有该参数时设置为 1 兼容。
散图实例重导入会从上一版的建筑父材质迁移到简单父材质，移除旧 AtlasMode 覆盖，
不删除旧资产或重建已有父材质图。

回归入口 `MHJX.DomDown.TextureModes`：独立导入散图建筑、卫星屋顶建筑、
atlas 建筑，检查只创建所需父材质、墙体/屋顶直接共享对应父材质和旧实例迁移。
混合导入与实际渲染继续由 `MHJX.DomDown.RealExports` 覆盖。

## 2026-09-30：重导入确认与资源设置保留

现象：专用导入器没有覆盖确认，重导入后用户手动调整的减面、LOD、Nanite
及贴图参数会丢失。影响合并和未合并 FBX，未合并网格的设置丢失范围更大。

根因和排查：

- 导入任务设置 `bAutomated=true`、`bReplaceExisting=true`，因此不会出现
  通用导入器的覆盖询问，必须在本入口执行写操作前统一确认。
- `UFbxFactory` 只对入口名称对应的已有资源转到原生重导入；未合并多网格的实际
  名称带节点后缀，会经过覆盖创建，不能假定设置 `bReplaceExistingSettings=false`
  就完整保留所有静态网格参数。
- 导入后无条件写 Nanite 开关、BuildSettings 和贴图默认值，会进一步覆盖用户修改。
- UE 5.5.4 `StaticMeshImportUtils::SaveExistingStaticMeshData` 按渲染 LOD 遍历，
  新增但尚未构建的 LOD 可能没有完整快照；还需要保存这些源模型的设置。
- 贴图普通覆盖创建仅恢复部分参数；`FReimportManager` 的原生贴图重导入在原对象
  上更新 Source，保留最大尺寸等其他资源属性，不能用手工复制少数参数替代。

解决方案：

1. 校验所有输入后，查询各来源目录已有资源，整批弹一次确认；拒绝时没有资源写入。
2. 对已有网格沿用 UE 的导入与快照恢复流程，按资产路径恢复 LOD、减面、构建、
   碰撞等已有设置，另外保留未构建源模型及引擎会按导入选项修补的 Build/Nanite 设置。
3. 已有图片通过 `FReimportManager::Reimport` 更新；资源默认值只用于新建网格和贴图。
4. 继续同步生成实例的 MainTex 绑定，并保留已有父材质图；旧资产不自动删除。

验证：`MHJX.DomDown.ReimportSettings` 在独立项目复制输入，修改未保存的资源设置，
同时更新源顶点和 PNG 像素。检查合并/未合并两条路径、接受/拒绝、混合批次取消、
Nanite 开关与面板选项相反、贴图属性保留及数据确实更新。最终 D3D12 SM6 为 6/6 通过。
交互窗口的实际点击仍需人工验收，不能把无人值守默认拒绝视为界面已验收。

本地源码参考：UE 5.5 `UnrealEd/Private/Fbx/FbxFactory.cpp`、
`FbxStaticMeshImport.cpp`、`Private/ImportUtils/StaticMeshImportUtils.cpp`、
`Private/Factories/EditorFactories.cpp`。使用游戏线程和现有 UE 模块，不引入第三方库。
UE 部分 ReimportFactory 类型/内部函数没有模块导出，插件应通过公开管理器入口调用，
不能依赖引擎内部未导出的类构造函数。

快速检查：先辨别「重新导入」与「覆盖创建」路径，再查导入后是否无条件赋默认值；
同时验证未保存设置、资源持久化和源数据刷新，不能只看资源对象是否仍存在。

## 2026-09-30：导入资源由用户保存

原入口在导入末尾显式调用 `UPackage::SavePackage`，导致新建及重导入资源立即落盘，
还会连带保存用户之前未保存的修改。移除该保存流程，并明确设置网格和贴图导入任务
`UAssetImportTask::bSave=false`，保留正常的 `MarkPackageDirty`。
已有且未修改的父材质保持原状态，不为显示未保存标记而强制标脏。
内容目录仍通过 `GEditorPerProjectIni` 保存用户偏好，与 `.uasset` 保存分开。

回归入口 `MHJX.DomDown.UserControlledSaving` 在旧实现上失败，修正后独立项目
D3D12 SM6 的 8/8 测试通过。覆盖合并/未合并导入，检查新资源无磁盘文件且标脏、
未保存资源也能确认后重导入；主动保存后再次导入，磁盘哈希和时间均不变，
之后由用户主动保存才持久化更新。测试的 SavePackage 仅模拟用户保存，不是生产逻辑。

快速检查：区分资源已创建、包已标脏和磁盘已保存三个状态；不能只关闭任务保存选项，
却保留入口末尾的独立保存调用。正式 `.uasset` 内的来源元数据也要等用户保存才落盘。

## 2026-10-10：DOM 卫星屋顶重复导入贴图

现象与范围：地形 FBX 和建筑 FBX 同时或分批导入同一内容目录时，DOM 卫星屋顶
使用的地面图片被重复创建为 Texture2D。地面与屋顶的材质实例引用了不同资源，
重复占用空间，用户对贴图的调整也不能统一生效。

排查：用下载器真实导出副本让屋顶引用地形同一张图片。旧实现回归测试应有
3 张独立图片，实际为 4 张；打印对象路径确认图片来源、摘要名称和 Clamp 模式
完全相同，但分别位于 `plane_<摘要>/Textures` 和 `buildings_<摘要>/Textures`。
导出器的卫星屋顶本身直接引用地形 DOM，并未复制图片；UE 自动贴图导入也已关闭。

根因：`BindMaterials` 的贴图表只在单个 FBX 内有效，`ImportTexture` 又只查找
当前 FBX 的贴图子目录。来源去重键虽然相同，目录隔离仍会创建第二个资源。

修复：

- 把规范化图片来源路径和导入 Wrap/Clamp 模式的复用表提升到整批导入。
- 使用 UE 资源注册表查询同一目标目录中的 Texture2D，并核对
  `UAssetImportData::GetFirstFilename()`，支持跨批次和未保存资源复用。
- 保留已有文件布局，图片由首次导入的 FBX 目录持有，不新增清单或缓存文件。
  不自动删除历史重复资源；重导入当前实例时绑定稳定选出的共享资源。
- 同一图片每批最多执行一次原生重导入，沿用用户的贴图属性和不自动保存策略。
  新 FBX 会刷新旧共享图片时，仍在整批写入前确认；拒绝时不创建部分资源。
- 不按名称或像素盲目合并。Wrap 墙体与 Clamp 地面/屋顶需要不同 Texture2D；
  不同目标目录保持隔离，用户手动改动的资源设置不被默认值覆盖。

回归入口：`MHJX.DomDown.SharedDomTextures`。覆盖两种文件顺序、同批和分批导入、
不同目标目录、未保存资源、共享图片单次刷新及跳过刷新、用户属性保留、
Wrap/Clamp 分离和共享资源覆盖确认。测试副本与报告只写入独立测试项目的 `tmp`。
修复后插件模块构建通过，独立 UE 项目 D3D12 SM6 全部 10/10 测试成功，
失败和未运行均为 0；模拟用户保存的测试仍有 6 条禁用 Niagara 的类型提示。

UE 5.5.4 本地源码参考：`AssetRegistry/IAssetRegistry.h`、`AssetRegistry/ARFilter.h`、
`EditorFramework/AssetImportData.h`、`UnrealEd/Public/EditorReimportHandler.h`。
使用游戏线程及 UE 原生注册表/重导入管理器，无第三方依赖；资源注册表查询包含
内存中的未保存资源，不能仅查询磁盘包。贴图寻址按导入语义参与身份判断，不能因
用户调整了已有 AddressX 就丢失原资源并重新创建。

快速检查：先比较 FBX 实际图片路径、导入寻址和生成资源完整对象路径；
然后检查去重表生命周期是否覆盖整批、已有资源搜索是否被单个 FBX 子目录限制。
仅验证两个模型各自导入成功，无法发现跨模型重复资源。

## 2026-10-26：建筑墙体与普通屋顶贴图上下颠倒

现象与范围：散图及合并图集的建筑墙体上下颠倒，普通屋顶贴图南北颠倒。
Cesium 建筑复用同一套源网格生成逻辑，也会受到影响；卫星屋顶和底板已有正确方向。

根因：墙体生成时把墙脚设为 V=1、墙顶设为 V=0，普通屋顶则额外计算了
`1 - normalizedNorth`。这些是图片从上向下的行坐标，不能直接作为 Assimp/FBX
从下向上的 V 坐标。图集范围本身已按下方原点计算，因此翻转图集图片或 UV1/UV2
不能修正这个源 UV0 错误。

排查路径与验证：

- 使用上半红色、下半蓝色的非二次幂 PNG，经真实导出和 Assimp 重新读取后，
  按三角面中心的 UV 采样导出图片；图集同时应用 UV1/UV2 的子图范围。
- 修复前 `AmapBuildingChunkTest` 报墙体倒置；只修墙体后继续报普通屋顶倒置，
  能分别定位两个问题。导出 PNG 仍然上红下蓝，排除了图片写出翻转。
- 原有测试只断言 V 覆盖 0 到 1，以及图集采样落在非空区域，不能识别上下颠倒。
- 回归覆盖散图拉伸、合并图集和卫星屋顶，检查墙顶及屋顶北侧采到红色，
  墙脚及屋顶南侧采到蓝色。

解决方案：墙脚改为 V=0、墙顶改为 V=1；普通屋顶改为 `normalizedNorth`，
与卫星屋顶和底板保持一致。保留原图片方向、横向重复、图集通道布局及导入约定。
已有模型需要重新导出，单独重新导入旧 FBX 不会改变旧 UV。

第三方边界：项目使用 Assimp 6.0.4，其源码
`code/AssetLib/FBX/FBXExporter.cpp` 直接写入网格 UV；
`code/AssetLib/glTF2/glTF2Exporter.cpp` 在导出时将 V 转换为 `1 - V`。
Cesium 图集烘焙和 KTX2 编码继续沿用现有处理，不增加一次全局翻转。

快速检查：先对比源图与输出图的上下像素，再检查墙脚/墙顶、屋顶南侧/北侧的 UV0；
图集单独核对 UV1/UV2 范围，最后检查目标格式及导入器是否已执行坐标转换。
方向测试必须使用不对称图片，并验证实际采样颜色，不能只比较 UV 最小值和最大值。
