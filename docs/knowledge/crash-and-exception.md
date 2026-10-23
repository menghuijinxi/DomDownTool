# 崩溃与异常问题复盘

## 命令行 HTTPS 下载完成后退出崩溃

- 现象：腾讯、百度电子地图首次联网下载已经写出 XYZ 和合并图片，但进程以
  `0xc0000005` 退出；复用完整缓存时正常。重定向的日志可能因缓冲尚未刷新，
  停在大图采样或下载前，不能据此认定该阶段崩溃。
- 影响范围：命令行下载器持有 HTTPS 连接时的退出流程，其他 HTTPS 地图源也有相同风险。
- 根因：`main` 中 `CurlGlobalContext` 晚于下载器构造，按逆序析构时先调用了
  `curl_global_cleanup()`。下载器随后执行 `curl_multi_cleanup()`，仍需关闭 TLS
  连接。本次 libcurl 8.21.0 使用 Windows Schannel，全局清理已将 `Curl_pSecFn`
  置空，`schannel_shutdown()` 调用 `ApplyControlToken` 时访问了空函数表。
- 排查路径：对照 WER 故障偏移与相同对象重链接生成的 MAP，定位到
  `schannel_shutdown`；反汇编中读取的全局地址与 `Curl_pSecFn` 一致。
  再核对 libcurl `lib/vtls/schannel.c`、`lib/curl_sspi.c` 和入口局部变量顺序。
  对比首次联网与缓存重跑，确认问题与图片内容、拼接算法无关。
- 解决方案：先构造全局 CURL 上下文，再创建下载器，让下载器及其连接先销毁，
  最后清理全局状态。利用 RAII 保证正常返回和异常退出遵守同一顺序。
- 验证方式：两家电子地图均使用全新空缓存，真实下载并输出 XYZ、合并 PNG，
  要求进程正常退出；同时运行命令行参数、图源 URL、投影和预览导出回归。
- 参考：`https://curl.se/libcurl/c/curl_global_cleanup.html` 要求在结束使用
  libcurl 后执行全局清理；连接和 multi handle 的销毁也属于 libcurl 调用。
- 快速检查清单：图片已生成但 HTTPS 下载进程退出崩溃时，先检查全局上下文是否
  比连接、下载器和后台线程活得更久；不要仅凭未刷新完的日志定位，也不要跳过
  清理或增加空函数表判断来掩盖生命周期错误。复现时必须用新缓存确保真正建立连接。

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

## Windows GUI 任务被标准输出异常误判为失败

- 现象：预览程序中的导出任务随机失败，转换记录只显示
  `cannot write to file: bad file descriptor`；失败前通常已经生成部分瓦片或大图。
- 影响范围：由 Windows GUI 子系统启动、没有有效标准输出句柄，但调用了
  `fmt::print` 的下载、缓存检测和后处理流程。命令行程序不受影响。
- 根因：`DomDownToolPreview` 使用 `WIN32` 子系统，不保证 `stdout` 和 `stderr`
  可写。导出核心的进度日志调用 `fmt::print` 时，fmt 12 会对写入失败抛出
  `fmt::system_error`，文本即为 `cannot write to file`。任务管理器捕获该异常后，
  将原本可继续执行的导出标记为失败。
- 排查路径：先查看任务转换记录中的精确错误，再搜索导出热路径的输出调用；本地
  fmt `format-inl.h` 的写入失败分支与记录文本完全一致。确认项目没有主动关闭标准
  输出后，即可定位为 GUI 子系统缺少输出句柄。
- 解决方案：预览程序进入 `wWinMain` 后立即将 `stdout` 和 `stderr` 重定向到 Windows
  空设备 `NUL`，再启动任何后台任务。CLI 保留原有控制台输出行为。
- 验证方式：`GuiStandardStreamsTest` 以 Windows GUI 子系统运行，调用同一个初始化
  函数后分别执行 stdout 和 stderr 的 `fmt::print`，要求无异常退出。
- 快速检查清单：GUI 程序出现 `bad file descriptor` 时，不要先假设产物文件写坏；
  先核对异常库的完整文本、可执行文件子系统、标准流是否有效，以及日志函数是否会
  抛异常。后台任务的诊断输出不应成为业务失败条件。
