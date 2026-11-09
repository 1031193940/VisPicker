# VisPick

面向 Windows 的便携截图、参考图和 GIF 工具。

已发布的 Windows 11 x64 便携版为 0.5.3，可下载 `VisPick-0.5.3.zip`。 [GitHub 0.5.3 发布页](https://github.com/1031193940/VisPicker/releases/tag/v0.5.3)

功能回归在 Windows 11 x64 的单屏 RDP 会话完成。Win10、Win7、干净系统、物理多屏和第三方软件粘贴等项目仍待验证。

## 使用

以下流程对应当前源码的本地 0.5.5 构建。

双击适合系统架构的 exe，程序常驻托盘。首次启动生成带中文注释的 `VisPick.ini`；exe 所在目录不可写时使用 `%APPDATA%\VisPick\VisPick.ini`。编辑配置并保存后自动生效，不需要重启，也不会覆盖你的注释。

热重载时，删除某个配置键会沿用本次运行中的有效值。恢复单项默认值可以将模板中的默认值填回该键。

| 操作 | 快捷键与流程 |
|---|---|
| 截图 | Ctrl+1 打开截图。拖出选区或单击窗口，按需调整和标注 |
| 复制截图 | 选区工具条点「✓ 复制」，也可按 Enter 或双击选区 |
| 保存截图 | 选区工具条点「保存」，选择文件位置与格式 |
| 参考图 | 选区工具条点「贴图」，把截图和标注钉在原位置 |
| GIF | Ctrl+2 → 选区 → 在工具条选帧率，点「开始录制」或按 Enter。再次按 Ctrl+2 或点「停止」打开预览 |
| GIF 预览 | 点击或拖动进度条跳转，空格暂停或继续，Ctrl+S 保存 |
| 调整选区 | 拖动内部、8 个手柄；方向键移动 1 px，Shift+方向键 10 px |
| 标注 | R 矩形、E 椭圆、A 箭头、P 画笔、T 文字、M 马赛克；再次点击工具取消选择 |
| 编辑输出 | Ctrl+Z / Ctrl+Y 撤销重做。Ctrl+C 复制，Ctrl+S 保存，Ctrl+T 贴图 |
| 取消 | Esc 取消选区。文字输入时 Esc 结束文字输入。录制请点工具条的取消按钮 |

截图保存支持 PNG/JPG/BMP。PNG 与剪贴板 PNG 保留像素和透明度，DIB 提供不透明兼容副本。文件编码在后台进行，失败不会截断已有目标文件。

GIF 帧率选择仅用于本次录制。可通过 `[Gif] Fps` 设置默认帧率。录制满五分钟自动停止，旧配置中的 `[Gif] MaxSeconds` 会被忽略。临时 GIF 位于 `%TEMP%\VisPick`，保存或确认放弃后清理，启动时清理一天前本程序遗留的录制。

录制不占用全局 Esc。录制达到 3 秒后，点取消会先停止并打开预览，可以保存或确认放弃。旧系统在全屏录制时可能隐藏工具条，此时按 GIF 热键停止。`[Advanced] capture_layered_gif=true` 可启用分层窗口采集，默认关闭。

参考图操作：

| 操作 | 效果 |
|---|---|
| 拖动 / 方向键 / Shift+方向键 | 移动 / 1 px / 50 px；靠近屏幕或其他参考图吸附 |
| 双击 | 在光标处收缩为方块，再次双击恢复；收缩时 Esc / Enter 恢复 |
| 滚轮 / Shift+滚轮 | 以光标为中心缩放；默认 10% / 1% |
| Ctrl+滚轮 | 调整透明度 |
| Alt+上下 / Alt+Shift+上下 | 缩放 ±10% / ±1% |
| Alt+左右 / Alt+Shift+左右 | 透明度 ±10% / ±1% |
| 5–9 / 0 | 缩放到 50%–90% / 100% |
| R / Alt+H / Alt+V | 顺时针 90° / 水平翻转 / 垂直翻转 |
| Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+S | 复制 / 剪切 / 剪贴板新贴图 / 保存 |
| Esc 或 Q / Ctrl+Z | 关闭入回收站 / 恢复最近关闭；默认保留 5 张 |

右键参考图可设置缩放、透明度、置顶和鼠标穿透。托盘菜单可隐藏/显示全部、关闭全部、恢复最近关闭，并解除所有参考图的鼠标穿透。参考图与回收站仅保留在本次进程中。

默认全局热键为 Ctrl+1 截图和 Ctrl+2 GIF。可在 `[Hotkeys]` 中修改热键或留空禁用。升级前先退出托盘中的旧进程，再启动新版 exe。新版会忽略旧配置 `[Hotkeys]` 段的 `Pin` 项，因此原来的 Ctrl+3 绑定会释放。托盘的「截取参考图」和 `--pin` 仍可直接打开贴图选区，该入口支持 `Pin.Instant=true` 松手即贴。

注册失败时托盘会提示对应热键，其余热键照常工作。可右键托盘打开「关于」，查看未注册的按键和失败原因。被其他程序占用的热键会自动重试，对方释放后即可使用。

默认关闭日志和自启。自启仅修改当前用户的 VisPick Run 项。完整配置模板见 [default.ini](res/default.ini)。

```powershell
VisPick.exe --screenshot
VisPick.exe --pin-file "D:\Pictures\参考.png"
VisPick.exe --pin-clipboard
VisPick.exe --toggle-pins
VisPick.exe --reload
VisPick.exe --exit
# 指定已有或待创建的配置文件；已运行实例不会悄悄切换配置
VisPick.exe --config "D:\Tools\VisPick.ini"
```

同一用户会话只运行一个实例，重复启动转发命令。`--perf` 本次运行启用诊断日志；`--help` / `--version` 单独使用，不启动常驻实例。应用无网络上传或联网依赖。

## 构建

开发环境：Windows、Visual Studio 2022 的 C++ 桌面开发组件、MSVC **14.42**、Windows SDK、VS 自带 CMake 3.29 / Ninja。构建脚本自动查找工具，不需要手工改 PATH。

在仓库根目录的 PowerShell 中运行。

```powershell
.\build.ps1 -Analyze
```

同一命令完成 x64/x86 Release 构建、单元测试、静态导入白名单检查和 2 MiB 大小检查，产出：

- `dist/VisPick.exe`：x64。
- `dist/VisPick-x86.exe`：x86。

Release 静态链接 CRT，只导入系统 DLL。Win7 SP1 是目标兼容版本；当前**尚未进行 Win7 实机或纯净系统运行验证**，导入检查不能替代它。具体边界见 [Win7 导入审查](docs/WIN7_IMPORTS.md)。

```powershell
# 只构建一个架构
.\build.ps1 -Architecture x64 -Analyze

# x64 Debug 带 AddressSanitizer；x86 Debug 保留普通调试检查
.\build.ps1 -Architecture x64 -Configuration Debug -Analyze
.\build.ps1 -Architecture x86 -Configuration Debug -Analyze
```

Debug/ASan 是开发产物，依赖工具链中的 ASan DLL，不复制到 `dist/`。在新终端运行 Debug 工具前先加载开发环境：

```powershell
. .\scripts\Enter-VsEnvironment.ps1 -Architecture x64
.\out\build\x64-Debug\VisPickTest.exe --desktop-tests --output .\out\reports\x64-Debug
```

## 运行原型与验收

`--help` / `--version` 支持控制台、UTF-8 文件和管道输出。以下开发工具位于构建目录，不复制到 `dist/`：

```powershell
.\out\build\x64-Release\VisPickSpikes.exe --spike-capture --output .\out\capture
.\out\build\x64-Release\VisPickSpikes.exe --spike-overlay --iterations 1000 --output .\out\overlay
.\out\build\x64-Release\VisPickSpikes.exe --spike-pin --output .\out\pin
# 串行正确性检查，不用于判定 30 fps 丢帧
.\out\build\x64-Release\VisPickSpikes.exe --spike-gif --seconds 10 --output .\out\gif
# 独立图样进程 + 采集线程 + 有界队列 + 编码线程
.\out\build\x64-Release\VisPickSpikes.exe --spike-gif-pipeline --seconds 10 --output .\out\pipeline
# 与修正前代码对照的量化吞吐/体积测试
.\out\build\x64-Release\VisPickSpikes.exe --spike-quantizer --output .\out\quantizer
```

量化实验默认读取仓库中的 NASA 照片；真实连续视频帧和素材来源见 [质量素材说明](docs/reports/data/quality-input/README.md)。指定目录可重复测试自己的 PNG/JPG/BMP：

```powershell
$env:VISPICK_QUALITY_INPUT = "$PWD\docs\reports\data\quality-input\video-bbb"
.\out\build\x64-Release\VisPickSpikes.exe --spike-quantizer --output .\out\video-quality
Remove-Item Env:\VISPICK_QUALITY_INPUT
```

输出包含量化时间、RGB 误差/PSNR、静止帧索引差异及供目测的 GIF；比较动画固定为 10 fps，原始视频帧时间另存于素材 CSV。

开发工具使用控制台子系统，可直接在 PowerShell 中等待退出。产品使用 GUI 子系统，需要等待时可用 `Start-Process -Wait -WindowStyle Hidden`。

```powershell
# 完整桌面测试与全部开发实验；会显示自动关闭的测试窗口
# 两个架构必须串行运行。采集含 1000 次小区域循环，远程会话下可能较慢。
.\out\build\x64-Release\VisPickTest.exe --all --output .\out\reports\x64-Release
.\out\build\x86-Release\VisPickTest.exe --all --output .\out\reports\x86-Release

# 仅测试物理像素一致性与 100 次资源生命周期
.\out\build\x64-Release\VisPickTest.exe --desktop-tests --output .\out\pixels
# 控制台、文件、管道、无控制台启动的帮助和错误输出
.\out\build\x64-Release\VisPickConsoleTests.exe
```

验收会暂时覆盖桌面，期间请不要遮挡测试窗口。无需模拟鼠标或键盘。报告输出到所选目录的 `results.json` / `results.md`；GIF 使用程序自绘且逐像素验证的已知图样，保存为 `gif-spike.gif` / `gif-pipeline.gif`。普通桌面抓屏仅保存在内存。未指定输出目录时使用 `%TEMP%\VisPick\spikes\` 下的独立目录。

开发工具退出码：`0` = 有效物理环境且已运行检查通过，`1` = 检查未通过或环境不具备验收资格，`2` = 参数或顶层运行错误。报告保留独立的 `checks_passed` 与 `acceptance_status`；远程、低于 50 Hz 或无法核实物理输出的会话统一为 `not_eligible`。性能阈值只在 Release 下解释，Debug/ASan 用于正确性与内存检查。产品参数错误返回 `2`，顶层异常返回 `3`。

当前会话确认为 32 Hz 远程显示，不能用于 AC0-4～AC0-7 的物理显示性能验收，也不能据此决定切换 DXGI。须在本地双 4K、175% 会话重测；时长正确不等同于无丢帧。原始报告与 JSON 保留，结论修订和逐项处理见 [评审修正记录](docs/reviews/P0-corrections-2026-11-06.md)。

## 当前进度

- [设计文档](docs/DESIGN.md)
- [P1–P6 评审修正记录](docs/reviews/P1-P6-corrections-2026-11-06.md)
- [开发记录](docs/DEVELOPMENT.md)
- [P1 常驻框架报告](docs/reports/P1-2026-11-06.md)
- [P2–P5 产品集成报告](docs/reports/P2-P5-2026-11-06.md)
- [P6 兼容与稳定性记录](docs/reports/P6-2026-11-06.md)
- [完整验收清单](docs/P6_CHECKLIST.md)
- [已知限制与待验收项目](docs/KNOWN_ISSUES.md)
- [P0 评审修正记录](docs/reviews/P0-corrections-2026-11-06.md)
- [P0 实测与验收报告](docs/reports/P0-2026-11-06.md)
- [P0 手工验收清单](docs/P0_CHECKLIST.md)

产品源码没有直接网络请求或上传逻辑；用户选择的网络路径及系统组件行为见 [代码审计](docs/reports/P6-code-audit.md)。只有测试使用 vendored doctest（MIT）；其来源与校验值见 [测试依赖说明](tests/third_party/README.md)。

产品集成回归与打包：

```powershell
.\tests\e2e\RunProductChecks.ps1 -Architecture x64 -FullRecording -IdleSeconds 600
.\tests\e2e\RunProductChecks.ps1 -Architecture x86 -FullRecording
.\scripts\Package.ps1
```

桌面测试须串行运行。集成回归只对自身窗口发消息，剪贴板检查使用独立窗口站。便携包包含两个 exe、使用说明和配置模板。

需要长时间资源诊断时，可选用 `.\tests\e2e\RunSoak.ps1 -Seconds 28800 -IntervalSeconds 30`。八小时浸泡已退出发布验收。 [诊断口径](docs/P6_CHECKLIST.md#浸泡测试的覆盖与口径)

## 许可证

VisPick 项目代码采用 MIT 许可证。 [LICENSE](LICENSE)

第三方依赖和测试素材沿用各自的许可证。 [测试依赖说明](tests/third_party/README.md) · [测试素材许可](docs/reports/data/quality-input/README.md)
