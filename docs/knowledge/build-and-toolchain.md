# 构建与工具链问题复盘

## 中文 MSVC 与 Ninja 的头文件依赖编码

- 现象：编译时大量打印“注意: 包含文件”，`ninja -t deps <object>` 显示 `#deps 0`，修改头文件
  后只有直接改动的源文件重编。涉及类布局时会生成 ABI 不一致的可执行文件。
- 根因：`cl.exe /showIncludes` 的重定向输出为 UTF-8，而 CMake 在系统代码页 936 下可能把
  `msvc_deps_prefix` 写成 GBK 字节。Ninja 按字节比较前缀，两者不一致时既不记录依赖，也不
  过滤包含提示。
- 解决方案：正式构建统一在代码页 65001 下运行 Visual Studio 随附的 CMake 配置和构建步骤。
  已经出现 `#deps 0` 的目录必须清理项目对象后重编；仅重新链接不能修复陈旧类布局。

```bat
chcp 65001 >nul
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^
  -S E:\Project\CMakeProject\DomDownTool ^
  -B E:\Project\CMakeProject\out\DomDownTool\build\x64-release
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^
  --build E:\Project\CMakeProject\out\DomDownTool\build\x64-release ^
  --target DomDownToolPreview --clean-first -j %NUMBER_OF_PROCESSORS%
```

- 验证方式：正常构建不再逐行输出 `/showIncludes` 内容；执行以下命令时依赖数应明显大于 0：

```bat
ninja -C E:\Project\CMakeProject\out\DomDownTool\build\x64-release -t deps ^
  CMakeFiles/DomDownToolPreview.dir/src/preview_main.cpp.obj
```

- 快速检查清单：确认配置和编译阶段使用相同代码页；确认没有混用不同版本的 CMake；确认
  `rules.ninja` 中的 `msvc_deps_prefix` 与 `cl /showIncludes` 的原始输出编码一致；类布局变化后
  对旧构建目录至少执行一次项目级清理重编。
