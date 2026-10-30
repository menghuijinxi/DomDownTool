# 旧平面网格的 UE 坐标转换

兼容导出任务 `1790567037646-0`，不修改 FBX 或已有 UE 资产。实现仅依赖 UE 的
`Core` 模块，已在 UE 5.5.4 中编译和验证；适用于 UE5 的双精度 `FVector`。

将本目录的 `.h/.cpp` 放到调用方自己的 UE C++ 模块中。跨模块公开时，为类添加该模块
的导出宏。它是独立的普通 C++ 类，不依赖导入插件、PROJ、GDAL、碰撞或物理射线检测。

## 本批旧网格的参数

- 输入为 WGS84 经度、纬度，顺序是经度在前；不能直接传高德 GCJ-02 经纬度。
- 平面为 EPSG:3857，半径 6378137 米，共同原点为 `(113.144184, 22.186737)`。
- 导入后的局部坐标为厘米，东向 +X，南向 +Y，高度向 +Z。
- `高度/meta.json` 记录 `referenceMode: minimum`，
  `referenceElevationMeters: -46.816097259521484`，垂直基准是 EGM2008。
  这里是本轮最低高程，不是平均高程。
- 旧版有高度截平：`Z = max(0, 海拔 - 参考高程) * 100`。
  因而海拔 0 米在旧网格中对应局部 Z `4681.6097259521484` 厘米。

## 当前用法：只转换 XY，通过射线取 Z

使用 `TryLongitudeLatitudeToUEXY`，无需加载或部署 `LegacyPlaneSurface.bin`。
该接口只做平面投影，不访问地形、不计算 Z，也不应用旧高度偏移：

```cpp
#include "DomDownLegacyPlaneCoordinates.h"

FDomDownLegacyPlaneCoordinates Coordinates;
FVector2D LocalXY;
if (Coordinates.TryLongitudeLatitudeToUEXY(113.144184, 22.186737, LocalXY))
{
    // 未移动网格时，LocalXY 就是射线的世界 XY；射线起止 Z 由业务指定。
    const FVector TraceStart(LocalXY.X, LocalXY.Y, TraceTopZCm);
    const FVector TraceEnd(LocalXY.X, LocalXY.Y, TraceBottomZCm);
    // 按现有射线流程使用 TraceStart / TraceEnd，最终位置取命中点。
}
```

输入非法时返回 false，输出参数保持原值。投影不检查网格覆盖范围，范围外能算出
XY，但未必能射中地面。保持 `double` 和 UE5 的双精度 `FVector2D`，不要中途转为 float。

若网格有统一的平移、旋转或缩放，先把上述两个局部射线端点通过旧网格共同的
`TransformPosition` 转到世界空间，再执行射线。实际命中点已经包含旧网格的高度
偏移，不要再加减 `46.816097259521484` 米；FBX 导入时的 Y 轴转换也已包含在公式内。
本接口不修改现有网格或碰撞配置，沿用业务已有的射线和碰撞设置。

## 可选：不用射线时查询原三角面的 Z

只用经纬度无法凭投影公式得到地面高度。配套 `LegacyPlaneSurface.bin` 直接提取旧
`plane.fbx` 的三角形顶点，保留其已有 Z，再通过空间索引与重心插值取高。
这里没有重新采样 DEM 或重新生成网格，不需要重新导出或重导入任何资产。

本机数据位于：

```text
D:/CMakeProject/DomDownTool/tmp/legacy-plane-1790567037646-0/LegacyPlaneSurface.bin
```

文件为 44,619,948 字节，约 42.55 MiB。程序初始化时加载一次，之后重复查询：

```cpp
#include "DomDownLegacyPlaneCoordinates.h"
#include "Misc/Paths.h"

FDomDownLegacyPlaneCoordinates Coordinates; // 在业务对象中持久保存
FString Error;
const FString SurfaceFile =
    FPaths::ProjectContentDir() / TEXT("LegacyPlane/LegacyPlaneSurface.bin");
if (!Coordinates.LoadSurface(SurfaceFile, Error))
{
    UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
    return;
}

FVector LocalPosition;
if (Coordinates.TryLongitudeLatitudeToUE(113.144184, 22.186737, LocalPosition))
{
    // CommonMeshTransform 是旧网格在当前世界中的共同变换；未移动时为 Identity。
    const FVector WorldPosition = CommonMeshTransform.TransformPosition(LocalPosition);
    // 使用 WorldPosition 放置目标；额外离地高度在需要时自行添加。
}
```

把数据部署到实际 `LoadSurface` 路径；打包时将对应目录加入额外非资产文件目录。
`LoadSurface` 创建约 250 米一个格子的三角形索引，查询不扫描整张网格。
加载完成后的 const 查询可并行调用；不要同时重载同一实例。

`TryLongitudeLatitudeToUE` 返回 false 表示尚未加载数据、输入非法、超出原网格或位于
没有三角形覆盖的位置，输出参数保持原值。它不会悄悄返回 Z=0 或吸附到远处地面。
局部 Z 已包含旧高度偏移，调用后不要再减参考高程或额外加 46.816 米。

## 已经有海拔：只做同一坐标规则的转换

无需加载查询数据即可使用固定的本批任务参数：

```cpp
FDomDownLegacyPlaneCoordinates Coordinates;
FVector LocalPosition;
const FVector3d LongitudeLatitudeAltitudeMeters(113.144184, 22.186737, 0.0);
if (Coordinates.TryLongitudeLatitudeAltitudeToUE(
    LongitudeLatitudeAltitudeMeters, LocalPosition))
{
    // LocalPosition = (0, 0, 4681.6097259521484)，单位厘米。
}
```

高度必须是 EGM2008 海拔米值，不能直接传 GPS 椭球高。此接口不会吸附地面；
即使 DEM 数值与旧版相同，旧三角面内部的线性插值也可能与 DEM 的插值不同。
要贴合现有地面，应使用只有经纬度的查询接口。

## 验证范围

源文件 117 块，Assimp 读取 3,718,326 个带面角属性的顶点和 1,239,442 个三角面。
全部源顶点根据旧贴图 JSON 像元范围及 UV 独立还原经纬度，水平最大差异
`0.3754657405` 厘米；对原 DEM 的 286,060 个顶点抽样也确认旧高度基准。

在目标 UE 项目中用导入插件相同的 `UFbxFactory` 几何转换设置实际导入整份旧 FBX，
只跳过材质与图片处理，不保存资产。结果是 644,590 个去重顶点和原有全部三角面。
检查每个顶点、每个三角面重心及一个非对称内部点，零查询遗漏，
与导入三角面的误差远小于毫米。另有按 UV 独立还原经纬度的逐面重心检查，
1,239,442 次查询无遗漏，最大三维差异 `0.3386727378` 厘米，
最大水平差异 `0.3317698750` 厘米，最大竖直差异 `0.2317837617` 厘米。
原始结果见本机 `ue-uv-validation.json`。这些数值是实测样点最大值；
取高本身是在原三角形内求线性插值，不使用近似栅格高度图。

新增纯 XY 接口也已在目标 UE 5.5.4 中编译验证。全程未加载地面数据，对同一组
1,239,442 个独立 UV 经纬度样点全部投影成功，水平最大差异仍为
`0.3317698750` 厘米，覆盖原点、轴向、输入边界和非法输入的输出保持行为。
结果见本机 `ue-xy-validation.json`；业务调用不需要测试用的查询文件。

精度结论针对这份旧平面网格的 LOD0 和统一变换下的相对位置，不代表卫星影像、DEM
或现场测量本身的地理精度。修改过顶点、地形变形、独立移动各块或使用低精度 LOD 时，
需要针对修改后的实际表面另行取高。球面网格不能使用此接口。

三项 UE 自动化测试状态均为 Success，坐标断言错误 0。加载不存在文件的反例会产生
预期的文件读取警告。测试接入已从目标插件清理，插件恢复后重新构建；
本工具文件保留在此目录，查询数据、复现探针及报告保留在 `tmp/`。
