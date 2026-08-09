# 崩溃与异常问题复盘

## Release 增量构建产生类布局不一致

- 现象：临时 Release 正常，但正式 Release 在 GIS `mesh_build_complete` 后立即退出，日志里没有
  `dx12_draw_succeeded`；Windows 事件先后记录 `0xc0000005` 和 Win32 回调包装后的
  `0xc000041d`，故障偏移从 `0x163c9a` 变为 `0x163e07`。
- 根因：Ninja 未能解析中文 MSVC 的 `/showIncludes` 输出，所有对象的依赖记录均为 `#deps 0`。
  `GisOverlayRenderer` 增加线程、条件变量和 DX12 renderer 成员后，只重编了直接修改的源文件，
  `PreviewController.cpp.obj` 和 `preview_main.cpp.obj` 仍使用旧类尺寸，构造时覆盖了后续内存。
  被覆盖的 `Dx12GisRenderer::impl_` 最终在 `Draw` 中表现为空值或悬空值。
- 排查路径：WER 与 PE 反汇编先定位到 `Dx12GisRenderer::Draw`，但第二个故障点显示 `impl_` 在
  同一次调用中失效；随后比较正式版与正常临时版的 `wWinMain`，正式版栈空间为 `0x1440`，
  正常版为 `0x16e0`，相差 `0x2a0`。对象时间戳和 `ninja -t deps` 最终确认了陈旧对象。
- 解决方案：使用 Visual Studio 自带的 CMake，并在 `chcp 65001` 后重新配置同一构建目录；随后
  清理并完整重编 `DomDownToolPreview` 及其项目内依赖。不要用 renderer 空值恢复掩盖 ABI 覆盖。
- 验证方式：UTF-8 代码页下生成的 Ninja 规则能够吞掉 `/showIncludes` 行；单独重编
  `preview_main.cpp.obj` 后，其依赖记录由 `#deps 0` 恢复为 `#deps 269`。完整重编后的
  `wWinMain` 栈空间应与当前类布局一致。
- 快速检查清单：增量构建出现只在某个目录崩溃时，先比较对象时间戳、编译命令和类尺寸；运行
  `ninja -t deps <object>` 确认头文件依赖不是 0；修改含有直接成员的类布局后，必须保证所有
  调用方重新编译。

## 缓存检测结束后进程崩溃

- 现象：下载前缓存检测跑完后进程直接崩溃，表现为主流程没有机会输出后续错误信息。
- 影响范围：并发缓存扫描、并发低层级瓦片修复，以及任何在线程函数或 OpenCV 并行回调里访问文件系统、解码图片、写文件的流程。
- 根因：扫描 worker 和 OpenCV 并行回调里如果有 C++ 异常逃出线程边界，会触发 `std::terminate`；腾讯占位图检测曾在缓存扫描阶段高并发使用 OpenCV 解码图片，扩大了异常和资源竞争风险。
- 解决方案：缓存扫描 worker 内部捕获异常，把对应瓦片标记为无效并集中打印首个异常；并行修复回调内部捕获异常，避免异常逃出 OpenCV 回调；腾讯占位图检测改为文件大小 + MD5，不在缓存扫描阶段批量解码图片。
- 验证方式：Debug 和 Release 构建通过。
- 快速检查清单：遇到“某个并发阶段结束就崩”的问题，先检查线程函数、`std::thread` 创建过程、OpenCV `parallel_for_` 回调里是否有异常边界；文件系统查询优先使用带 `std::error_code` 的重载。
