# 截图标注二次编辑

## 目标

已有标注可以再次选中、移动、修改适用属性；矩形和椭圆可拖动手柄调整大小，箭头可调整端点，文字可重新输入。编辑支持撤销重做，属性栏只显示当前工具或选中对象适用的选项。

## 任务

- [x] 1. 扩展标注模型的命中、选择和编辑历史 → 新增 13 项单测，覆盖几何命中、移动缩放、属性变更、撤销重做与分支历史。
- [x] 2. 支持原生文字重编辑（依赖 1）→ 双架构隐藏窗口检查通过，覆盖原文、软折行、字号、颜色、取消恢复和合成 IME 延迟提交。
- [x] 3. 接入鼠标操作和组件属性栏（依赖 1、2）→ 代码及静态分析通过；形状显示颜色/线宽，文字显示颜色/字号，马赛克显示块大小。真实窗口验证见任务 4。
- [ ] 4. 运行原生窗口集成检查（依赖 3）→ 用例已新增并编译；桌面会话断开，尚未运行拖动/属性栏/导出交互及视觉截图检查。
- [ ] 5. 完成验收（依赖 4）→ 双架构构建和开发版产物已完成；真实桌面交互检查待恢复会话。

## 交互

- 点击已有标注的轮廓选中并拖动；空心形状内部仍可绘制其它标注。
- 矩形/椭圆使用八个缩放手柄，箭头使用两个端点；文字双击或选中后按 Enter 重新编辑。
- 点击工具切换回新增模式；Esc 先取消当前拖动或组件选择，再退出截图。
- Ctrl+Z / Ctrl+Y 撤销重做，Delete 删除选中组件。
- 选择框仅显示在编辑界面，复制、保存、贴图均输出实际内容。

## 完成标准

- [ ] 上述交互及按对象显示属性均可用。
- [ ] 现有新增标注、截图选区、复制/保存/贴图不回归。
- [x] 验证结果据实记录，不将未运行检查写成通过。

## 2026-11-14 验证与交付

源码版本为 0.7.0，分支 `codex/annotation-editing`，尚未合并到 main 或发布到 GitHub。

| 检查 | x64 | x86 |
|---|---|---|
| Release `/W4 /WX /analyze` | 通过 | 通过 |
| 非桌面单元测试 | 332 项、58,152 断言通过 | 332 项、58,152 断言通过 |
| 原生文字像素比较 | 600,600 像素零差异 | 600,600 像素零差异 |
| 文字编辑资源循环 | 200 次，GDI/USER 无增长 | 200 次，GDI/USER 无增长 |
| 导入白名单 | 339 符号 / 11 系统 DLL | 330 符号 / 11 系统 DLL |
| exe 大小（2 MiB 上限） | 897,024 bytes | 727,040 bytes |

文字检查通过隐藏的原生 EDIT 和 `WM_PRINTCLIENT` 验证，无需读取用户桌面；覆盖文字替换、字号/颜色重新排版、取消、删除恢复、原始 Unicode/硬换行/软折行、合成 IME 提交。真实中文输入法操作仍待人工验证。200 次循环私有提交：x64 无增长，x86 增加 159,744 bytes；这不是长期稳定性结论。

首次完整 x64 单测为 332/334，通过的断言无失败；两项原有采集测试因 BitBlt 句柄无效/访问拒绝失败。只读诊断确认执行进程位于已断开的远程 Session 1（`WTSDisconnected`），活动控制台为 Session 4，`OpenInputDesktop` 返回错误 5。后续非桌面验证明确排除这两项，没有改动测试实现或放宽断言。Overlay 集成检查和界面截图尚未运行，不能宣称桌面操作已经验收。

开发版：`dist/VisPick_0.7.0.exe`（x64）。

- SHA256：`DA1D54CB5833F56BFD29231D8A1A58612ECF3752028EDA630B7B5B3A024733F4`
- 源码指纹：`A289CE9ED972FD721C094C187A2436EE3C3CE370CAE3610BE4358B854A815B73`
- 构建/单测日志：`out/annotation-editing-build-*.log`、`out/annotation-editing-unit-*-nondesktop.log`
- 文字报告：`out/annotation-editing-editor-x64.json`、`out/reports/annotation-editing/editor-x86.json`
- 格式检查：146 个项目文件通过 clang-format 18.1.8；`git diff --check` 通过。

恢复开发进程所在桌面会话并解锁后，继续执行（两架构窗口测试应串行）：

```powershell
.\out\build\x64-Release\VisPickOverlayTests.exe --annotations-only --visual-output out/reports/annotation-editing/visual-x64
.\out\build\x86-Release\VisPickOverlayTests.exe --annotations-only --visual-output out/reports/annotation-editing/visual-x86
.\out\build\x64-Release\VisPickOverlayTests.exe --selection-only
.\out\build\x64-Release\VisPickUnitTests.exe --no-colors
.\out\build\x86-Release\VisPickUnitTests.exe --no-colors
```

视觉输出为 `shape-properties.png` 与 `text-properties.png`，用于检查组件手柄和属性栏；集成用例同时对比导出像素，检查选择框不进入输出。
