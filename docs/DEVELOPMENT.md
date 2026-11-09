# 开发记录

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
