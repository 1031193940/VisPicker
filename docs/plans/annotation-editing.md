# 截图标注二次编辑

## 目标

已有标注可以再次选中、移动、修改适用属性；矩形和椭圆可拖动手柄调整大小，箭头可调整端点，文字可重新输入。编辑支持撤销重做，属性栏只显示当前工具或选中对象适用的选项。

## 任务

- [x] 1. 扩展标注模型的命中、选择和编辑历史 → 新增 13 项单测，覆盖几何命中、移动缩放、属性变更、撤销重做与分支历史。
- [x] 2. 支持原生文字重编辑（依赖 1）→ 双架构隐藏窗口检查通过，覆盖原文、软折行、字号、颜色、取消恢复和合成 IME 延迟提交。
- [x] 3. 接入鼠标操作和组件属性栏（依赖 1、2）→ 形状显示颜色/线宽，文字显示颜色/字号，马赛克显示块大小；输入框增高时工具栏自动避让。
- [x] 4. 运行原生窗口集成检查（依赖 3）→ 双架构拖动/属性栏/导出交互通过；检查真实 HWND 命中、输入焦点、工具切换与界面截图。
- [x] 5. 完成当前环境验收（依赖 4）→ 双架构完整单测、构建与开发版产物完成；x64 原有截图流程及 100 次采集/取消循环通过。未覆盖环境列于下文。

## 交互

- 点击已有标注的轮廓选中并拖动；空心形状内部仍可绘制其它标注。
- 矩形/椭圆使用八个缩放手柄，箭头使用两个端点；文字双击或选中后按 Enter 重新编辑。
- 点击工具切换回新增模式；Esc 先取消当前拖动或组件选择，再退出截图。
- Ctrl+Z / Ctrl+Y 撤销重做，Delete 删除选中组件。
- 选择框仅显示在编辑界面，复制、保存、贴图均输出实际内容。

## 完成标准

- [x] 上述交互及按对象显示属性均可用。
- [x] 现有新增标注、截图选区、复制/保存/贴图流程回归通过。
- [x] 验证结果据实记录，不将未运行检查写成通过。

## 2026-11-14 验证与交付

本次验证对应 `v0.7.0` 的源码和发布文件。标注二次编辑在 `codex/annotation-editing` 分支开发。

| 检查 | x64 | x86 |
|---|---|---|
| Release `/W4 /WX /analyze` | 通过 | 通过 |
| 完整单元测试（含桌面采集） | 341 项、58,238 断言通过，无跳过 | 341 项、58,238 断言通过，无跳过 |
| 原生文字像素比较 | 600,600 像素零差异 | 600,600 像素零差异 |
| 文字编辑资源循环 | 200 次，GDI/USER 无增长 | 200 次，GDI/USER 无增长 |
| 标注窗口交互与属性栏命中 | 通过 | 通过 |
| 导入白名单 | 339 符号 / 11 系统 DLL | 330 符号 / 11 系统 DLL |
| exe 大小（2 MiB 上限） | 898,560 bytes | 728,576 bytes |

文字检查通过隐藏的原生 EDIT 和 `WM_PRINTCLIENT` 验证；覆盖文字替换、字号/颜色重新排版、取消、删除恢复、原始 Unicode/硬换行/软折行、合成 IME 提交。新增限高长文字回归：滚动输入、制表符位置、再次编辑与撤销的导出像素均与不限高版本一致。200 次循环私有提交：x64 增加 651,264 bytes，x86 增加 577,536 bytes；此项只记录结果，不作为长期稳定性结论。

本轮桌面环境为 Windows 11 远程会话、单显示器 2560×1440、125% 缩放。恢复前会话断开，以及中途会话仍 Active 但无前台窗口/黑屏时，采集曾报 BitBlt 句柄无效或访问拒绝；恢复可见远程桌面后，上表完整测试均通过，没有排除采集用例或放宽断言。

补充检查发现全屏底部的多行 EDIT 会覆盖父窗口绘制的工具栏。保留的旧程序在字号按钮位置命中了 EDIT，复现日志为 `out/annotation-toolbar-before-repeat.log`。修复后工具栏优先避让输入框；四周无可用空间时停靠显示器底部并限制输入视口，保留滚动及完整导出布局。七项布局单测覆盖四方向、负原点、缩放和文字位于新裁剪框上方的边界。双架构回归检查了窗口命中、工具栏实际绘制像素、改字号后焦点保留、鼠标切换工具和再次编辑。

x64 `--selection-only` 同时通过原有截图选区、Copy/Pin/Cancel/Save 动作、GIF 帧率选择及模式切换；100 次采集/取消循环 GDI/USER 均无增长，64×48 采集像素完全一致。这里的 Save/Copy/Pin 断言验证 Overlay 返回动作和图像，不替代完整应用的保存对话框/剪贴板/参考图端到端验收。

未覆盖：真实中文输入法人工操作、强制拒绝前台激活夹具、物理多屏/混合 DPI，以及 Win7/Win10 实机兼容。本轮没有运行八小时测试，也不将其作为发布门槛。

发布文件为 `dist/VisPick_0.7.0.exe`（x64）。

- SHA256：`81AB7EEA1FB59FC215DDB71C30D0E83CDB8D0F4973C629A3D7C5A25C91D4180C`
- 源码指纹：`1609248F054A982F1B10D10C7B8D9CF85705243AD433B1F9EEB02C1F322CD031`
- 构建日志：`out/annotation-toolbar-build-all.log`；完整单测明细：`out/build/{x64,x86}-Release/Testing/Temporary/LastTest.log`
- 窗口日志：`out/annotation-toolbar-overlay-{x64,x86}.log`、`out/annotation-toolbar-selection-x64.log`
- 文字报告：`out/annotation-editing-editor-x64.json`、`out/reports/annotation-editing/editor-x86.json`
- 格式检查：146 个项目文件通过 clang-format 18.1.8；`git diff --check` 通过。

复测命令（保持远程桌面可见且解锁，两架构窗口测试串行）：

```powershell
.\out\build\x64-Release\VisPickOverlayTests.exe --annotations-only --visual-output out/reports/annotation-editing/visual-x64
.\out\build\x86-Release\VisPickOverlayTests.exe --annotations-only --visual-output out/reports/annotation-editing/visual-x86
.\out\build\x64-Release\VisPickOverlayTests.exe --selection-only
.\out\build\x64-Release\VisPickUnitTests.exe --no-colors
.\out\build\x86-Release\VisPickUnitTests.exe --no-colors
```

视觉输出位于 `out/reports/annotation-editing/visual-{x64,x86}/`，包含 `shape-properties.png`、`text-properties.png`、`text-editing-properties.png` 与 `text-editing-large-font.png`，均只采集测试夹具自绘内容。已目检组件手柄、按类型显示的属性和多行输入框旁的工具栏；集成用例同时检查选择框、重复文字不进入导出图像。
