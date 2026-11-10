# 开发与验证

当前公开版本与源码版本为 **0.6.0**。新功能和本轮验证见 [审查实施记录](reviews/Implementation-Optimization-2026-11-10.md)，公开用户说明见 [README](../README.md)。下方历史记录中的旧目标不代表当前版本。

## 构建

开发环境：Windows、Visual Studio 2022 的 C++ 桌面开发组件、MSVC **14.42–14.49**（推荐仍受维护的 **14.44**）、Windows SDK、VS 自带 CMake 3.29 / Ninja。构建脚本自动查找工具，不需要手工改 PATH。

在仓库根目录的 PowerShell 中运行。

```powershell
.\build.ps1 -Analyze
```

同一命令完成 x64/x86 Release 构建、单元测试、静态导入白名单检查和 2 MiB 大小检查，产出：

- `dist/VisPick.exe`：x64。
- `dist/VisPick-x86.exe`：x86。

Release 静态链接 CRT，只导入系统 DLL。Win7 SP1 是目标兼容版本；当前**尚未进行 Win7 实机或纯净系统运行验证**，导入检查不能替代它。具体边界见 [Win7 导入审查](WIN7_IMPORTS.md)。

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

量化实验默认读取仓库中的 NASA 照片；真实连续视频帧和素材来源见 [质量素材说明](reports/data/quality-input/README.md)。指定目录可重复测试自己的 PNG/JPG/BMP：

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

当前会话确认为 32 Hz 远程显示，不能用于 AC0-4～AC0-7 的物理显示性能验收，也不能据此决定切换 DXGI。须在本地双 4K、175% 会话重测；时长正确不等同于无丢帧。原始报告与 JSON 保留，结论修订和逐项处理见 [评审修正记录](reviews/P0-corrections-2026-11-06.md)。

产品集成回归与本地归档：

```powershell
.\tests\e2e\RunProductChecks.ps1 -Architecture x64 -FullRecording -IdleSeconds 600
.\tests\e2e\RunProductChecks.ps1 -Architecture x86 -FullRecording
.\scripts\Package.ps1
```

桌面测试须串行运行。集成回归只对自身窗口发消息，剪贴板检查使用独立窗口站。`Package.ps1` 可选生成包含两个 exe、使用说明和配置模板的本地归档；该归档不上传 Release。公开 Release 仅提供 `VisPick.exe` 单文件，不附带配置或其它文件。

需要长时间资源诊断时，可选用 `.\tests\e2e\RunSoak.ps1 -Seconds 28800 -IntervalSeconds 30`。八小时浸泡已退出发布验收。 [诊断口径](P6_CHECKLIST.md#浸泡测试的覆盖与口径)

## 格式与 CI

项目使用 clang-format **18.1.8**，只格式化 `src/` 和 `tests/` 的自有 C++，不修改 vendored doctest 和历史基准输入。VS 2022 LLVM 组件中若有此版本，可显式指定路径；也可安装 [同版本 Python 发行包](https://pypi.org/project/clang-format/18.1.8/)。

```powershell
.\scripts\Format.ps1 -Check -ClangFormat 'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang-format.exe'
# 去掉 -Check 可更新格式。格式调整应单独提交。
```

[Windows 工作流](../.github/workflows/windows.yml) 在 push/PR 上检查格式，并分别运行 x64/x86 Release `/analyze`、单元测试、导入白名单和 2 MiB 门禁。构建日志、测试输出、PDB 和源码指纹作为 CI 产物保留 14 天。GitHub 托管机不是物理显示兼容性验收环境。

推送 `v<版本>` 标签后，两架构和格式检查全通过才会创建 Release，仅上传 x64 的 `VisPick.exe`。已有 Release 会跳过发布，保留其说明和附件；新 Release 使用 `docs/releases/VisPick-<版本>.md`。手工发布和标签发布均先核对版本、产物指纹和说明，SHA256 写入发布说明而不另附文件。GitHub 自动生成的源码归档由平台提供，不是产品附件。

本机现有编译器仍是 14.42.34433；脚本放宽范围并不会安装 14.44。推荐安装 VS 2022 17.14 对应工具集后复建；Win7/Win10 的导入检查与真实运行验收仍分开记录。

## 工程资料

- [设计](DESIGN.md)、[已知限制](KNOWN_ISSUES.md)、[P6 清单](P6_CHECKLIST.md)
- [代码审计](reports/P6-code-audit.md)、[测试依赖](../tests/third_party/README.md)
- 后续原始日志、JSON、PNG 放 `out/` 或 CI 产物；仓库保留结论和必要复现素材。已有媒体和历史证据保留，不改写 Git 历史。

## 历史开发记录（以下保留当时状态）

## 当前目标：P6 回归、稳定性与交付准备

依据 [DESIGN.md](DESIGN.md) 已连续实现 P1–P5，产品版本为 **0.5.0-candidate**。用户已授权逐阶段继续，不再设置中途审批点；未实际运行的验收项仍保留未验证状态。

## 任务与验证

- [x] 建立 C++20 / Win32 工程和四个 CMake 预设 → MSVC 14.42、静态 CRT、嵌入 manifest。
- [x] 实现 Bitmap、GDI RAII、兼容层和预分配截屏会话 → 单元测试、静态分析、真实桌面测试。
- [x] 实现浮层和分层贴图原型 → 真实窗口重绘计时、像素对比、资源计数。
- [x] 实现 GIF 原型 → LZW 往返、调色板和 WIC 解码校验、30 fps 实测。
- [x] 完成双架构 Release 构建与导入检查 → 一条 build.ps1 命令产出两个 exe。
- [x] 完成 Debug / ASan 与 /analyze → 四套配置的单元测试通过；ASan 桌面、浮层、贴图及短 GIF 路径未报告内存错误。
- [x] 形成 P0 报告和验收清单 → 见 [技术报告](reports/P0-2026-11-06.md)，明确无丢帧未通过、双 4K 和旧系统未验证。

- [x] P1 托盘、热键、配置热更新、单实例、自启、日志/崩溃转储 → 单测、真实子进程与十分钟空闲测量。
- [x] P2 冻结选区、手柄/微调、窗口识别、放大镜、PNG+DIB 复制 → 独立图样和 100 次取消循环。
- [x] P3 六种标注、原生文字、撤销重做、后台保存 → 编码单测、原生 EDIT 像素比较及 200 次会话。
- [x] P4 分层参考图、变换/收缩/回收站 → 几何与 alpha 单测、20 张 1080p 实际内存门禁。
- [x] P5 有界录制、透明差分、抖动、暂停/取消、预览与保存 → WIC 独立解码、时间戳、真实按钮及临时文件生命周期。
- [ ] P6 最终回归、发布包、Defender、稳定性与兼容矩阵 → 逐项证据见 [P6 报告](reports/P6-2026-11-06.md)。

依赖顺序：基础 → 选区 → 编辑/参考图/录制 → 集成回归 → 发布。并行任务仅修改各自负责的模块。

## 完成标准

- 两个 Release exe 可重现构建；项目代码 `/W4 /WX` 和静态分析无告警。
- 单元测试通过；原型在本机产生可复核的 JSON / Markdown 实测结果。
- 数值不达标时保留失败和原始数据，给出后续工程选择，不放宽设计阈值。
- Win7、其他 DPI、浏览器/照片播放等未实际验证项明确列出。
- 按用户后续授权连续推进；正式 v1.0 标签仍要求实际验收达标。

## 开发环境事件

2026-11-06：启动开发时 D 盘剩余 0 字节，Git 初始化失败。暂在 C 盘临时目录准备和构建；用户选择保留 `D:\Workspace\VisPick` 并释放空间。源码已同步回原目录，并从原目录完成了最终构建和验收。原始设计内容未修改。

## 当前阶段状态

P1–P5 功能已接通，P6 验证进行中。当前显示路径确认为 Microsoft Remote Display Adapter / 32 Hz；远程显示测量不能支持物理显示性能判断或 DXGI 决策。P0 双 4K、旧系统等未测项目不会被后续功能覆盖为通过。

评审前代码保存为提交 `70faa85`、标签 `p0-baseline`，便于复现量化对照基准；这不是阶段验收或发布标签。逐项处理、修正后测试数据与剩余验收见 [P0 评审修正记录](reviews/P0-corrections-2026-11-06.md)。

## 评审修正的工程边界

- 产品 `VisPick.exe` 只链接 `vispick_core`；命令行原型、图样进程、微基准在 `VisPickSpikes.exe`，桌面检查在 `VisPickTest.exe`。开发工具不复制到 `dist/`。
- `CaptureSession::Image()` 是借用的只读 DIB 视图，下次采集或会话销毁后失效。一次截图由 `shared_ptr<const FrozenScreen>` 保持最终 DIB；裁剪导出时才补不透明 alpha。流水线队列为跨线程所有权另做一次有界拷贝，独立计时。
- GIF 正确性与吞吐分开：`--spike-gif` 顺序验证像素/时长；`--spike-gif-pipeline` 使用独立图样进程和编码线程，记录调度跳帧、队列满、图样拒绝、捕获/编码时间。来源 QPC 是 GDI 提交快照，不能当作实际呈现时间。
- 报告 schema 2 分离原始检查结果 `checks_passed` 与验收 `passed`；远程/未知物理输出/低刷新率强制 `not_eligible` 并返回非零。
- 原型窗口显式选择 `Passive`；选区、参考图和预览按交互需要选择 `Interactive`，由独立图样验证键盘和鼠标流程。

## 当前生命周期与验证入口

不透明参考图源图使用无损 BGR24，透明源图保持 BGRA，另持有一个分层窗口 DIB。GIF 使用有界槽位和编码线程池，按采集完成时间补偿延时。后台保存编码完成后才原子替换目标；取消、失败、退出和未领取结果均明确处理资源所有权。

普通退出确认未保存预览/参考图；系统结束会话不再弹框，完成进行中的图像写入，保留已完成但未保存的 GIF 临时文件。自动测试只操作自身进程和图样；剪贴板测试使用独立窗口站，不终止用户 Explorer。

```powershell
.\build.ps1 -Analyze
.\build.ps1 -Configuration Debug -Analyze
.\tests\e2e\RunProductChecks.ps1 -Architecture x64 -FullRecording -IdleSeconds 600
.\tests\e2e\RunProductChecks.ps1 -Architecture x86 -FullRecording
.\tests\e2e\RunSoak.ps1 -Seconds 28800 -IntervalSeconds 30
.\scripts\Package.ps1
```

桌面测试保持会话解锁并串行执行，避免其他窗口遮挡图样。浸泡测试记录真实时长、间隔与资源；八小时命令必须实际经过八小时，短测不能替代。旧系统、混合 DPI、真实输入法、跨应用粘贴/播放按 [检查清单](P6_CHECKLIST.md) 保留未测边界。
