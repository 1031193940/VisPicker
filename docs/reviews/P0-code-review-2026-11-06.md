# VisPick P0 实现评审

> 日期：2026-11-06 ｜ 评审对象：`D:\Workspace\VisPick` 当前工作区（P0 阶段，版本 `0.0.1-p0`，尚无 Git 提交）
> 依据：[DESIGN.md](../DESIGN.md)、[P0 报告](../reports/P0-2026-11-06.md)、全部 `src/`、`tests/`、`scripts/`、`CMakeLists.txt`、`build.ps1`
> 范围说明：本文只评审，没有修改任何源码。凡是“推断”或“假设”的地方都已明确标注，并给出验证方法。

---

## 目录

1. 总体结论
2. 关键发现：GIF “采集瓶颈”很可能是测试环境造成的
3. 问题清单总览
4. 缺陷与问题详述（中等）
5. 缺陷与问题详述（轻微）
6. 设计层面的建议
7. 做得好的地方
8. 建议的行动计划
9. 附录：评审方法与引用

---

## 1 总体结论

P0 的工程基础扎实：工具集锁定、导入白名单、RAII 封装、`/W4 /WX /analyze` 零告警、35 个单元测试、ASan 通过、像素级校验，报告也很诚实。

但有三类问题需要在进入 P1 之前处理：

| 类别 | 结论 |
|---|---|
| 验证结论 | P0 报告中“采集比编码更贵、应引入 DXGI”的结论证据不足，很可能是测试会话使用了远程显示适配器造成的假象（见 §2）。**必须在真实本地会话上重测后再决定。** |
| 实现缺陷 | 采集与浮层路径存在多余的整屏拷贝；GIF 编码路径有体积和速度浪费；若干辅助代码（控制台输出、报告精度）有 bug。 |
| 架构隐患 | 产品 exe 链接了全部 spike 代码；窗口基类的类名、异常处理、析构规则会在 P1 之后制造麻烦；AC0-6 的测试方法本身无法通过。 |

---

## 2 关键发现：GIF “采集瓶颈”很可能是测试环境造成的

### 2.1 报告的结论

[P0 报告](../reports/P0-2026-11-06.md#L9)写道：当前会话的采集路径比 GIF 编码更昂贵，按 AC0-7 应在 P5 引入 DXGI 采集。依据是：1280×720 / 30 fps / 10 秒实验中丢失 58.67% 的节拍，采集平均 39.9 ms，编码平均 3.8 ms。

### 2.2 证据

| 编号 | 证据 | 来源 |
|---|---|---|
| E1 | 全桌面 2560×1440 的 BitBlt 中位数为 31.16 ms，1080p 区域为 31.26 ms。面积相差近一倍，耗时几乎相同。BitBlt 是带宽受限操作，耗时通常随面积变化，所以这更像是在等待某个固定的同步点 | `docs/reports/data/x64-Release.json` |
| E2 | 评审时在本机查询显卡，列表中包含 **Microsoft Remote Display Adapter，分辨率 2560×1440，当前刷新率 32 Hz**；同时存在 NVIDIA GeForce RTX 5080 和 Intel Graphics（均为 3840×2160、59 Hz）。1000 ÷ 32 = 31.25 ms，与 E1 的 31.2 ms 吻合 | `Win32_VideoController` 查询 |
| E3 | 报告记录的实测环境是“1 台 2560×1440、120 DPI（125%）”，而设计阶段记录的真实环境是两台 3840×2160、175%。报告里的这台“显示器”应当就是远程显示适配器 | [P0 报告第 29 行](../reports/P0-2026-11-06.md#L29) |
| E4 | [GifSpike.cpp:268-272](../../src/spikes/GifSpike.cpp#L268-L272) 在每次采集前调用 `DwmFlush()` 等待一次合成，随后 `BitBlt` 可能再等待一次。所以 GIF 实验里采集平均耗时（39.9 ms）比单独测量（31 ms）更高 | 源码 + 报告 |

### 2.3 推断与边界

- **推断（未经验证）**：采集耗时被远程显示适配器的 32 Hz 刷新节拍限制，而不是 GDI 本身慢，也不是 GPU 回读慢。
- **不能断言的事**：不能由此推出“真实 GPU 上 BitBlt 一定够快”。1080p 的 BitBlt 在真实机器上也可能是 5–20 ms，只是现在没有任何有效数据。
- **影响**：报告中的以下结论都应视为“未验证”：
  - AC0-6 的丢帧率 58.67%；
  - AC0-7 触发了 DXGI 采集决策；
  - “BitBlt 两种模式耗时接近，不能把关闭 `CAPTUREBLT` 当作性能解法”（这句话本身仍然成立，但原因应是“被同步点限制”，而不是“两者本来就一样快”）。

### 2.4 验证方案

| 步骤 | 做法 | 判定 |
|---|---|---|
| 1 | 确认测试会话是本地控制台、物理显示器：`GetSystemMetrics(SM_REMOTESESSION)` 为 0，`EnumDisplaySettings` 的 `dmDisplayFrequency` ≥ 50 | 否则数据无效 |
| 2 | 在物理显示器上重跑 `--spike-capture` | 看 1080p BitBlt 的中位数、p95 |
| 3 | 增加一个“320×240 小区域、循环 BitBlt、不调用 `DwmFlush`、不做任何其他工作”的基准 | 若耗时与 1080p 相同，说明仍是固定同步开销；若明显更小，说明是带宽受限 |
| 4 | 重跑 `--spike-gif`（或按 §6.2 重做后的测试） | 看丢帧率和采集耗时 |
| 5 | 仅当步骤 2–4 在物理显示器上仍然 > 15 ms，才开 DXGI Desktop Duplication 原型 | 对应 AC0-7 的原定判据 |

### 2.5 对测试框架的配套修改

- 报告头部和 JSON 中增加 `remote_session`（布尔）、每台显示器的 `refresh_hz`、显卡名称。
- 当 `remote_session` 为真，或任一显示器刷新率低于 50 Hz 时，spike 在结果里标红并注明“非验收有效环境”，不允许其“通过”。

---

## 3 问题清单总览

| 编号 | 级别 | 位置 | 概述 |
|---|---|---|---|
| P-01 | 关键 | 测试环境 / 报告 | 采集瓶颈结论可能源于远程显示适配器（§2） |
| P-02 | 中 | [ScreenGrab.cpp:25-38](../../src/capture/ScreenGrab.cpp#L25-L38) | 采集后又逐像素复制整屏，内存和耗时翻倍 |
| P-03 | 中 | [OverlaySpike.cpp:23-33](../../src/spikes/OverlaySpike.cpp#L23-L33) | 进入浮层要分配并写入约 6 份整屏缓冲，双 4K 上约 400 MB |
| P-04 | 中 | [GifWriter.cpp:128-135](../../src/gif/GifWriter.cpp#L128-L135) | 总是写 256 项调色板、LZW 最小码长固定为 8，浪费体积 |
| P-05 | 中 | [Quantizer.cpp:205-231](../../src/gif/Quantizer.cpp#L205-L231) | 精确调色板路径逐像素查 `unordered_map`，速度偏慢 |
| P-06 | 中 | [Quantizer.cpp:12](../../src/gif/Quantizer.cpp#L12) | 每帧独立调色板导致视频内容色彩闪烁；Wu 仅 5 位精度，渐变有色带 |
| P-07 | 中 | [main.cpp:54](../../src/main.cpp#L54) | `AttachConsole` 后没有重开 `CONOUT$`，终端里直接运行 `--help` 会弹 MessageBox；错误信息编码不一致 |
| P-08 | 中 | [Report.cpp:60](../../src/core/Report.cpp#L60) | 只给 JSON 设了精度，Markdown 的大数被截断为科学计数法 |
| P-09 | 轻 | [Window.cpp:7](../../src/platform/Window.cpp#L7) | 所有窗口共用一个类名 |
| P-10 | 轻 | [Window.cpp:68-73](../../src/platform/Window.cpp#L68-L73) | 消息处理异常被静默吞掉 |
| P-11 | 轻 | [Window.cpp:10-12](../../src/platform/Window.cpp#L10-L12) | 基类析构依赖派生类先调用 `Destroy()` 的隐含规则 |
| P-12 | 轻 | [Compat.cpp:153](../../src/platform/Compat.cpp#L153)、[CMakeLists.txt:55](../../CMakeLists.txt#L55) | `dwmapi` 既动态加载又静态链接 |
| P-13 | 轻 | [GifWriter.cpp:182](../../src/gif/GifWriter.cpp#L182) | `InspectGif` 要求所有帧同尺寸，与 P5 差分帧冲突 |
| P-14 | 轻 | [GifSpike.cpp:341](../../src/spikes/GifSpike.cpp#L341) | 实际使用的延时公式与有测试的 `FrameDelay()` 是两份代码 |
| P-15 | 轻 | [Lzw.cpp:43-51](../../src/gif/Lzw.cpp#L43-L51) | 每帧额外扫描一遍像素做校验，并分配两个 8192 项的表 |
| P-16 | 轻 | 工作区 | 没有任何 Git 提交 |
| D-01 | 设计 | [CMakeLists.txt:72-73](../../CMakeLists.txt#L72-L73) | 产品 exe 链接了全部 spike 代码 |
| D-02 | 设计 | [GifSpike.cpp](../../src/spikes/GifSpike.cpp) | AC0-6 的测试方法本身无法达到“零丢帧” |
| D-03 | 设计 | [GifSpike.cpp:341](../../src/spikes/GifSpike.cpp#L341) | 帧延时按名义时刻而不是实际采集时间计算 |
| D-04 | 设计 | 采集/浮层/输出 | 缺少统一的“冻结图”所有权模型 |
| D-05 | 设计 | [OverlaySpike.cpp:72](../../src/spikes/OverlaySpike.cpp#L72) | 回贴用 `rcPaint` 外接矩形，大选区斜向移动时拷贝了未变化区域 |
| D-06 | 设计 | [OverlaySpike.cpp:36](../../src/spikes/OverlaySpike.cpp#L36)、[PinSpike.cpp:49](../../src/spikes/PinSpike.cpp#L49) | 原型窗口使用 `WS_EX_NOACTIVATE`，与设计中的键盘交互冲突 |
| D-07 | 设计 | [GifSpike.cpp:142-147](../../src/spikes/GifSpike.cpp#L142-L147) | 等待用 `Sleep(1)` 轮询而非事件驱动 |

---

## 4 缺陷与问题详述（中等）

### P-02 采集后又整屏复制一遍

**位置**：[ScreenGrab.cpp:25-38](../../src/capture/ScreenGrab.cpp#L25-L38)

**现象**：`Capture()` 先 `BitBlt` 到 `captureSurface_`（DIB），`GdiFlush` 之后，用 `std::transform` 把每个像素 `| 0xFF000000` 写入另一个缓冲 `image_`。

**影响**：
- 内存占用翻倍：7680×2160 下，`captureSurface_` 与 `image_` 各约 66 MB。
- 每次采集多一次整屏的读写遍历。P0 报告中 1080p 的 31 ms 也包含了这次复制，所以采集耗时的口径并不干净。
- 对 30 fps 录制，这是每帧都要付出的固定开销。

**原因分析**：GDI 的 `BitBlt` 不会写 alpha 通道（保持为 0），而 `Bitmap` 约定是不透明 BGRA，所以补了一遍。但只有在导出 PNG 或 `UpdateLayeredWindow` 时才需要 alpha。GDI 用 `SRCCOPY` 画图不需要。

**建议**：
1. 让 `CaptureSession` 直接暴露 `DibSection` 的像素（只读视图），不做额外复制。
2. “补 alpha”移到真正需要的地方（PNG 编码、剪贴板的 PNG 格式、分层窗口），并且只对选区裁剪后的小图执行。
3. 录制路径中的帧差分和 GIF 编码都只读 RGB，直接用 `& 0x00FFFFFF` 比较即可（测试代码已经这样做）。

**验收标准**：
- 重复采集 1000 次，私有内存基线不变；
- 同一区域的采集耗时较现在下降（具体幅度在物理显示器上实测记录，不预设数值）。

### P-03 浮层进入时的内存峰值过高

**位置**：[ScreenGrab.cpp:40-44](../../src/capture/ScreenGrab.cpp#L40-L44)、[OverlaySpike.cpp:23-33](../../src/spikes/OverlaySpike.cpp#L23-L33)

**现象**：进入浮层的初始化链路如下：

| 步骤 | 分配的整屏缓冲 |
|---|---|
| `CaptureScreen` → `CaptureSession` | DIB 一份 + `Bitmap` 一份 |
| `return session.Image()`（按值返回） | 再复制一份 |
| `std::copy` 到 `frozen_` | 一份 |
| 逐像素生成 `masked_` | 一份 |
| `backBuffer_` | 一份 |

合计约 6 份。7680×2160 时每份约 66 MB，总计约 400 MB，并且全部要被写入一遍（缺页和清零也有成本）。

**影响**：
- 直接威胁 N5（热键到浮层 ≤ 150 ms，双 4K）。P0 报告里的 59.67 ms 来自 2560×1440 的 3.7 MP 画布，7680×2160 是 16.6 MP，约 4.5 倍。
- x86 版本只有 2 GB（开启 `/LARGEADDRESSAWARE` 后最多 4 GB）地址空间，更容易遇到碎片和分配失败。
- 与“没有参考图时空闲内存 ≤ 10 MB”的目标相比，浮层关闭后必须确认全部释放。

**建议**：
1. `BitBlt` 直接写入最终要保留的 `frozen_`（只此一份），不经过中间缓冲，也不要按值返回。
2. 压暗图不预先生成：用 `AlphaBlend` 在绘制脏矩形时叠加半透明黑色，或者先显示原图，再在后台线程补算压暗图。
3. 若保留预计算，则用 SSE2/AVX2 或多线程做 `×55/100`，并把 `/ 100` 换成乘法加移位（现在的整数除法依赖编译器优化）。
4. 采用 §6.4 的统一“冻结图”对象。

**验收标准**：
- 进入浮层的私有内存峰值不超过 3 份整屏缓冲（冻结图、后备缓冲，加至多一份压暗图）；
- 在 7680×2160 上实测“热键到浮层出现”并记录（AC0-4 的前提）；
- 浮层关闭后私有内存回到基线 ±5 MB 以内。

### P-04 GIF 写入器的体积浪费

**位置**：[GifWriter.cpp:128-135](../../src/gif/GifWriter.cpp#L128-L135)

**现象**：每帧都写 256 项局部调色板（768 字节），并且 LZW 最小码长固定为 8。

**影响**：
- 调色板浪费：界面类画面往往只有几种到几十种颜色，每帧多出数百字节。
- 码长浪费：用 8 位的最小码长，LZW 的初始码字长度是 9 位，而 4 色图像只需 3 位。压缩率明显变差。
- 差分子矩形帧很小，调色板开销占比会更高。

**建议**：
1. 调色板大小取 2 的幂次方向上取整（最小 2 项），局部调色板按此大小写，并设置标志位中的大小字段。
2. LZW 最小码长取 `max(2, ceil(log2(调色板大小)))`，`EncodeLzw` 已经支持参数，只需在 `EncodeFrame` 和 `WriteFrame` 里传递。
3. 考虑全局调色板：当整段录制颜色稳定时，只写一份，帧里不再带调色板（对应 P-06 的调色板复用）。

**验收标准**：
- 单元测试：用 2、3、16、255 色的合成图编码，再用 WIC 解码，结果逐像素一致；
- 对 ≤ 8 色的测试图样，单帧编码结果小于现在的实现（以字节数对比记录）。

### P-05 精确调色板路径的速度

**位置**：[Quantizer.cpp:205-231](../../src/gif/Quantizer.cpp#L205-L231)

**现象**：精确路径对每个像素：先与上一个像素比较（游程缓存），不同则在 `unordered_map` 中查找。

**影响（推测，需要基准确认）**：
- 抗锯齿文字边缘的颜色几乎逐像素变化，游程缓存命中率低。1080p 有约 200 万像素，每次哈希查找通常是几十纳秒，总计可能达到数十毫秒，远超 30 fps 的预算。
- `unordered_map` 是节点式容器，缓存局部性差。
- 颜色超过 255 种时，要扫描到第 256 种颜色才放弃，已处理的像素白白浪费，然后整幅图再走一遍 Wu。

**建议**：
1. 用固定大小的开放寻址哈希表（例如 1024 项，键为 24 位 RGB，值为索引），或者 15 位 RGB 的 32 K 项直接查找表做第一层，冲突时再精确确认。
2. 先对帧差分得到的变化区域做量化（P5 已规划），像素数会小得多。
3. 颜色数统计与量化合并成一次遍历。

**验收标准**：
- 加一组微基准：1080p 的 2 色、16 色、200 色、300 色合成图，分别记录量化耗时；
- 记录改造前后的对比，不预设绝对数值。

### P-06 视频内容的色彩闪烁和色带

**位置**：[Quantizer.cpp:12](../../src/gif/Quantizer.cpp#L12)、[Quantizer.cpp:116-201](../../src/gif/Quantizer.cpp#L116-L201)

**现象**：
- 每一帧独立量化出各自的调色板。
- Wu 把每个通道量化到 5 位（32 级）后建立直方图。

**影响**：
- 静止或缓慢变化的区域，相邻两帧的调色板不同，量化后的颜色会跳变，肉眼表现为闪烁。
- 5 位精度对渐变和暗部不够细，容易出现色带。
- P0 的测试图样不超过 8 色，只走了精确路径，**Wu 路径没有任何质量或吞吐数据**（报告也承认了这一点）。

**建议**：
1. 调色板复用：新帧的颜色集合都能映射到上一帧调色板（误差在阈值内）时，直接沿用，不重新量化。
2. 对变化区域之外的像素使用透明索引（P5 已规划），根本不重新量化。
3. 视频内容可选抖动（配置项 `Dither` 已在设计里）；先把无抖动版本跑通。
4. 若色带明显，把直方图精度提高到 6 位（需要重新评估 33³ 的积分表大小）。

**验收标准**：
- 增加真实内容的测试素材（照片类渐变、视频帧），记录 Wu 路径的耗时和主观质量；
- 在静止画面的 GIF 中，相邻帧的调色板索引映射差异为 0。

### P-07 控制台输出

**位置**：[main.cpp:20-47](../../src/main.cpp#L20-L47)、[main.cpp:54](../../src/main.cpp#L54)

**现象**：
1. GUI 子系统程序在终端里运行时，`AttachConsole(ATTACH_PARENT_PROCESS)` 只是挂到父进程的控制台，**不会更新进程的标准句柄**。如果没有重定向，`GetStdHandle(STD_OUTPUT_HANDLE)` 返回空，`ShowHelp` 就走到 `MessageBoxW` 分支。
2. 重定向到文件或管道时，`PrintConsole` 把 `what()` 返回的 ANSI 文本原样写出，而 `ShowHelp` 写的是 UTF-8，两处编码不一致。中文系统上错误信息会出现乱码。

**影响**：
- 报告里“帮助重定向输出非空”的测试只覆盖了重定向场景。用户在终端里直接输入 `VisPick.exe --help`，看到的是弹窗，而不是文字。
- 脚本调用时错误信息可能乱码。

**建议**：
1. `AttachConsole` 成功后，如果标准句柄无效，就用 `CreateFileW(L"CONOUT$", ...)` 取得输出句柄，并 `SetStdHandle`。
2. 控制台输出统一用宽字符 API（`WriteConsoleW`）；管道和文件输出统一转成 UTF-8。
3. 异常信息先统一转成 UTF-8（`what()` 在 MSVC 下是 ANSI），再输出。

**验收标准**：在 PowerShell 直接运行、`> file`、`| more` 和双击启动四种情况下，帮助和错误信息都符合预期。

### P-08 报告里的大数被截断

**位置**：[Report.cpp:60](../../src/core/Report.cpp#L60)、[Report.cpp:88](../../src/core/Report.cpp#L88)

**现象**：`json` 流设置了 `std::setprecision(12)`，`markdown` 流没有，默认 6 位有效数字。像素数 11,366,400 会输出为 `1.13664e+07`，文件大小 1,717,873 会输出为 `1.71787e+06`。

**影响**：自动生成的 `results.md` 丢失精度，容易被直接引用到报告里造成错误。

**建议**：给 `markdown` 同样设置精度，并对整数值用不带科学计数法的格式输出；增加单元测试，覆盖 ≥ 10^6 的值。

---

## 5 缺陷与问题详述（轻微）

### P-09 窗口类名

[Window.cpp:7](../../src/platform/Window.cpp#L7) 所有窗口共用 `VisPick.P0.Window`。设计文档规划的是 `VisPick.Main / Overlay / Pin / RecFrame / RecBar / Osd`。
- 影响：E2E 工具无法按类名定位参考图窗口；光标、类样式（如 `CS_DBLCLKS`）无法按窗口类型定制；窗口类名里有 `P0`，P1 以后需要改。
- 建议：`Window::Create` 增加类名参数（或模板参数），按类名懒注册。

### P-10 消息处理异常被静默吞掉

[Window.cpp:68-73](../../src/platform/Window.cpp#L68-L73) 的 `catch (...)` 之后只调用 `DefWindowProc`，没有任何记录。
- 影响：业务代码里抛出的异常（例如分配失败）不会留下痕迹，表现为“某个功能偶尔没反应”。
- 建议：至少调用一个全局的错误回调（写日志、计数）；Debug 构建里直接 `__debugbreak()` 或 `std::terminate()`。

### P-11 基类析构规则

[Window.cpp:10-12](../../src/platform/Window.cpp#L10-L12) `~Window()` 调用 `DestroyWindow`，但此时派生类部分已经析构，后续消息会被分发到基类的 `OnMessage`。现在每个派生类都在自己的析构里先调用 `Destroy()`，规则正确，但没有任何机制保证以后的派生类也这样做。
- 建议：在基类析构里增加 `assert(window_ == nullptr)`（Debug），或把 `Window` 设计成 `Create` 时就把窗口生命周期交给 `WM_NCDESTROY` 自行清理，派生类不再需要手动 `Destroy`。

### P-12 `dwmapi` 的两套用法

[Compat.cpp:153](../../src/platform/Compat.cpp#L153) 用 `LoadLibraryW` 动态加载 `dwmapi.dll`，而 [CMakeLists.txt:55](../../CMakeLists.txt#L55) 同时静态链接了 `dwmapi`，spike 和测试直接调用 `DwmFlush`。同一个 DLL 两种方式，维护者会困惑，也不利于导入白名单的一致性。
- 建议：统一。Vista 以上 `dwmapi.dll` 都存在，可以静态链接；仅对 Win8+ 才有的属性（如 `DWMWA_CLOAKED`）做运行时判断。

### P-13 `InspectGif` 要求所有帧同尺寸

[GifWriter.cpp:178-183](../../src/gif/GifWriter.cpp#L178-L183) 遇到与首帧尺寸不同的帧就抛出异常。P5 的差分帧是子矩形，这个检查必须放宽。同时，WIC 解码 GIF 得到的是未合成的原始帧，预览窗口需要自己做合成（设计文档已经考虑了），`InspectGif` 用于逐帧哈希比对时也需要先按帧位置和处置方式合成。
- 建议：现在就把 `InspectGif` 拆成“读取帧元数据”和“合成到画布”两步，并为差分帧、透明索引和 disposal 方式补单元测试。

### P-14 延时公式有两份

[GifWriter.cpp:56-63](../../src/gif/GifWriter.cpp#L56-L63) 的 `FrameDelay()` 有单元测试覆盖，但 [GifSpike.cpp:341](../../src/spikes/GifSpike.cpp#L341) 和 [GifSpike.cpp:364](../../src/spikes/GifSpike.cpp#L364) 用的是内联公式。测试验证的不是实际使用的代码。
- 建议：删除内联公式，统一调用同一个函数；P5 改成基于时间戳的版本后，把误差传递逻辑放进一个独立的、可测试的类里。

### P-15 LZW 编码器的小开销

[Lzw.cpp:43-51](../../src/gif/Lzw.cpp#L43-L51)：
- 编码前对所有像素扫描一遍做范围校验；
- 每帧分配并填充两个 8192 项的 `int` 表（64 KB）。

对 30 fps 来说开销不大，但可以优化：范围校验可以移到调试断言；字典表可以放在线程局部的编码器对象里复用，只在清除码处重置。

### P-16 没有 Git 提交

`git log` 显示当前分支没有任何提交，所有文件处于未跟踪状态。
- 影响：没有变更历史，无法回滚，也无法给 P0 打标签。
- 建议：立即提交一次初始版本，给 P0 打标签。注意 `.gitignore` 已忽略 `/dist/`、`/out/`，二进制交付物不入库，报告里的 SHA-256 可以保留作为记录。

---

## 6 设计层面的建议

### D-01 产品和 spike 拆开

[CMakeLists.txt:72-73](../../CMakeLists.txt#L72-L73) 中 `VisPick` 链接 `vispick_spikes`，spike 代码、测试图样窗口都编进了发布的 exe。
- 问题：体积、攻击面和用户可见的命令行（`--spike-*`）都不应该属于产品。
- 建议的目标划分：

| 目标 | 内容 |
|---|---|
| `vispick_core`（静态库） | 配置、命令行、位图、GDI 封装、`Window`、兼容层、GIF、采集 |
| `VisPick`（产品 exe） | `main.cpp` 与 P1 起的托盘、热键、浮层等产品代码 |
| `VisPickSpikes`（开发用 exe） | 四个 spike、测试图样窗口 |
| `VisPickTest`（开发用 exe） | E2E 自动化验收 |
| `VisPickUnitTests` | 单元测试 |

这样 `build.ps1` 的体积检查和导入检查只针对 `VisPick.exe`，结论才对用户有意义。

### D-02 重做 AC0-6 的测试方式

**问题**：[GifSpike.cpp](../../src/spikes/GifSpike.cpp) 在同一个循环里串行做：置顶、生成图样、绘制、`DwmFlush`、采集、逐像素校验、编码、写文件。其中图样生成逐像素做整数除法和取模（[GifSpike.cpp:78-79](../../src/spikes/GifSpike.cpp#L78-L79)），校验要遍历 92 万像素。这些都是测试自己的开销，把它们算进“节拍”之后，零丢帧在结构上就不可能。报告也承认了这一点，但验收标准仍然写着“零丢帧”。

**建议的结构**：

```
进程 A（图样服务）           进程 B（录制器 spike）                       离线校验
按自己的节奏动画     ─▶   采集线程：QPC 时间戳 + BitBlt + 差分     ─▶   录制结束后：
                          编码线程：只做编码和写文件                      逐帧解码并与
                                                                          已知图样比对
```

- 图样进程按帧序号和时间戳可预测地动画，录制结束后根据时间戳还原“当时应该显示的图样”，再与 GIF 逐帧比对。
- 录制循环里只有“采集 + 差分 + 入队”，丢帧率才反映真实的生产路径。
- 另外保留现在这种串行测试，用来做正确性（像素一致）验证，不再用它判定丢帧。

**验收标准**：新的 spike 在物理显示器上给出丢帧率、采集耗时分布、编码耗时分布，三者分开报告。

### D-03 帧延时应基于实际时间戳

[GifSpike.cpp:341](../../src/spikes/GifSpike.cpp#L341) 用名义节拍序号计算延时：`nextTick * 100 / fps - pendingTick * 100 / fps`。采集实际上是在截止时刻之后才发生的（`scheduler_lateness` 平均约 1 ms，最大约 2.4 ms），画面内容与时间轴会有偏差；若某帧采集迟到较多，播放时的节奏会失真，只是总时长恰好对得上。

**建议**：生产版用高精度时钟（QPC）记录每帧**采集完成**的时间，帧 i 的延时 = 时间戳(i+1) − 时间戳(i)，转换成 10 ms 单位时把舍入误差带入下一帧。设计文档里已经是这么规划的，P5 实现时不要沿用 spike 的写法。

### D-04 统一的“冻结图”所有权模型

现在采集结果在三处被各自拷贝：`CaptureSession::image_`、返回值、`OverlayWindow::frozen_`。进入 P2/P3 后，还有选区裁剪、标注渲染、复制、保存、钉住，拷贝只会更多。

**建议**：

```
FrozenScreen                         // 只读、引用计数
 ├─ HBITMAP / DIB + 像素视图         // 唯一一份
 ├─ 虚拟桌面原点和尺寸
 └─ 窗口列表快照
Overlay    持有 shared_ptr<FrozenScreen>，只读
Editor     持有同一份，标注作为矢量叠加
Output     按选区裁剪 → 新的小 Bitmap → 补 alpha → 编码
PinWindow  持有裁剪后的小 Bitmap
```

- 全屏大图只存在一份，浮层关闭即释放。
- x86 版尤其需要，地址空间有限。

### D-05 重绘时只回贴脏区域

[OverlaySpike.cpp:72-75](../../src/spikes/OverlaySpike.cpp#L72-L75) 用 `paint.Bounds()`（`rcPaint`，即更新区域的外接矩形）把后备缓冲回贴到窗口。大选区沿对角线移动时，更新区域是几条细长的“L 形”条带，外接矩形几乎覆盖整个选区，回贴时把没有变化的内部也拷了一遍。结果是正确的，但白白多出一次大块拷贝。
- 建议：用 `GetUpdateRgn` 取得区域，设置为 DC 的剪裁区后再 `BitBlt`；或者逐个遍历脏矩形回贴。
- 数据层面：先在 7680×2160 的物理显示器上测一次，再决定是否值得做。

### D-06 原型窗口的 `WS_EX_NOACTIVATE`

[OverlaySpike.cpp:36](../../src/spikes/OverlaySpike.cpp#L36)、[PinSpike.cpp:49](../../src/spikes/PinSpike.cpp#L49) 和 [VisualSupport.h:70](../../src/spikes/VisualSupport.h#L70) 都带有 `WS_EX_NOACTIVATE`。这对自动化测试是合理的（不抢焦点），但产品窗口不能这样：
- 浮层需要接收键盘（Esc、方向键、Enter、工具快捷键、文字输入法）；
- 参考图需要接收快捷键（设计中的 S02–S19）。

**建议**：产品窗口去掉 `NOACTIVATE`，浮层在热键响应里 `SetForegroundWindow`，参考图处理 `WM_MOUSEACTIVATE`。在 `Window` 基类中把“是否可激活”做成创建时的显式选项，避免原型的默认值被产品复用。

### D-07 事件驱动的等待

[GifSpike.cpp:142-147](../../src/spikes/GifSpike.cpp#L142-L147) 的 `WaitUntil` 是 `PumpWindowMessages` + `Sleep(1)` 的轮询。对 spike 无妨（`timeBeginPeriod(1)` 之后误差约 1 ms），但产品的消息循环不应该这样写（设计目标是空闲 CPU 为 0%）。

**建议**：
- 主消息循环用 `GetMessage`，或者 `MsgWaitForMultipleObjectsEx` 同时等待消息和事件；
- 录制采集线程用可等待定时器（Win10 1803+ 用高精度版本，旧系统用 `timeBeginPeriod(1)` + 普通定时器，已在设计里规划）；
- 把这个模式提前在 P1 的消息循环里写好，作为后续阶段的模板。

---

## 7 做得好的地方

- **诚实的报告**：没有为了“通过”而放宽阈值；零丢帧失败和 x86 中途不匹配都保留了原始记录，并明确区分了“实测”“推断”“未验证”。
- **工程基础**：
  - 工具集锁定（14.42），构建脚本会校验；
  - 导入白名单加 `dumpbin` 检查，阻止未经审查的 API 和外部运行库混入；
  - 构建选项齐全（`/W4 /WX /analyze /guard:cf`，x86 加 `/SAFESEH`）；
  - 单元测试 35 个、断言 35,783 个，加 ASan。
- **正确性意识**：
  - 像素级比较（用坐标编码的图样，任何偏移或缩放都会被发现）；
  - 编码后用独立的 WIC 解码并逐帧哈希核对；
  - GDI/USER 句柄增量检查；
  - 对旧系统的 `WDA_EXCLUDEFROMCAPTURE` 做了版本保护，避免黑块。
- **代码质量**：
  - RAII 封装统一（`UniqueGdiObject`、`ScreenDc`、`MemoryDc`、`DibSection`）；
  - 对尺寸溢出做了防护（`Bitmap`、`CreateBitmap`）；
  - Win32 回调里不让异常穿出。
- **算法**：LZW 与 Wu 的实现经过阅读，没有发现逻辑错误，并有往返测试覆盖。

---

## 8 建议的行动计划

### 8.1 进入 P1 之前（建议全部完成）

| 序号 | 事项 | 对应 | 说明 |
|---|---|---|---|
| 1 | 在本地控制台、物理显示器上重跑 `--spike-capture` 和 `--spike-gif` | P-01 | 决定 P5 是否需要 DXGI |
| 2 | 报告中增加会话类型、刷新率、显卡名称；无效环境标红 | P-01 | 避免再次拿远程会话的数据做结论 |
| 3 | 修正 Markdown 报告的数字精度并加单元测试 | P-08 | 小改动 |
| 4 | 拆分 CMake 目标：产品 exe 与 spike 分离 | D-01 | 进入 P1 前最合适 |
| 5 | 给 `Window` 增加类名参数；异常处理增加日志出口；明确析构规则 | P-09~P-11 | P1 起所有窗口都依赖它 |
| 6 | 提交 P0，打标签 | P-16 | |
| 7 | 修正 `AttachConsole` 后的控制台输出和编码 | P-07 | |

### 8.2 P2 之前

| 序号 | 事项 | 对应 |
|---|---|---|
| 1 | 去掉采集路径的重复拷贝，补 alpha 延后 | P-02 |
| 2 | 引入统一的“冻结图”对象，浮层改为单份缓冲加按需压暗 | P-03、D-04 |
| 3 | 在 7680×2160 的物理环境上测“热键到浮层出现”和重绘耗时 | AC0-4、AC0-5 |
| 4 | 去掉 `NOACTIVATE` 的默认值，浮层完成前台激活 | D-06 |

### 8.3 P5 之前

| 序号 | 事项 | 对应 |
|---|---|---|
| 1 | 重做 AC0-6 的测试结构（进程分离、时间戳、离线校验） | D-02 |
| 2 | GIF 写入器：按调色板大小写表、最小码长；支持子矩形帧、透明索引、disposal | P-04、P-13 |
| 3 | 量化器：小哈希表替换 `unordered_map`；调色板复用；真实内容基准 | P-05、P-06 |
| 4 | 帧延时改用实际时间戳，并合并成一份带测试的实现 | D-03、P-14 |
| 5 | 视 §2.4 的结果决定是否开发 DXGI 采集；若开发，使用其变化矩形 | P-01 |
| 6 | `InspectGif` 拆分为元数据读取与画布合成 | P-13 |

### 8.4 可以随手做的小改动

- 删除 `EncodeLzw` 开头的逐像素范围校验（改为断言），并把字典表放进可复用的编码器对象里（P-15）。
- 统一 `dwmapi` 的使用方式（P-12）。
- 事件驱动的消息循环模板在 P1 里一次写对（D-07）。
- 回贴脏区域的优化，等有双 4K 的实测数据再决定是否值得做（D-05）。

---

## 9 附录：评审方法与引用

### 9.1 评审了哪些文件

- 源码：`src/main.cpp`、`src/core/{CommandLine,Report}.cpp`、`src/platform/{Compat,Gdi,Window}.*`、`src/capture/ScreenGrab.*`、`src/imaging/Bitmap.cpp`、`src/gif/{GifWriter,Lzw,Quantizer}.*`、`src/spikes/{Capture,Overlay,Pin,Gif}Spike.cpp`、`src/spikes/VisualSupport.h`
- 测试：`tests/e2e/DesktopTests.cpp`
- 构建：`CMakeLists.txt`、`build.ps1`、`scripts/{Check-Imports,Enter-VsEnvironment}.ps1`、`res/VisPick.manifest`
- 文档与数据：`docs/reports/P0-2026-11-06.md`、`docs/WIN7_IMPORTS.md`、`docs/reports/data/*.json`

### 9.2 评审时的环境查询

- `Win32_VideoController`：NVIDIA GeForce RTX 5080（3840×2160，59 Hz）、Microsoft Remote Display Adapter（2560×1440，32 Hz）、Intel Graphics（3840×2160，59 Hz）；`SESSIONNAME=Console`。
- 数据来源：P0 报告的 `x64-Release.json` 和 `x64-Release-gif-final.json`。

### 9.3 评审的边界

- 没有运行程序，也没有改动代码。所有关于耗时的判断都来自现有报告数据和源码阅读。
- 凡是写了“推断”“推测”“可能”的地方，都需要用 §2.4 或各条目的“验收标准”去验证。
- 没有评审单元测试内部（`tests/unit`）的覆盖细节，也没有评审 `doctest` 第三方文件。
- Win7 的运行时行为仍然没有任何实机数据，这一点与 P0 报告一致。
