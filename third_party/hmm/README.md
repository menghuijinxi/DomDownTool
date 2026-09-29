# hmm 算法适配

- 上游：https://github.com/fogleman/hmm
- 固定提交：`5ea611461e9f8b40f3d97e9f10fcf0f40592030b`
- 许可：MIT，见 `LICENSE.md`。
- 派生实现位置：`src/FbxTerrainTriangulator.cpp`。

采用 `src/triangulator.cpp` 的逐点细化、半边局部换边、索引最大堆，
以及 `src/heightmap.cpp` 的三角形栅格误差评价思路。未引入上游命令行、
图片处理、GLM 或共享指针高度场，不增加链接依赖。

项目适配：既有 CDT 初始化固定矩形边界；只允许内部插点；double 米制误差；
扫描线整数区间；严格面数预算；显式换边栈；输入校验；边界误差单独统计。
内部使用上游顺时针方向，对外保持原接口的逆时针方向。
