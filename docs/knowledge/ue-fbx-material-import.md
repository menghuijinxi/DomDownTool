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
