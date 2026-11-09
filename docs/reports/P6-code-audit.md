# P6 产品隐私与便携性代码审计

审计日期：2026-11-06。结论：在本次检查的产品源码与两个 `dist` 二进制中，**未发现产品直接网络请求、遥测上报、下载或更新客户端；未发现依赖外置 VC/UCRT 运行库的静态导入**。这是限定范围的静态审计结论，不是“进程及系统组件绝不会产生网络流量”的动态证明，也不代替干净 OS、Win7 或第三方应用验收。

本轮只读产品代码和二进制，未启动产品、未运行桌面测试，未修改生产代码。报告和 `dumpbin` 输出是本轮新增的审计产物。

## 范围与二进制身份

检查 `src` 中除 `src/spikes` 外的源码、[CMakeLists.txt](../../CMakeLists.txt)，并对下面两个真实发布目录文件重新执行 `dumpbin /imports`。没有拿开发工具或构建目录中的同名 exe 代替。

| 文件 | 字节数 | SHA-256 | 导入证据 |
|---|---:|---|---|
| `dist/VisPick.exe` | 778240 | `B07A5E3D6B5D2976052AD0179072815167AF4517070F7637E18F7D39CAAF92CA` | [x64 imports](data/p6-imports-x64.txt) |
| `dist/VisPick-x86.exe` | 623104 | `E3161A8247A120A10ECFA5EA541AD35DA7E0FB9527E201744731B2A13342D7A2` | [x86 imports](data/p6-imports-x86.txt) |

本快照已包含最终预览 DPI 与隐藏启动修复；x64 / x86 文件修改时间分别为 2026-11-06 15:24:04 / 15:24:27（UTC+8）。工具为 MSVC 14.42.34444 `dumpbin`。x64 / x86 分别有 322 / 313 个命名静态导入；DLL 集合相同，均为以下 11 项：`USER32`、`GDI32`、`SHELL32`、`ole32`、`dwmapi`、`WINMM`、`ADVAPI32`、`WTSAPI32`、`gdiplus`、`IMM32`、`KERNEL32`。

哈希只对应这次快照。后续替换 `dist`、修改链接或更新源码后，需重新生成导入表并核对本文；不能把本次结论自动附到新的二进制。`out/` 默认可能不入库，归档报告时应一起保存对应导入表。

## 网络与进程入口

| 检查 | 证据与判定 |
|---|---|
| 产品直接联网 | 非 spikes 源码未检出 WinHTTP、WinINet、Winsock/socket、URL 下载或 WebSocket 调用，也未找到产品服务端 URL/遥测路径；两架构未导入 `winhttp`、`wininet`、`ws2_32`、`wsock32`、`urlmon`。CMake 产品链接项没有网络库 |
| 外部命令执行 | 产品源码未发现 `CreateProcess`、`WinExec`、`system` 或 `_popen` 调用；对应两架构静态导入也没有 `CreateProcess`/`WinExec` |
| Shell 打开 | 两处 `ShellExecuteW(..., L"open", ...)`：用户“打开配置”操作交给配置关联编辑器；用户点击保存通知时打开保存目录。未传命令拼接的 shell 参数、未指定 `runas`。证据：[App.cpp](../../src/App.cpp)、[SaveDialog.cpp](../../src/output/SaveDialog.cpp) |
| 单实例通信 | `Local\\` 命名互斥体带当前用户 SID，命令通过自有窗口 `WM_COPYDATA` / `SendMessageTimeoutW` 传递；不是网络 IPC。载荷包含命令、显式配置/贴图路径和性能日志标志。证据：[SingleInstance.cpp](../../src/platform/SingleInstance.cpp) |
| 剪贴板 | 用户复制时发布 PNG 与 DIB；贴图粘贴操作读取图片/文件路径。没有发现产品轮询并上传剪贴板的路径。系统剪贴板历史或同步是否启用不在产品控制范围。证据：[Clipboard.cpp](../../src/imaging/Clipboard.cpp)、[App.cpp](../../src/App.cpp) |

以上不能推断 Shell、文件关联程序、WIC/COM 扩展、Defender、Windows 错误报告或其他系统服务的内部行为。用户选择 UNC、映射驱动器、同步目录，或系统将 Desktop/APPDATA/TEMP 重定向后，常规文件操作也可能触发系统网络或同步访问；这与产品直接实现遥测/HTTP 客户端是不同的事实。

## DLL 加载与按需组件

| 路径 | 实际做法 | 边界 |
|---|---|---|
| 可选 DPI 库 `shcore.dll` | [Compat.cpp](../../src/platform/Compat.cpp) 用 `GetSystemDirectoryW` 拼接绝对路径再 `LoadLibraryW`；解析 `GetDpiForMonitor`、`SetProcessDpiAwareness`，不存在时走旧接口 | 未使用配置项、当前目录或 exe 同目录来指定该显式加载目标 |
| 已加载系统模块 | 从 `GetModuleHandleW(user32.dll/ntdll.dll)` 解析新 DPI、窗口捕获排除和版本 API；新增 `AdjustWindowRectExForDpi` 同样从已加载 `user32.dll` 解析，旧系统回退到 `AdjustWindowRectEx`。[Recorder.cpp](../../src/recording/Recorder.cpp) 从已加载 `kernel32.dll` 解析 `CreateWaitableTimerExW`，失败时使用旧计时器 | 这些调用不接收用户提供的 DLL 名或函数名；最终两架构 imports 不含新 DPI 函数的静态导入；旧 OS 是否完整可运行须实测 |
| 崩溃 `dbghelp.dll` | 安装过滤器时预计算系统目录绝对路径；实际未处理异常时才 `LoadLibraryW` 并解析 `MiniDumpWriteDump` | 正常启动不加载此库、不创建 dump；不保证进程内崩溃转储一定成功 |
| WIC / 保存对话框 | `CLSID_WICImagingFactory1`、`CLSID_FileSaveDialog` 通过 `CoCreateInstance`，代码指定 `CLSCTX_INPROC_SERVER` | 即使最终 imports 中没有 `windowscodecs.dll`，也不能据此说不依赖 WIC；系统 COM/编解码器运行时依赖未穷举 |
| GDI+ | 编辑渲染器按编辑会话 `GdiplusStartup` / `GdiplusShutdown`；`gdiplus.dll` 是静态系统导入 | “按会话初始化”不等于该 DLL 没有在进程装载时映射 |
| `LoadLibraryExW` 静态导入 | 两个 exe 都有此符号；检查到的产品源码没有直接调用 | 本轮没有通过反汇编归因该符号的链接调用者；不能仅凭该导入宣布存在任意路径加载，也不能用源码搜索排除链接代码的所有动态依赖 |

显式 DLL 目标路径已经核对，但本轮没有审计所有系统 DLL 的传递依赖解析、COM 注册、Shell 扩展或运行环境的完整性，也没有做 DLL 注入/劫持渗透测试。

## 文件与注册表副作用

| 触发 | 位置及操作 | 证据/限制 |
|---|---|---|
| 首次配置 | 默认 exe 同目录 `VisPick.ini`，`CREATE_NEW` 仅首次生成；默认位置失败可回退 `%APPDATA%\\VisPick\\VisPick.ini` 并提示。显式 `--config` 使用指定位置，创建/读取失败不另选目录 | [Paths.cpp](../../src/core/Paths.cpp)、[App.cpp](../../src/App.cpp)。已有配置只读取，不自动覆盖；配置模板嵌入资源 |
| 配置监视 | 打开选定配置的父目录用于异步 `ReadDirectoryChangesW`；收到通知后 UI 线程重载 | [ConfigWatcher.cpp](../../src/platform/ConfigWatcher.cpp)。这不是目录内容上传，也不做全盘扫描 |
| 截图保存 | 原生对话框取得文件系统路径并提示覆盖；编码成功后在目标目录创建唯一临时文件、写入并刷新，再替换目标。失败清理本次临时文件 | [SaveDialog.cpp](../../src/output/SaveDialog.cpp)、[ImageCodec.cpp](../../src/imaging/ImageCodec.cpp)。只在用户确认输出后执行；异常终止时不能保证所有临时文件消失 |
| GIF 录制 | `%TEMP%\\VisPick\\rec_vispick_<PID>_<tick>_<sequence>.gif`，先 `CREATE_NEW` 保留路径，再流式编码；取消/放弃清理其持有的临时文件 | [Recorder.cpp](../../src/recording/Recorder.cpp)、[GifPreview.cpp](../../src/recording/GifPreview.cpp)。GIF 本身包含屏幕内容；预览期间临时文件真实存在 |
| GIF 保存 | 用户选定目标；同卷移动替换，跨卷在目标目录写 `.VisPick-save-*.tmp`、刷新后替换；成功后删除源临时文件 | [RecordingFiles.cpp](../../src/recording/RecordingFiles.cpp)。删除源失败可能留待下次启动清理，不声称无残留 |
| 启动清理 | 仅枚举 `%TEMP%\\VisPick` 一层；匹配严格数字标识 `rec_vispick_<PID>_<tick>_<sequence>.gif` 结构，超过 24 小时才删除，排除目录和重解析点 | 同上 `CleanOldFiles`，调用见 `App::Run`。名称是归属约定，不是创建者身份认证；不递归删除任意临时目录 |
| 用户自启 | 仅 `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run` 的 `VisPick` 值；用户菜单开关写入/删除；启动只修复已存在且非空的该值 | [Autostart.cpp](../../src/platform/Autostart.cpp)、[Autostart.h](../../src/platform/Autostart.h)。不默认加入自启，不写 HKLM、不安装服务/任务 |
| 控制台 | 帮助/错误写现有控制台或用户重定向句柄，必要时打开 `CONOUT$` | [Console.cpp](../../src/platform/Console.cpp)。没有把帮助输出固定写入产品日志文件 |

图片输入来自用户指定贴图文件、剪贴板图片或剪贴板文件路径，通过 WIC 读取。截图/贴图像素在使用中保存在进程内存和按需缓存；本轮没有证明内存或磁盘数据经过安全擦除。系统分页、剪贴板历史、备份及文件系统删除后的恢复能力不在本次实现保证之内。

## 日志与崩溃转储

日志默认 `Config::General::log=false`。`App::ApplyConfiguration` 仅在该配置或显式 `--perf` 为真时启用；单实例转发的 `--perf` 也可以显式开启本进程性能日志。[Log.cpp](../../src/core/Log.cpp) 的 disabled 路径先返回，不创建文件、目录、工作线程或定时刷新；关闭后释放文件句柄。

启用时依次尝试 exe 目录、APPDATA/VisPick、TEMP/VisPick，追加 UTF-8 `VisPick.log`；回退路径显示提示。日志包含时间、命令、配置/保存路径、错误信息和性能计数，可能含个人路径或配置文本，不应描述为匿名数据。未发现把图像像素直接写入日志或上传日志的代码。当前实现没有日志轮转/总大小上限；这是显式诊断模式的保留行为，不影响默认关闭判断。

[main.cpp](../../src/main.cpp) 的主要实例在进入应用前默认安装崩溃过滤器；`--help`、`--version`、转发给已有实例等提前返回的路径不安装。安装只计算候选目录和系统 `dbghelp.dll` 路径，不在此时加载库或写文件。真实未处理异常后，按 exe/APPDATA/TEMP 顺序尝试创建唯一 `VisPick-crash-*.dmp`，使用 `MiniDumpNormal`，自定义流包含实际 dump 路径；失败的部分文件被尝试删除，结果路径/错误经对话框和调试输出报告。证据：[CrashDump.cpp](../../src/core/CrashDump.cpp)、[Paths.cpp](../../src/core/Paths.cpp)。

dump 是本地故障诊断文件，代码无自动上传。它可能含进程状态和敏感上下文，不能称为匿名或无敏感内容。该路径为进程内 best effort：已损坏堆栈、加载失败、磁盘/权限问题均可能使转储不可用。默认安装过滤器与“只有发生异常才产生 dump”应分别陈述。

## 便携性与开发工具剥离

CMake 将 CRT 设置为静态 `MultiThreaded`（Release `/MT`）；产品只链接 `vispick_core`。虽然 `src/core/Report.cpp` 位于非 spikes 目录，其 `results.json`/Markdown 输出属于独立 `vispick_spikes` 目标，未加入产品目标。`src/core/CommandLine.cpp` 也在开发目标中。测试框架、桌面验收、浸泡、质量素材和性能原型不随这两个产品 exe 链接。

两份实际 imports 没有 `VCRUNTIME`、`MSVCP`、`ucrtbase`、`api-ms-win-crt-*`；没有产品自行导入的网络库或 `dbghelp`。这支持“无外置 VC 运行库部署要求”的静态结论。系统 DLL/WIC/GDI+/COM/字体仍是运行环境依赖，静态导入检查不能证明全新系统、无平台更新的 Win7、所有字体/IME配置均可运行。最低 API 审查另见 [WIN7_IMPORTS.md](../WIN7_IMPORTS.md)。

本轮未执行 ETW/抓包、文件/注册表运行时追踪、Defender 扫描、干净系统启动、目标 OS 验收或传递依赖逆向。未发现需要据此立即大改产品的直接联网/默认日志问题；上述边界和按需生成的本地诊断/临时内容须在发布说明中准确保留。
