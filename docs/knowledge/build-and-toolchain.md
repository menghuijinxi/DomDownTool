# 构建与工具链问题复盘

## 中文 MSVC 与 Ninja 的头文件依赖编码

- 现象：编译时大量打印“注意: 包含文件”，`ninja -t deps <object>` 显示 `#deps 0`，修改头文件
  后只有直接改动的源文件重编。涉及类布局时会生成 ABI 不一致的可执行文件。
- 根因：`cl.exe /showIncludes` 的重定向输出为 UTF-8，而 CMake 在系统代码页 936 下可能把
  `msvc_deps_prefix` 写成 GBK 字节。Ninja 按字节比较前缀，两者不一致时既不记录依赖，也不
  过滤包含提示。
- 解决方案：项目在配置阶段调用当前 MSVC 编译器执行一次最小 `/showIncludes` 探测，并让 CMake
  按 UTF-8 解码其输出，再把实际前缀写入 Ninja 规则。该过程同时兼容中文和英文 MSVC 输出，
  不再依赖终端代码页或 `VSLANG`。
- 旧构建目录如果已经出现 `#deps 0`，需要重新运行一次 CMake 配置，然后对目标执行一次完整清理
  重编。仅重新链接不能修复由陈旧类布局产生的 ABI 不一致；完成迁移后，日常增量构建会自动跟踪
  头文件变化。

```bat
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

- 快速检查清单：配置日志不能出现 `/showIncludes` 前缀探测失败；正常编译不能逐行泄漏“包含文件”
  输出；`rules.ninja` 中的 `msvc_deps_prefix` 应与当前 `cl /showIncludes` 输出一致；任一核心对象的
  `ninja -t deps` 结果都不应为 `#deps 0`。
