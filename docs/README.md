# 项目文档

## 使用指南

- [高德 GIS 下载与 PNG 渲染](amap-gis-download.md)：构建命令、参数说明、常用命令、
  输出目录和 C++ 调用示例。
- [卫星图和栅格地图命令行下载](satellite-download.md)：地图源、下载范围、缓存、合并大图、
  金字塔和 FBX 参数。
- [预览器导出与导出任务](preview-export.md)：在 `DomDownToolPreview.exe` 中配置范围、GIS
  分组、输出格式，并查看排队任务、实时进度和历史记录。

## 设计与实现

- [瓦片后处理流水线](design/tile-postprocess-pipeline.md)
- [下载组件模型](design/download-component-model.md)：导出配置改为内容组、输出组件和参数。
- [高德 Web 矢量瓦片](knowledge/amap-vector-tile.md)
- [腾讯 Web 矢量瓦片](knowledge/tencent-vector-tile.md)：前端协议对照、C++ 数据源、
  坐标与分类规则、建筑高度、独立缓存及导出边界。
- [Esri Wayback 历史影像](knowledge/esri-wayback.md)：在线预览、发布日期选择、
  版本缓存隔离、接口验证范围与离线导出授权条件。

## 问题排查

- [崩溃与异常](knowledge/crash-and-exception.md)
- [性能问题](knowledge/performance.md)
- [网络连接与系统代理](knowledge/network-and-proxy.md)
