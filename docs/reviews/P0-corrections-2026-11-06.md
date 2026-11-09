# P0 评审修正记录

日期：2026-11-06。依据 [原评审](P0-code-review-2026-11-06.md)，修正对象为 `D:\Workspace\VisPick`。修正前基线：Git 提交 `70faa85` / 标签 `p0-baseline`。

基础代码和测试方法已按下表修正；P-06 完成有限误差复用与真实素材测试，照片色带和视频时域质量尚未解决，不能标为关闭。P0 尚未完成目标环境验收，P1 未开始。原始 JSON 和二进制哈希保持历史原样，[原报告](../reports/P0-2026-11-06.md) 中的 DXGI 决策明确撤回。修正后报告单独保存在 `docs/reports/data/review-*`。

## 逐项处理

| 编号 | 处理与证据 | 边界 |
|---|---|---|
| P-01 | `Compat` 检测 SM_REMOTESESSION、WTS 协议、GlassSessionId、显示适配器/刷新率及 QueryDisplayConfig 物理输出。schema 2 单列 `checks_passed` 和验收状态，未知/远程/低于 50 Hz 一律 `not_eligible`，退出码 1。 | 本会话已确认为 Microsoft Remote Display Adapter、2560×1440、120 DPI、32 Hz。AC0-7 必须物理重测。 |
| P-02 | `CaptureSession` 仅保留最终 DIB，返回 `BitmapView`，去除逐像素整屏 alpha 修正；`CropOpaque` 在裁剪后补 alpha。新增 320×240、1000 次原地采集及内存/地址稳定检查，分开 BitBlt 和必要的 GdiFlush。 | 物理显示上的速度提升尚未测得，不把远程同步等待解释成 GDI 带宽。 |
| P-03 | 浮层使用共享 `FrozenScreen` 加 mask/back-buffer，共三份整屏图。采集直接写最终冻结 DIB，记录初始化、私有提交内存的采样峰值和关闭后增量。 | 保留简单的预计算 mask；没有未经基准支持的 AVX2/线程化。峰值是采样值，非进程生命周期精确峰值。双 4K 待测。 |
| P-04 | 按 2 的幂写实际调色板，传递 `max(2, ceil(log2(table_size)))`；支持全局调色板。2/3/16/255 色均 WIC 独立解码逐像素相等，微基准输出旧格式与紧凑格式字节数。 | 流水线当前仍用局部表；P5 整段/差分调色板策略未提前实现。 |
| P-05 | 精确量化换为 1024 项开放寻址表；复用工作区和索引缓冲。开发目标内保留 `70faa85` 的原算法作 1080p、2/16/200/300 色交替顺序对照。 | 报告记录真实提升或回退，不预设每种输入都更快。 |
| P-06（部分完成） | 有界误差的上一帧调色板复用，精确 UI 色不近似；高色输入逐通道检查误差阈值 8。静止帧索引/调色板稳定性测试；两张 NASA 照片和 12 张真实连续视频帧的吞吐、误差、WIC 与目测证据。 | 照片渐变仍有色带，12 张视频帧均未触发阈值内的调色板复用。未通过完整质量验收；抖动、全局/差分调色板策略留在 P5，不扩大为本轮编码器重写。 |
| P-07 | 标准句柄无效时 CONOUT$ 兜底并保留已有重定向；控制台用 WriteConsoleW，文件/管道 UTF-8。MSVC 异常按 ACP 转换，filesystem_error 路径取原生 UTF-16。四类启动/输出集成测试。 | 本机旧版直接控制台已通过，评审关于 AttachConsole 的绝对表述不成立，详见下节。 |
| P-08 | JSON/Markdown 共用数值格式器；整数完整十进制，非整数 12 位有效数字，非有限值 JSON 为 null。覆盖百万级整数的报告文件测试。 | 不宣称 double 可精确表达所有 64 位整数；当前计数规模在精确范围内。 |
| P-09 | 显式 `WindowClassOptions` 定义类名、样式、光标，按类型注册，冲突配置拒绝创建。 | 原型使用独立 `VisPick.Spike.*` 类名。 |
| P-10 | 消息回调异常计数、OutputDebugString 和 noexcept 回调；创建消息失败返回 FALSE/-1 并解除关联。 | 不让 C++ 异常越过 Win32 回调；未强制每个异常终止进程，便于验证错误路径。 |
| P-11 | Debug 断言派生析构已 Destroy；Release 兜底先解除 HWND→对象关联，避免向已析构状态分发。 | 窗口仍要求在创建线程销毁；单测验证清理幂等。 |
| P-12 | DWM 统一静态导入；现代 DPI/捕获排除特性保留版本探测。 | 导入检查不能替代 Win7 实机测试。 |
| P-13 | `GifReader` 读取逻辑画布/帧位置/透明/处置元数据与原始子帧，`GifCanvas` 单独合成；WIC 验证差分子帧与 disposal 1/2/3。 | 不再假设每帧与画布同尺寸。 |
| P-14 | 串行与流水线共用 `TimestampDelayAccumulator`，从实际采集完成时刻生成 10 ms 延时并传递余数。 | 零延时合帧；超范围/逆序有测试。 |
| P-15 | `LzwEncoder` 工作区按对象复用；索引范围验证并入编码遍历。 | 保留 Release 运行时校验，未采纳降为 Debug-only 的建议，避免损坏输入越界。 |
| P-16 | 建立评审前基线提交/标签及评审修正提交/标签，生成物继续忽略。 | 标签仅表示代码检查点，不是 P0 整体验收或产品发布。 |
| D-01 | `VisPick` 只链接 core；`VisPickSpikes`、`VisPickTest` 独立，大小/导入门禁只针对产品。 | 产品目前是 P0 入口，不能作为日常截图工具。 |
| D-02 | 独立图样进程、采集线程、8 个预分配帧槽和编码线程；离线 WIC 验证。顺序实验只判定正确性，独立报告调度/队列/采集/编码。 | 工作者为防止把被遮挡的桌面内容保存，写入前验证已知图样；这项开销独立计时。源 QPC 只表示提交快照，不伪造精确呈现对应。 |
| D-03 | 使用 QPC 采集完成时间；被拒绝/跳过的时间由上一有效帧保持，尾帧延续至停止时刻。 | 首帧前采集启动时间不算 GIF 时间轴，另列 wall duration。 |
| D-04 | `shared_ptr<const FrozenScreen>` 持有一份 DIB、虚拟桌面坐标和只读视图；输出按选区裁剪。 | 窗口快照属于 P2 窗口吸附，本轮没有加入空抽象。流水线跨线程拷贝为有界所有权移交，单独计时。 |
| D-05 | 已核实 BeginPaint 的目标 DC 自动按精确更新区域裁剪，`rcPaint` 是边界，不等于实际写入区域。保留回贴方式并明确注释；back-buffer 继续逐条 dirty strip 合成。 | 不重复 GetUpdateRgn/裁剪；双 4K 的实际驱动传输成本仍待实测。 |
| D-06 | `WindowActivation::{Interactive, Passive}` 必填；测试窗口显式被动，单测检查扩展样式。 | 产品键盘交互随 P1/P2 验证。 |
| D-07 | waitable timer + MsgWaitForMultipleObjectsEx 等待，源进程另监听父进程/停止事件。 | 所有等待检查取消；WM_QUIT 未来/过期截止时刻回归测试。 |

## 与原评审判断的差异

1. **AttachConsole**：微软文档说明 GUI 子系统初始标准句柄可能无效，附加控制台后可以变为有效；继承/重定向句柄也可能已有效。本机对评审前 exe 的直接控制台、文件及无控制台启动对照通过。改动用于保证无效句柄兜底、保留重定向和统一 Unicode，不宣称已复现“直接启动必然弹窗”。新的错误测试还要求回显中文未知参数，旧版不包含该原文，不能把这一失败冒充为编码损坏证据。[AttachConsole](https://learn.microsoft.com/en-us/windows/console/attachconsole)、[GetStdHandle](https://learn.microsoft.com/en-us/windows/console/getstdhandle)。
2. **脏区域回贴**：BeginPaint 本身设置精确更新区裁剪。不能只根据 BitBlt 接受外接矩形就认定窗口内部全部重新写入；显式重新裁剪属于重复操作。[BeginPaint](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-beginpaint)。
3. **远程识别**：SM_REMOTESESSION 为 0 不足以证明本地物理会话，因此加入 WTS、GlassSessionId 和活动物理输出路径证据；未能确认时采取“不具备验收资格”。[RemoteFX 检测边界](https://learn.microsoft.com/en-us/windows/win32/termserv/detecting-the-terminal-services-environment)。

## 验证数据

所有此会话的桌面报告均为 `not_eligible`，即使 `checks_passed=true`。以下是功能诊断结果，不能替代物理显示验收。

最终 `build.ps1 -Architecture all -Analyze` 与 `build.ps1 -Architecture x64 -Configuration Debug -Analyze` 均成功；x64 Release、x86 Release、x64 ASan 各通过 **70 个测试、51889 条断言**。项目目标 `/W4 /WX`，生产与开发工具代码 `/analyze` 无告警；最终产品导入白名单和 2 MiB 门禁通过。日志见 `review-build-release.log`、`review-build-asan.log` 和 `review-unit-*.log`。本轮未重跑 x86 Debug。

| 检查 | x64 Release | x86 Release | 证据 |
|---|---:|---:|---|
| 桌面逐像素差异 | 0 / 11366400 | 0 / 11366400 | `review-*-desktop.json` |
| 100 次生命周期 GDI / USER 增量 | 0 / 0 | 0 / 0 | 同上 |
| 1000 次小区域采集私有提交内存增量 | -32768 B | 本轮未重跑该微基准 | `review-x64-capture.json` |
| 浮层整屏缓冲 | 3 份 | 3 份 | `review-*-overlay.json` |
| 浮层关闭后私有提交内存增量 | 135168 B | 1142784 B | 均小于 5 MiB |
| 顺序 GIF / 10 秒：WIC 差异 | 0 / 130 帧 | 0 / 130 帧 | `review-*-gif.json`，不用于帧率判定 |
| 流水线 / 10 秒：采集次数 | 300 | 300 | `review-*-pipeline.json` |
| 流水线调度跳帧 / 队列满 | 0 / 0 | 0 / 0 | 同上 |
| 流水线拒绝的未知或撕裂图样 | 1 | 3 | 两次均保留 `checks_passed=false`，不重试隐藏失败 |
| 流水线 WIC 差异 | 0 / 299 有效帧 | 0 / 297 有效帧 | 只保存已核实图样 |
| 流水线时间轴误差 | 0.0633% | 0.0239% | 不是帧率通过证据 |
| 控制台 / 文件 / 管道 / 无控制台启动 | 4/4 | 4/4 | `review-console-*.log` |

x64 ASan 的独立流水线 2 秒采集 60 帧，源图样和 WIC 校验均零差异，无 ASan 诊断；原始结果见 `review-asan-pipeline.json`。Release 中被拒绝帧的具体原因未定位：可能涉及显示呈现或遮挡，仅凭现有快照不能区分。未用合成数据替代捕获，也未因此放宽门槛。

320×240 与 1920×1080 的 SRCCOPY 中位数分别为 31.2421 ms 和 31.3210 ms；这个诊断结果支持固定同步开销的怀疑，仍不能证明物理 GPU 上的 BitBlt 速度。浮层在当前画布的初始化为 58.7243 ms，不等同于双 4K 上的真实热键到可见时延。

紧凑 GIF 格式对照（640×360，同一输入，旧格式固定 256 色表/8 位最小码长；两边均 WIC bit-exact）：

| 色数 | 修正前 | 修正后 | 减少 |
|---|---:|---:|---:|
| 2 | 2065 B | 1183 B | 42.7% |
| 3 | 2364 B | 1484 B | 37.2% |
| 16 | 4597 B | 3735 B | 18.8% |
| 255 | 42439 B | 42439 B | 0% |

首次量化对照保留在 `review-x64-quantizer.json`：2/16/200 色从 3.3334/3.2880/3.5966 ms 降至 1.3262/1.2206/1.3517 ms；300 色 Wu 因新增误差统计和复用映射遍历从 5.5633 ms 上升至 10.5752 ms。该回退触发了循环融合与 SSE2 误差归约修正，数据分轮保留，不覆盖首次记录。

最终 Wu 在原本的索引输出遍历中计算精确最大通道误差，使用 SSE2 字节饱和差/最大值，末尾只归约 RGB，忽略 alpha；不使用 SSSE3/AVX。仅在需要复用时准备 32K 桶映射，去掉额外的全图遍历。SSE2 与当前 MSVC x86 默认代码生成基线一致，不新增 AVX 硬件要求。[MSVC `/arch` 默认值](https://learn.microsoft.com/en-us/cpp/build/reference/arch-x86?view=msvc-170)。300/512/4096 色及变化 alpha 的单测对照旧版调色板、索引和独立计算的精确误差。

最终官方微基准（1080p，15 次交替顺序中位数，所有调色板和索引与基线相同；`review-*-quantizer-sse2.json`）：

| 色数 | x64 旧版 → 修正版 | x86 旧版 → 修正版 |
|---|---:|---:|
| 2 | 3.2623 → 1.2014 ms | 3.9901 → 2.0697 ms |
| 16 | 3.3032 → 1.2363 ms | 3.9634 → 2.0761 ms |
| 200 | 3.5989 → 1.1937 ms | 4.4805 → 2.0614 ms |
| 300（Wu） | 5.5117 → 5.6632 ms | 5.9509 → 7.0009 ms |

精确 UI 路径提升明显。Wu 相对旧版增加了精确最大误差统计，仍有约 2.7%（x64）/17.6%（x86）成本；这不是 Wu 算法吞吐提升，也不把它藏进总体提速结论。保留这项可验证的质量信息，本轮停止继续引入复杂向量化或直方图结构改动。

### 真实素材质量

素材与复现方法见 [quality-input](../reports/data/quality-input/README.md)。两张官方 NASA JPEG 保持原样；Big Buck Bunny 取官方 320×180 / 24 fps 视频中 30.0000～30.4583 秒的 12 张连续帧，系统 Media Foundation 解码，时间戳和 SHA-256 随素材保存。图片合计约 1.43 MB，完整视频只在忽略的 `out/` 中。

逐张静止重复量化的调色板与索引均不变，全部输出 GIF 的 WIC 回读与量化后像素一致。照片的 RGB PSNR 分别为 47.80 dB / 40.29 dB，视频各帧约 34.79～35.08 dB。这里比较的是解码后的 8 位 RGB 数值，没有 ICC/线性光转换；PSNR 不能代替目测。

实际查看源照片与 GIF 后：日落光晕和大气蓝色渐变中可见分层/色带，虽然主体和地平线结构保留，仍不能据此宣布照片质量通过。视频首帧的树叶/草地细节有量化损失，整体结构可辨；本轮未做实时播放器的连续闪烁验收。全部 12 帧的 `palette_reused=0`，说明当前逐通道最大误差 8 的策略没有解决这个视频片段的跨帧调色板变化。不会通过放宽误差阈值掩盖这一限制。

最终源码对应 `review-x64-quantizer-sse2.json`、`review-x86-quantizer-sse2.json`、`review-x64-video-quality-sse2.json`；前一轮标量归约记录保留为 `review-*-quantizer-final.json` / `review-x64-video-quality.json`。最终 x64 两图量化中位数分别为 4.2794 ms / 2.0982 ms；320×180 视频各帧为 0.9790～1.1116 ms。数值和视觉质量不因误差统计归约优化而改变。重跑命令会生成可查看的单图 GIF 和固定 10 fps 比较动画；后者仅用于慢速观察，不能当作源视频帧率。

## 代码检查点与产品产物

`p0-baseline` 保存评审前版本；`p0-review-corrections` 保存本轮修正。两个标签均是代码检查点，不代表 P0 验收或产品发布。当前产品入口仅提供阶段说明、帮助和版本；开发实验通过构建目录下的 `VisPickSpikes.exe` / `VisPickTest.exe` 运行。

| 当前产物 | 大小 | SHA-256 |
|---|---:|---|
| `dist/VisPick.exe`（x64） | 138752 B | `81E59C57E2446A19D4802A1CBC47426099FBB195B229AC029C947F78D3713326` |
| `dist/VisPick-x86.exe` | 102912 B | `A26916ECC7E70C3A89B60FE85F3B171A257211DEE5DD756CD59DEA1AB22A08A5` |

哈希对应本次生成的二进制，后续重建可因 PE/链接时间等发生变化；二进制不提交入库。

## 仍需目标环境验收

- 本地双 4K / 175% 的初始化、三缓冲内存、关闭恢复、浮层绘制和 1080p BitBlt 分布；在此前不决定 DXGI。
- 物理显示下的独立流水线 1280×720 / 30 fps / 10 秒、源帧覆盖及队列压力。
- Win7 SP1 / Win10 / 纯净 Win11 实际运行，浏览器与“照片”播放。
- GIF 照片/视频主观质量、抖动及差分策略的 P5 专项验收。

GIF 格式/合成依据：[GIF89a](https://giflib.sourceforge.net/gifstandard/GIF89a.html)、[WIC 元数据查询](https://learn.microsoft.com/en-us/windows/desktop/wic/-wic-native-image-format-metadata-queries)、[微软 WIC Animated GIF 样例](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/Win7Samples/multimedia/wic/wicanimatedgif/WicAnimatedGif.cpp)。
