# VisPick 审查与优化建议

> 基线：提交 `d500e22`（0.5.11 源码）｜ 日期：2026-11-10 ｜ 性质：静态评审，未构建、未运行

## 1 范围与方法

- 阅读对象：`src/` 下除 `spikes/` 外约 1.1 万行产品代码，以及 [DESIGN.md](../DESIGN.md)、[KNOWN_ISSUES.md](../KNOWN_ISSUES.md)、构建脚本、CMake 预设和现有评审记录。
- 重点模块：`App`、`Overlay`、`Recorder`、`GifPreview`、`PinManager`、`Config`、`Clipboard`、`Window`、`Compat`、`SingleInstance`、`Tray`。
- 未做的事：没有构建，没有运行单元测试或桌面测试，也没有在 Win7、Win10 或物理显示器上验证。涉及 Windows 行为的判断来自代码和微软文档，落地前需实测。
- 编号规则：F 可靠性与反馈，T 工具链与平台，U 交互与使用习惯，A 架构与代码，E 工程与发布。优先级 P0 最高。

## 2 结论摘要

整体质量高，问题集中在三处：

1. **失败反馈被一并静默**。0.5.11 把“成功提示”和“失败提示”都关掉了。后台保存失败、录制失败、热键冲突因此对用户完全不可见（F-01）。
2. **工具链与平台策略不匹配**。锁定的 MSVC 14.42 所属通道已停止服务；Win7 和 x86 一直没有实机验证，却长期限制着采集、绘制和测试矩阵（T-01～T-03）。
3. **默认热键、选区操作和 GIF 编辑缺少一批常用能力**（U 系列），工程侧缺 CI 和版本标签（E 系列）。

## 3 值得保留的设计

改动时不要破坏下列做法：

- 全项目统一使用虚拟桌面物理像素坐标，DPI 只作用于界面元素。
- `Bitmap` 单一图像类型；`BitmapView` 明确只借用、不延长生命周期。
- RAII 管理 GDI/COM/线程；[Window::Dispatch](../../src/platform/Window.cpp#L99) 在窗口过程外层拦截异常。
- GIF 流水线：有界槽位、背压、只编码变化区域、延时误差向后传递、内存预算随位数变化。
- 配置热重载：缺失或无效项沿用上次有效值，不覆盖用户注释。
- 构建：导入表白名单、2 MiB 体积检查、Release PDB 按哈希归档。
- 文档如实区分“已验证”和“待验证”。

## 4 发现与建议

### 4.1 可靠性与反馈

| 编号 | 优先级 | 现状 | 建议 | 成本 |
|---|---|---|---|---|
| F-01 | P0 | [App::Report](../../src/App.cpp#L101) 只写日志，日志默认关闭。受影响路径：后台保存失败（[ImageSaved](../../src/App.cpp#L565)）、录制失败或没有帧（[RecordingFinished](../../src/App.cpp#L333)）、热键注册失败（[RebindHotkeys](../../src/App.cpp#L210)）、剪贴板没有图片、重复启动。配置和热键问题仍可在“关于”中查看，但用户不会主动去看。 | 原则改为“成功静默，失败必须可见”。用户主动操作失败时用 `Tray::Notify(..., warning=true)`（已实现，目前只用于启动）。启动通知附带热键冲突摘要。重复启动时重放启动气泡，因为 Win11 常把托盘图标收进溢出区。可选增加 `[General] Notify = errors｜all｜none`，默认 `errors`。 | 小 |
| F-02 | P1 | 浮层窗口过程抛异常时，只记录计数并转交 `DefWindowProc`（[Window::Dispatch](../../src/platform/Window.cpp#L99)、[WindowFailure](../../src/App.cpp#L590)），浮层本身保持显示。 | 对全屏置顶的浮层，回调异常时自动取消会话，避免用户被困在停止重绘的冻结画面后面。 | 小 |
| F-03 | P2 | [CopyImage](../../src/imaging/Clipboard.cpp#L267) 在 UI 线程同步编码 PNG，由 [ProcessSelection](../../src/App.cpp#L308) 调用，大选区会短暂卡住。 | 改用剪贴板延迟渲染（`WM_RENDERFORMAT`），或先发布 DIB、PNG 在后台编码后补发。 | 中 |

验收（F-01）：模拟目标目录只读保存、热键被占用启动、录制中磁盘写满，三种情况都应出现可见提示；正常复制和保存仍无提示。

### 4.2 工具链与平台

| 编号 | 优先级 | 现状 | 建议 | 成本 |
|---|---|---|---|---|
| T-01 | P0 | [CMakeLists.txt](../../CMakeLists.txt#L8) 与 [Enter-VsEnvironment.ps1](../../scripts/Enter-VsEnvironment.ps1#L8) 精确锁定 MSVC 14.42（VS 2022 17.12 LTSC）。该通道已于 2026 年 7 月中旬结束支持，不再有编译器修复。Win7 目标真正的断点在 VS 2026（MSVC 14.50）：STL 起最低支持 Win10。VS 2022 17.14（MSVC 14.44）仍可面向 Win7，服务期到 2032 年 1 月。 | 把版本检查放宽为 `1942 ≤ MSVC_VERSION < 1950`，或改锁 14.44。Win7 兼容仍交给 [Check-Imports.ps1](../../scripts/Check-Imports.ps1) 和实机验证把关。同时修正 [DESIGN.md](../DESIGN.md) §2.7 中“14.3x–14.4x”与实际精确锁定不一致的描述。 | 小 |
| T-02 | P0 | Win7 与 x86 从未实机运行，但长期带来约束：只能 BitBlt 截屏、只能 GDI/GDI+ 绘制、工具链有上限、测试矩阵翻倍、代码中有大量 32 位内存预算分支。 | 先做决策，再按结论执行。①保留 Win7：在 Win7 SP1 与 Win10 x86 虚拟机各完整跑一次 E2E，补齐 [KNOWN_ISSUES.md](../KNOWN_ISSUES.md) 的“待验证”项。②提高到 Win10 以上：发布 x64 和 ARM64，去掉 x86，采集可用 WGC。③折中：见 T-03。 | 中 |
| T-03 | P1 | 采集只有 BitBlt；设计中规划的 DXGI 迟迟没有落地，原因是 RDP 的 32 Hz 会话无法验收。HDR 显示器偏色已记为已知风险。 | 抽出采集接口，按 WGC → DXGI Desktop Duplication → BitBlt 运行时降级。静态截图可继续用 BitBlt。HDR 需要色调映射，不是换 API 就自动正确。WGC 的边框和光标开关对系统版本有更高要求，落地前核对微软文档。 | 大 |
| T-04 | P1 | 大量验收项被“环境不合格”挡住：[DetectTestEnvironment](../../src/platform/Compat.cpp#L294) 把远程会话和低刷新率判为不可验收。 | 在一台本机物理显示器会话上跑一次 `VisPickTest --all`，再在 Win7/Win10 虚拟机上跑一次。这两步能关闭 BitBlt/DXGI 决策和多数待验收项，比继续补文档划算。 | 小 |

### 4.3 交互与使用习惯

| 编号 | 优先级 | 现状 | 建议 | 成本 |
|---|---|---|---|---|
| U-01 | P1 | 默认全局热键 Ctrl+1/Ctrl+2 会吞掉常见软件的同名快捷键：浏览器切换标签页、Excel 打开设置单元格格式、Word 设置单倍行距、VS Code 切换编辑组（以各软件当前行为为准，落地前核对）。 | 首次启动时让用户选择热键组合，或默认改用不易冲突的组合（如 Ctrl+Alt+A、PrintScreen）。使用 PrintScreen 时需提示关闭 Win11 的“使用 Print Screen 键打开屏幕截图”。 | 小 |
| U-02 | P1 | 托盘菜单（[Tray.cpp](../../src/platform/Tray.cpp)）不显示快捷键，用户难以发现。 | 菜单项带上当前热键，例如 `截图\tCtrl+1`，热键重绑后同步刷新。 | 小 |
| U-03 | P1 | 选区阶段只识别顶层窗口，缺少键盘选区手段。 | 增加 Ctrl+A（全屏或当前显示器）、Tab 在候选窗口间切换、Ctrl+方向键调整大小；评估识别子窗口和控件。 | 小–中 |
| U-04 | P1 | 放大镜显示 HEX/RGB，但不能复制。 | 按 C 复制颜色值，Shift+C 复制 RGB 形式。成本很低，也贴合产品名。 | 小 |
| U-05 | P1 | 放大镜取样图固定为 120×120 物理像素（[Overlay.cpp](../../src/overlay/Overlay.cpp#L485)），外框却随 DPI 缩放，175% 下取样图偏小。 | 取样图边长按 DPI 缩放，格数不变。 | 小 |
| U-06 | P2 | 标注只有六种，画完后不能再选中、移动或删除；马赛克块较小时可能被还原。 | 补序号步骤、荧光笔、实心遮挡和自定义颜色；实现“选中、移动、删除已有标注”。数据模型已是屏幕坐标矢量对象，扩展成本可控。敏感信息建议优先用实心块。 | 中 |
| U-07 | P1 | `Recorder::Pause` 已实现（[Recorder.h](../../src/recording/Recorder.h)），但录制条没有暂停按钮，只有锁屏时内部使用。预览不能裁剪。 | 录制条加暂停/继续。预览加“裁掉开头和结尾”（最常见用途是去掉点停止键的画面），可复用现有时间轴。中长期评估 MP4 或 WebP 输出。 | 小–中 |
| U-08 | P3 | 退出后参考图和回收站全部丢失；设计将其列为 v1 不做。 | 退出时可选保存到用户目录，下次一键恢复。内存已有无损存储形式，主要工作是落盘格式和清理策略。 | 中 |

### 4.4 架构与代码

| 编号 | 优先级 | 现状 | 建议 | 成本 |
|---|---|---|---|---|
| A-01 | P2 | `ImageSaver`、`GifPreview`、`Recorder`、`ConfigWatcher` 各自手写线程、锁、`WM_APP` 消息和 `TakeResult`/`Busy`；消息号由各模块手工分配（`WM_APP+1` 在 App 与 GifPreview 中含义不同，靠窗口隔离才不冲突）。 | 提供一个 UI 线程调度器（一个注册消息加一个闭包队列）和一个后台任务辅助类，统一完成通知、取消和异常传递。 | 中 |
| A-02 | P2 | [App](../../src/App.h) 的状态靠 `recorder_`、`preview_`、`overlay_->Active()`、`stopping_`、`modalDepth_`、`pinsPending_`、`exitAfterRecording_` 组合推断；嵌套模态循环下的重入最脆弱。 | 改为显式状态枚举加统一的交互入口，状态转换即可写成单元测试。 | 中 |
| A-03 | P2 | [Overlay::State](../../src/overlay/Overlay.cpp#L118) 约 700 行，混合布局、命中测试、绘制、输入状态机和焦点处理，只能靠桌面 E2E 覆盖。 | 抽出不依赖 HWND 的工具条布局与输入状态机，用 doctest 直接测。 | 中 |
| A-04 | P2 | 一个配置键要同时改 `Config` 结构、[ParseConfig](../../src/core/Config.cpp#L52)、[default.ini](../../res/default.ini)、README、DESIGN 五处。[SavePinAcrossDesktops](../../src/core/Config.cpp#L223) 用约 115 行只改一个键，且打破了“程序只读配置”的设计原则。`capture_layered_gif` 的命名风格与其他键不一致。 | 用一张描述表驱动解析、诊断信息和默认模板。把 `SavePinAcrossDesktops` 泛化为 `UpdateIniValue(section, key, value)`。新键沿用 PascalCase，旧名保留兼容。 | 中 |
| A-05 | P3 | 中英文字符串对散落在约 6 个产品文件中；语言判断在 [App](../../src/App.cpp) 与 [Overlay](../../src/overlay/Overlay.cpp) 各算一次；配置诊断和 ini 注释只有中文；各窗口字体选择逻辑重复。 | 集中为字符串表和一次性的字体/语言解析。 | 中 |
| A-06 | P3 | 验收环境判定（`DetectTestEnvironment` 等）位于产品核心库 `vispick_core`。 | 移到测试或 spikes 支持库，缩小产品 exe 的体积和攻击面。 | 小 |
| A-07 | P2 | 风格不一致：[Recorder.cpp](../../src/recording/Recorder.cpp) 有 92 行、[GifPreview.cpp](../../src/recording/GifPreview.cpp) 有 126 行逗号后不留空格，其余文件为 0；超过 120 字符的行约 360 行（约占 3%），[Config.cpp](../../src/core/Config.cpp) 最集中。 | 加 `.clang-format` 并在 CI 检查；一次性格式化提交，之后不再混入功能改动。 | 小 |
| A-08 | P3 | [ToolbarIcon.cpp](../../src/platform/ToolbarIcon.cpp) 用 GDI 直线绘制图标，没有抗锯齿；程序已链接 GDI+。 | 图标改用 GDI+，高 DPI 下边缘更平滑。 | 小 |

### 4.5 工程与发布

| 编号 | 优先级 | 现状 | 建议 | 成本 |
|---|---|---|---|---|
| E-01 | P1 | 仓库没有 CI。 | 用 GitHub Actions 跑 x64/x86 构建、单元测试、导入检查和体积检查；打 tag 时自动发布 exe 与 SHA256。托管机自带的是较新的 MSVC，是放宽 T-01 的另一个理由。 | 中 |
| E-02 | P2 | `docs/` 共 13 MB、约 200 个文件，其中 9 MB 的宣传视频和大量日志/JSON 报告已进入 git 历史。 | 媒体文件移到 Release 附件或 LFS；原始验收数据作为 CI 产物保存，仓库只留摘要。 | 小 |
| E-03 | P1 | CHANGELOG 已到 0.5.11，只有 `v0.5.3` 打了标签；README 同时描述已发布版本与未发布行为。 | 每个版本打标签。README 拆成用户说明和开发文档两部分，用户部分只写已发布行为。 | 小 |
| E-04 | P2 | exe 未签名，下载后会触发 SmartScreen。 | 评估 SignPath 基金会（面向开源项目）或微软的托管签名服务；便携工具适合上架 Scoop 和 winget。 | 中 |

## 5 路线图

| 阶段 | 事项 | 目标 |
|---|---|---|
| 第一批（P0） | F-01、T-01、T-02 的决策与首轮实机验证 | 失败可见；工具链不再停留在已停服通道；平台边界有结论 |
| 第二批（P1） | E-01、E-03、A-07、T-04、U-01～U-05、U-07、F-02 | 流程可复现；常用操作补齐 |
| 第三批（P2） | A-01～A-04、F-03、U-06、E-02、E-04 | 降低重入与维护风险；扩展标注 |
| 后续（P3） | T-03、U-08、A-05、A-06、A-08 | 采集升级、参考图持久化、收尾整理 |

## 6 局限与待核实

- 本文是静态评审，F-02、F-03 对卡顿程度和异常路径的判断需要实际复现。
- U-01 中各软件的快捷键以其当前版本为准。
- T-03 的最低系统版本、HDR 色调映射和边框/光标选项需要对照微软文档确认。
- 微软生命周期与平台支持来源：
  - [Visual Studio 2022 生命周期](https://learn.microsoft.com/lifecycle/products/visual-studio-2022)
  - [Supported platforms (Microsoft C++)](https://learn.microsoft.com/cpp/overview/supported-platforms-visual-cpp)
  - [microsoft/STL 变更记录](https://github.com/microsoft/STL/wiki/Changelog)
