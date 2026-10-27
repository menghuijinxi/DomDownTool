# 网络连接与系统代理

## Google 图源不能把 libcurl 默认行为当作 Windows 系统代理检测

### 现象与影响

Windows 已启用系统代理，Google 图源仍连接超时；将同一地址显式传给 libcurl 后，
相同瓦片请求立即返回图片。预览和导出都会受到影响。

### 根因与排查

- Google 下载器的代理字段默认为空，原先写死的本机代理已经被注释掉。
- 两条下载路径只会将非空的图源代理传给 `CURLOPT_PROXY`，没有读取 Windows 设置。
- CMake 的 `set(ENV{http_proxy} ...)` 只影响配置进程及其子进程，不是应用运行时配置。
- 命令行原先没有设置 `User-Agent`，即使代理连接成功，也可能收到 Google 的 403
  自动请求拦截页。诊断时必须检查 HTTP 状态、内容类型和图片解码，不能只看退出码。

### 解决方案与边界

- 创建 Google 下载器时调用 Windows 原生 `WinHttpGetIEProxyConfigForCurrentUser`，
  只读取当前用户已启用的手动代理。未配置、未启用、读取失败或没有 HTTPS 代理时，
  不设置图源代理。此路径不执行 PAC 脚本或 WPAD 自动发现。
- 统一代理地址用于 HTTPS；按协议配置时选择 `https=` 项，不误用仅供 HTTP 的代理。
  无协议前缀的代理地址显式按 HTTP 代理处理，HTTPS 目标仍使用 CONNECT 隧道。
- 检测结果只存入 Google 下载器实例，预览请求和导出请求复用该实例配置。
  显式 `--proxy` 和 `--no-proxy` 继续覆盖自动检测结果。
- 不修改 Windows 设置、进程代理环境变量或其他图源。删除 CMake 写死的本机代理。
- Google 请求统一携带应用 `User-Agent`，覆盖预览及命令行下载。

### API 约束与验证

- 本次使用 libcurl 8.21.0 与 Windows SDK 自带的 WinHTTP。
- `WINHTTP_CURRENT_USER_IE_PROXY_CONFIG` 的代理、绕过列表和自动配置 URL 字符串均由
  Windows 分配，必须分别调用 `GlobalFree`；实现使用 RAII 清理成功和失败路径。
- 每次创建图源独立读取配置，不保留全局可变代理状态。已创建的图源不实时跟踪设置变化；
  修改系统代理后重启预览程序或创建新的导出任务。
- `TileProviderUrlTest` 覆盖空配置、统一代理、协议选择、IPv6、请求参数传递、
  手动覆盖、关闭代理和其他图源隔离。接入前已确认真实系统代理回归断言失败。
- 修复后 10 项相关 CTest 通过；不传 `--proxy` 的全新缓存下载获得 4 张 Google
  瓦片，下载后缓存检测全部有效。未修改本机系统代理设置。
- 官方参考：
  [读取当前用户代理](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpgetieproxyconfigforcurrentuser)、
  [返回结构与启用条件](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/ns-winhttp-winhttp_current_user_ie_proxy_config)。

### 快速检查

- Windows “使用代理服务器”是否启用，Google 下载器是否刚刚创建？
- 系统是否只配置了 HTTP 代理，没有为 HTTPS 配置代理？
- 请求是否携带了检测到的图源代理和应用 `User-Agent`？
- 失败是连接超时、TLS 错误、HTTP 403，还是返回内容无法解码？
- 是否只在 Google 图源上设置代理，避免影响其他网络页面？
