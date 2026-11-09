# P0 手工验收清单

日期：2026-11-06。按 [DESIGN.md](DESIGN.md) 的 P0 标准执行。修正后自动化结果见 [评审修正记录](reviews/P0-corrections-2026-11-06.md)；[原始报告](reports/P0-2026-11-06.md) 保留历史数据及结论撤回说明。

## 已自动验证的项目

- [x] `build.ps1 -Analyze` 在 VS2022 / MSVC 14.42 下产出 x64 / x86 Release。
- [x] 静态运行库、manifest、版本资源、图标已嵌入；静态导入满足审查白名单。
- [x] Bitmap、LZW、精确调色板、Wu 量化、GIF 延时与 WIC 解码等单元测试。
- [x] 产品与原型入口拆分；开发工具提供截图、浮层、贴图、GIF 正确性、独立流水线及量化对照实验。
- [x] 检测会话、显示适配器、刷新率及物理输出；无效环境不能验收通过。
- [x] 控制台、文件、管道、无控制台启动的输出回归检查。

## 需要在实际目标环境继续执行

1. 在本机双 4K、175% 下执行：

   ```powershell
   .\out\build\x64-Release\VisPickTest.exe --all --output .\out\reports\dual-4k-175
   ```

   核对 `remote_session=false`、`acceptance_eligible=true`、两台物理显示器的名称/边界/DPI/刷新率确实对应目标配置；不能用 32 Hz 远程 1440p 单屏结果替代双 4K 验收。两个架构及其它性能任务串行运行。

2. 分别设置副屏在主屏左方、上方以及混合 DPI 后，重复 `--desktop-tests`；要求 `mismatched_pixels=0`、`window_bounds_match=1`，GDI/USER 增量均在 ±5 内。测试只读显示设置，不会自行修改它们。

3. 在纯净 Win11、Win10 和 Win7 SP1 虚拟机/设备中，只复制对应产品 exe，双击能出现帮助。开发验证另复制同架构的 `VisPickSpikes.exe` / `VisPickTest.exe` 后运行实验；流水线的图样子进程要求两个开发 exe 同目录。P0 不生成 INI，首次运行配置属于 P1。虚拟显示结果只检查功能，不作为物理显示性能验收。

4. 在 Chrome/Edge 和 Windows“照片”中打开生成的 `gif-spike.gif` / `gif-pipeline.gif`，核对能循环播放、尺寸为 1280×720、播放时长与报告相符。WIC 解码通过不等同于这些应用已验证。

5. 核对 `--spike-overlay` 的平均绘制 ≤5 ms、最大值 ≤16 ms、三份整屏缓冲、关闭后私有提交内存增量在 ±5 MiB；在双 4K 上记录初始化时延。P0 无真实热键，最终热键到可见时延须在 P1/P2 接入后验证。

6. `--spike-capture` 对照 320×240 和 1920×1080 的 BitBlt/GdiFlush 分布及 1000 次采集内存稳定性；只有有效物理环境中的 1080p 数据才用于 AC0-7 的 15 ms 判据。

7. `--spike-gif` 仅验证像素/实际时间戳；30 fps 检查看 `--spike-gif-pipeline` 的 `schedule_skipped_ticks=0`、`queue_full_skipped_ticks=0`、`rejected_unknown_or_torn_frames=0`、`decoded_mismatched_frames=0`、`duration_error<1`。同时审查重复/跳过源帧 ID 和源快照变化，区分捕获调度与显示合成问题，不能用时长正确替代帧率验收。

## 当前未通过/未验收

- [ ] AC0-3：纯净系统运行。
- [ ] AC0-4、AC0-5：指定 7680×2160 真实桌面配置。
- [ ] AC0-6：30 fps、10 秒完全无丢帧；浏览器和照片应用播放。
- [ ] AC0-7：在有效物理输出上重新判断 BitBlt / DXGI，旧结论已撤回。
- [ ] AC0-8：技术报告评审确认。

下一阶段不据此宣称全部兼容或 30 fps 性能达标。真实内容量化、Win7、双 4K 和物理帧率以独立证据验收。
