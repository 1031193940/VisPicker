# Win7 静态导入基线

`scripts/Win7-Imports.txt` 是经审查的 `DLL!导出名` 白名单。`build.ps1` 对 x64/x86 最终 **产品 `VisPick.exe`** 执行 `dumpbin /imports`，遇到不在白名单中的实际导入即失败。白名单并非从开发机的系统 DLL 导出表全量生成；不能因为一个函数在 Win11 存在，就认定它在 Win7 存在。

0.5.1-candidate 的 x64 和 x86 分别通过 333、324 个普通和延迟导入。DLL 集合仍为 11 项。DWM、GDI+、WinMM 和 WTS 改从系统目录延迟加载，检查脚本也会拒绝这些库出现在普通导入表。见 [x64 导入](reports/data/review-0.5.1/imports-x64.txt)、[x86 导入](reports/data/review-0.5.1/imports-x86.txt) 和 [本轮修正](reviews/P1-P6-corrections-2026-11-06.md)。

首批基线来自 MSVC **14.42.34433** 编译的 VisPick P0 和单元测试程序的实际导入，按 Windows 7 SP1 目标审查。覆盖 Win32 窗口/GDI、文件/进程/同步、Shell、COM、DWM、WinMM，以及静态 CRT 引入的系统 API。x86 的 `GetWindowLongW` / `SetWindowLongW` 与 x64 的 `GetWindowLongPtrW` / `SetWindowLongPtrW` 分别列出。

## 产品与开发工具的范围

P0 评审修复已把 `vispick_spikes` 从产品链接关系中移除。`VisPickSpikes.exe`、`VisPickTest.exe`、单元测试及后续各阶段集成测试、浸泡工具都是开发工具，不随产品交付。`vispick_core` 是静态库；其链接依赖列表包含某个系统库，并不代表最终产品一定导入该 DLL，仍以最终 PE 的导入表为准。

2026-11-06 最终 P1–P6 Release 候选（已包含预览 DPI 与隐藏启动修复）直接从 `dist` 重新核对：

| 产品 | 字节数 | 命名静态导入数 | 直接导入 DLL 数 | 原始证据 |
|---|---:|---:|---:|---|
| `dist/VisPick.exe`（x64） | 778240 | **322** | **11** | [x64 imports](../out/reports/p6-code-audit/VisPick.exe.imports.txt) |
| `dist/VisPick-x86.exe` | 623104 | **313** | **11** | [x86 imports](../out/reports/p6-code-audit/VisPick-x86.exe.imports.txt) |

两者 DLL 集合相同：`KERNEL32.dll`、`USER32.dll`、`GDI32.dll`、`SHELL32.dll`、`ole32.dll`、`dwmapi.dll`、`WINMM.dll`、`ADVAPI32.dll`、`WTSAPI32.dll`、`gdiplus.dll`、`IMM32.dll`。完整 SHA-256、生成工具及范围见 [P6 产品代码审计](reports/P6-code-audit.md)。这些是实际构建快照，不是未来版本的固定数量要求；WIC、保存对话框等 COM 运行时依赖不能只从直接 DLL 列表推断。

历史记录：P0 拆分产品与开发工具后的骨架曾为 x64 **75** / x86 **68** 个导入、两者 **3 DLL**（Kernel32/User32/Shell32）；更早整合原型曾为 **7 DLL / 164 符号**。这些数字只描述当时的二进制，不再作为当前完整产品依赖说明。

P0 评审时，下节审查了开发工具新增 API，并将确认兼容 Win7 的 48 个命名导出加入白名单。白名单表示“允许使用”，不表示“当前产品实际依赖”。后续新增功能按下面的增量证据逐项审查；自动构建的发行门槛仍针对当前产品实际 imports。单元测试、桌面 E2E 的通过情况与静态导入检查是不同证据。

## P1–P6 增量审查证据

- **P1 常驻框架**：[白名单](../scripts/Win7-Imports.txt) 的 `P1 resident framework additions` 段逐条保留 Microsoft Learn 最低客户端和 URL，覆盖热键/托盘、注册表、配置目录通知、文件和同步等实际新增导入。
- **P2/P3 截图与编辑**：[API 原始审查记录](reports/data/p23-api-review.json) 保存符号、最低客户端及官方 SDK 文档来源；已核准静态符号在白名单对应段。GDI+、IME、WIC/COM 的可用性不能等同于所有字体、输入法和系统组件配置已实测。
- **P4/P5 贴图与录制**：[API 原始审查记录](reports/data/p45-api-review.json) 与白名单 `P4/P5 Win32 imports` 段，包含绘制、文件提交、计时器、剪贴板、会话通知等实际增量。
- **动态路径**：`Compat` 动态探测 DPI/窗口捕获接口；可选 `shcore.dll` 与崩溃时的 `dbghelp.dll` 使用系统目录绝对路径。新 API 或新标志在旧系统不可用时需保留回退。动态 API 不因为已有运行成功记录就自动加入 Win7 静态白名单；具体路径与未测边界见 [P6 审计](reports/P6-code-audit.md)。

## P0 评审新增 API 审查（历史基线）

以下最低客户端来自微软 API 文档的 Requirements，以及其 [MicrosoftDocs/sdk-api 官方源文件](https://github.com/MicrosoftDocs/sdk-api) 中的 `req.target-min-winverclnt` 元数据。表中 XP/2000 表示当前文档列出的最低客户端；不推断更早版本是否实际上也有该 API。

| 新增用途 / DLL | 新增 API 与官方来源 | 最低客户端及本轮限制 |
|---|---|---|
| 显示配置 / User32 | [QueryDisplayConfig](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-querydisplayconfig) | Win7；只使用 `QDC_ONLY_ACTIVE_PATHS`，不传 Win10/11 的虚拟模式/刷新率标志 |
| 显示配置 / User32 | [GetDisplayConfigBufferSizes](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdisplayconfigbuffersizes)、[DisplayConfigGetDeviceInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-displayconfiggetdeviceinfo) | 当前函数页列 Vista；本项目与上述 Win7 查询 API 一起使用，并采用 Win7 已有的 source-name 请求 |
| 设备与窗口 / User32 | [EnumDisplayDevicesW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaydevicesw)、[EnumDisplaySettingsW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaysettingsw)、[GetClassInfoExW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getclassinfoexw) | Windows 2000 |
| 会话注册表 / Advapi32 | [RegGetValueW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-reggetvaluew) | Vista；XP Professional x64 也列为支持；仅只读 DWORD 查询 |
| 会话 ID / Kernel32 | [ProcessIdToSessionId](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-processidtosessionid) | 当前文档列 Vista |
| 会话协议 / Wtsapi32 | [WTSQuerySessionInformationW](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsquerysessioninformationw)、[WTSFreeMemory](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsfreememory) | 当前文档列 Vista；只查询当前进程会话的 `WTSClientProtocolType` |
| 关闭动画 / Dwmapi | [DwmSetWindowAttribute](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmsetwindowattribute) | Vista；仅使用已有的 `DWMWA_TRANSITIONS_FORCEDISABLED`，允许桌面合成关闭时失败 |
| 内存统计 / Psapi | [GetProcessMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo) | XP；显式 `PSAPI_VERSION=1` 对应 `Psapi.dll!GetProcessMemoryInfo`，不与 `K32GetProcessMemoryInfo` 混淆 |
| 子进程继承 / Kernel32 | [InitializeProcThreadAttributeList](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-initializeprocthreadattributelist)、[UpdateProcThreadAttribute](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute)、[DeleteProcThreadAttributeList](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-deleteprocthreadattributelist) | Vista；使用 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`，不使用 Win8+ 安全能力或 Win10+ Job-list 属性 |
| 进程 / Kernel32 | [CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)、[OpenProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openprocess)、[GetExitCodeProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getexitcodeprocess) | XP；独立图样进程与测试子进程 |
| 线程 / Kernel32 | [CreateThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createthread)、[ExitThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitthread)、[GetExitCodeThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getexitcodethread)、[FreeLibraryAndExitThread](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-freelibraryandexitthread) | XP；包括静态 CRT 的线程实现导入 |
| 共享内存 / Kernel32 | [CreateFileMappingW](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-createfilemappingw)、[MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile)、[UnmapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-unmapviewoffile) | XP；匿名共享段，无新式映射 API |
| 同步 / Kernel32 | [CreateEventW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw)、[SetEvent](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setevent)、[WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)、[WaitForSingleObjectEx](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobjectex)、[WaitForMultipleObjects](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects) | XP |
| 条件变量 / Kernel32 | [WakeConditionVariable](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-wakeconditionvariable) | Vista |
| 定时 / Kernel32 | [CreateWaitableTimerW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerw)、[SetWaitableTimer](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimer) | XP；旧系统的普通可等待定时器路径 |
| 消息等待 / User32 | [MsgWaitForMultipleObjectsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex)、[WaitForInputIdle](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-waitforinputidle) | XP |
| 控制台测试 / Kernel32 | [FillConsoleOutputCharacterW](https://learn.microsoft.com/en-us/windows/console/fillconsoleoutputcharacter)、[ReadConsoleOutputCharacterW](https://learn.microsoft.com/en-us/windows/console/readconsoleoutputcharacter)、[SetConsoleCursorPosition](https://learn.microsoft.com/en-us/windows/console/setconsolecursorposition) | Windows 2000；只操作本工具创建的隐藏控制台 |
| 重定向 / Kernel32 | [CreatePipe](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-createpipe)、[SetHandleInformation](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-sethandleinformation) | Windows 2000 |
| 路径 / Kernel32 | [GetFullPathNameW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfullpathnamew) | XP |
| 测试超时 / Kernel32 | [GetTickCount64](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-gettickcount64) | Vista |
| 窗口异常诊断 / Kernel32 | [OutputDebugStringW](https://learn.microsoft.com/en-us/windows/win32/api/debugapi/nf-debugapi-outputdebugstringw) | XP；异常回调的调试诊断输出 |
| 测试窗口查找 / User32 | [EnumWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumwindows)、[EnumChildWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumchildwindows)、[GetWindowThreadProcessId](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowthreadprocessid) | Windows 2000；操作前核对窗口所属测试子进程 PID |
| 测试消息 / User32 | [PostMessageW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-postmessagew)、[SendMessageTimeoutW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw) | Windows 2000 |

高精度计时器仍是可选路径：[CreateWaitableTimerExW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw) 函数本身最低为 Vista，但 `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` 标志仅支持 Win10 1803+。当前代码动态取得函数，创建失败则退回 `CreateWaitableTimerW` 与 `timeBeginPeriod(1)`；不能因函数可用就认为高精度标志可用。本轮没有把这个动态符号自动添加为静态导入许可。

显示检测 API 在 Win7 可加载，不代表每个会话都能查询成功。微软文档明确描述了非 WDDM、远程会话或无当前桌面访问权限时的失败；当前检测将无法确认的输出视为验收环境未知，保留诊断信息。该保守判定不应被写成“这些机器不能运行 VisPick”。

## 容易误判的边界 API

| API | 微软文档标注的最低客户端 | 处理 |
|---|---|---|
| [TryAcquireSRWLockExclusive](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-tryacquiresrwlockexclusive) | Windows 7 | 可静态导入 |
| [SleepConditionVariableSRW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleepconditionvariablesrw) | Windows Vista | 可静态导入 |
| [InitializeCriticalSectionEx](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-initializecriticalsectionex) | Windows Vista | 可静态导入 |
| [FlsAlloc](https://learn.microsoft.com/en-us/windows/win32/api/fibersapi/nf-fibersapi-flsalloc) | Windows Vista | CRT 的 FLS 路径可用 |
| [SetThreadStackGuarantee](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadstackguarantee) | Vista；XP Professional x64 | 可静态导入 |
| [GetFileInformationByHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex) / [SetFileInformationByHandle](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileinformationbyhandle) | Windows Vista | 可静态导入；信息类参数仍有版本差异 |
| [GetLocaleInfoEx](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getlocaleinfoex) / [LCMapStringEx](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-lcmapstringex) | Windows Vista | CRT 的区域设置路径可用 |
| [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext) | Windows XP | 可静态导入 |
| [DwmFlush](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmflush) | Windows Vista | 可静态导入；关闭桌面合成时需处理失败 |
| [SetProcessDPIAware](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setprocessdpiaware) | Windows Vista | 旧系统回退 |
| [SetProcessDpiAwarenessContext](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setprocessdpiawarenesscontext) | Windows 10 | 仅在 `Compat` 中动态探测；不在白名单 |
| [AdjustWindowRectExForDpi](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-adjustwindowrectexfordpi) | Windows 10 1607 | 最终预览 DPI 修复从已加载 User32 动态解析；旧系统调用 `AdjustWindowRectEx` 并按系统 DPI 缩放边框。两架构最终 imports 均不静态导入此新 API；不加入 Win7 静态白名单 |
| [SetWindowDisplayAffinity](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity) 的 `WDA_EXCLUDEFROMCAPTURE` | 函数为 Win7；该标志为 Win10 2004 | 必须检查系统版本；旧系统传该标志会得到黑块语义 |

每次新增导入，应先核对微软 API 文档的 Requirements，再审查调用使用的标志、信息类、COM 类和结构版本，最后手动更新白名单。不要把“把 dumpbin 输出追加到白名单”加入自动构建流程。

## 检查的范围

静态导入检查能阻止新增的未知 API，以及 VCRUNTIME/MSVCP/UCRT/API-set 等外部运行库依赖混入交付 exe。`_WIN32_WINNT=0x0601`、静态 CRT、固定工具集和导入审查共同降低旧系统加载失败的风险。

它不能证明程序已经在 Win7 运行成功：动态加载路径、API 参数的新旧语义、COM 注册情况、驱动/桌面合成、系统更新状态都不由 PE 导入表表达。Win7 SP1 x86/x64 实机或虚拟机运行仍属于待完成验收，不能用本检查替代。

ASan Debug 构建仅用于开发机检测内存错误，其运行时依赖不属于便携版发行依赖检查范围。
