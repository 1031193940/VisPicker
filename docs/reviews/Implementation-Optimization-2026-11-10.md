# 审查建议实施记录

基线为已发布的 0.5.11（产品提交 `d500e22`，发布提交 `1431931`）。本轮版本为 0.6.0。

## 已确定的产品取舍

- 用户确认默认恢复错误通知，成功操作保持静默。
- 保留 x64/x86 和现有旧系统兼容目标，未验证的系统继续明确标注。
- 保留用户指定的 Ctrl+1 / Ctrl+2 默认热键，托盘显示配置中的实际组合。
- 0.5.11 已发布并有标签，评审 E-03 的这一部分已解决。

## 实施与验证任务

- [x] 1. 失败反馈和浮层异常恢复（F-01/F-02）→ 通知配置、热键冲突诊断、保存失败、异常取消资源释放已覆盖；系统通知呈现的手工验收见下方限制。
- [x] 2. 工具链选择和 CI（T-01/E-01）→ 双架构构建/分析/单元/导入/体积及脚本检查通过；GitHub 线上工作流尚未运行。
- [x] 3. 键盘选区、颜色复制和 DPI（U-02～U-05/A-03）→ 纯逻辑与私有剪贴板通过；x64 选区全流程通过，x86 前景中断单独记录。
- [x] 4. GIF 暂停及首尾裁剪（U-07）→ 暂停、首帧合成、可变延时、disposal、裁剪保存/复制和失败保留源文件通过。
- [x] 5. 剪贴板响应和配置更新（F-03/A-04）→ 异步 PNG、剪贴板所有权、编码/注释保留及旧配置兼容通过。
- [x] 6. 工程边界和界面资源（A-06/A-07/A-08）→ 检测代码分离、146 文件格式检查、图标 DPI/降级/资源测试通过。
- [x] 7. 现有环境回归与建议取舍（T-02/T-04、其余建议）→ 已记录通过、未达标、环境前提失败和后续方案。
- [x] 8. 用户/开发文档、变更记录及便携包（E-03）→ 本地双架构包生成，归档内 exe 的尺寸、SHA256 和源码指纹逐项核对通过；尚未推送或发布。

任务 3、4 依赖现有图标接口；任务 5 在基础反馈策略就绪后接入 App；任务 7、8 在源文件稳定后执行。

## 验证结果（2026-11-10）

功能提交 `9f153ab`；独立格式提交 `88fa568`。最后构建使用 MSVC 14.42.34433，源文件指纹为 `BA123DE5B15A5676581299F0EAD6BBB1EAADB07C5603B619F3DC2FA58A3DB726`。

| 检查 | x64 | x86 |
|---|---|---|
| Release `/W4 /WX /analyze` | 通过 | 通过 |
| 单元测试 | 321/321，58,060 个断言 | 321/321，58,060 个断言 |
| 产品导入白名单 | 339 符号 / 11 系统 DLL | 330 符号 / 11 系统 DLL |
| 产品体积 | 866,816 bytes | 698,880 bytes |
| 输出 / 私有剪贴板 | 通过 | 通过 |
| 标注 / 参考图 | 通过 | 通过 |
| 录制 / 暂停 / 预览裁剪 | 34 项通过 | 34 项通过 |
| 常驻框架 | 40 项通过；未跑十分钟空闲 | 40 项通过；未跑十分钟空闲 |
| 控制台 / 文件 / 管道 / 无控制台 | 通过 | 通过 |
| 独立选区测试 | 完整通过；100 次取消 GDI +0 / USER +1 | 双屏像素、工具条及 GIF 帧率检查通过；后续前景激活被拒绝，整体未通过 |
| 前景拒绝夹具 / 跨进程生命周期 | 未通过前景权限前提 | 未通过前景权限前提 |

截图后台 PNG 回归验证了即时 DIBV5 alpha、200 次同步复制兼容、32 次连续复制合并、同 owner 后续文本不被旧 PNG 覆盖、退出 join。真实测试发现并修复 `CloseClipboard` 合成格式后序列号变化的问题：在关闭后采样并核对所有者，补发时在锁内再次验证。

INI 更新测试覆盖最后一个重复键、注释、Unicode、UTF-8/UTF-16/旧编码、只读失败、空值及注释歧义。写盘前重新解析候选内容，不能准确表示请求值时保留原文件。GIF 裁剪验证了 Previous/Background disposal、最小 20 ms 延时、取消和失败保留源文件；未裁剪仍保留原文件字节。

原始日志和图样位于本地 `out/reports/review-0.6.0/`，构建日志为 `out/review-0.6.0-build-verified.log`，格式检查为 `out/review-0.6.0-format-check-final.log`。不把原始数据继续加入 Git。

### 前景限制与对照

前景锁定夹具的 `LockSetForegroundWindow` 返回 `ERROR_ACCESS_DENIED (5)`，当时前景 PID、窗口 owner PID 与测试 PID 相同。跨进程选区首次拖选处于等待激活状态，未形成选区；新增诊断明确区分前景前提失败和贴图结果失败。

0.5.11 对照曾完整通过，随后也在自有测试窗口激活前失败。因此这里没有足够证据判为本轮产品回归，也不宣称生命周期验收通过。禁用截图 worker / 预热 GDI+ 的临时定位改动均已撤回；没有修改产品焦点保护来迁就测试。测试现在只在缺少前景时重新通过自有窗口请求一次合法授权，并在拖动前检查授权实际生效；失败照常返回非零。

### 物理显示与性能

本次环境为 Windows 11 build 26200、RTX 5080、两块 3840×2160、175% DPI、60 Hz 显示器，非远程会话，物理输出资格通过。`VisPickTest --all` 的实际结果：

- 全桌面与 100 次生命周期共比较 24,268,800 像素，零差异，GDI/USER 增量均为 0；整桌面分配及采集 208.13 ms，超过旧 150 ms 门槛，该项整体未通过。
- 1080p CAPTUREBLT P95 17.80 ms；1000 次小区域复用采集私有提交增量 0。满足启动现代后端实验的条件，尚不等于后端已经替换。
- 旧浮层原型平均重绘 2.85 ms、最大 32.73 ms，最大值超过 16 ms；关闭后内存回落。该原型使用三张全屏图，并非当前产品 Overlay 的测量。
- GIF 正确性图样第 54 tick 失配，首次差异命中 `VirtualDesktopHotkeySwitcher`，是切桌面/覆盖干扰线索，不单凭命中测试断言根因。
- GIF pipeline 捕获 300 帧，拒绝 1 个未知/撕裂/被覆盖帧；其余 299 帧解码零差异，时长误差 0.031%，无调度/队列跳帧。由于拒绝帧数要求为零，此项未通过。
- Pin 和量化实验通过。没有降低性能门槛或把未通过改写成通过。

仍需对应环境实际验收：Win7/Win10/干净系统、混合 DPI/HDR、前景权限受控的完整生命周期、三种错误通知的系统实际呈现（只读保存/热键冲突/录制磁盘写满）、第三方粘贴与真实输入法。八小时浸泡仍不是发布门槛。

### 本地交付

- 分支：`codex/review-optimization-0.6.0`；功能与格式分开提交，工作区干净。此次没有推送 GitHub、打标签或创建 Release。
- 便携包：`dist/VisPick-0.6.0.zip`，含 x64/x86 exe、对应版本说明、配置模板、MIT LICENSE、CHANGELOG 与 manifest。
- SHA256：`2836D6DC213B8984636067BFFCE45032B9B92C87E2B6BA678168A44B9522F315`；旁置 `dist/VisPick-0.6.0.zip.sha256`。
- manifest 的打包提交为 `86af424`，产品源码为 `88fa568`；后续提交仅补充此交付记录。归档内两份 exe 与已验证的构建清单一致。

## 验收边界

当前没有 Win7/Win10 虚拟机实机结果，不把导入检查当作兼容性通过。长期采集后端替换、完整标注对象编辑、跨会话参考图持久化和签名服务需要分别评估，不能仅凭静态评审直接声明完成。

## 建议逐项处理

| 建议 | 本轮处理 |
|---|---|
| F-01 | 默认 `Notify=errors`，成功静默，失败走托盘提醒；启动汇总错误，重复启动重放提醒。支持 `all/none`。GIF 错误保留内嵌状态。系统勿扰和气泡实际呈现不由程序保证。 |
| F-02 | 异常消息带失败窗口 HWND；全屏选区或其子窗口异常时先取消并释放捕获，再报告错误。 |
| F-03 | 截图立即发布 DIBV5/DIB，单个后台线程编码 PNG。只保留最新待处理图片，用所有者和序列号阻止过期结果覆盖；参考图复制保留原同步路径。 |
| T-01 | 允许 14.42–14.49、优先已安装最高版本，推荐 14.44；本机编译器仍为 14.42。 |
| T-02 | 按用户选择保留 x64/x86 与旧系统目标；缺少 Win7/Win10 实际环境，不声明兼容性通过。 |
| T-03 | 保留 BitBlt。现代采集及 HDR 作为独立后端实验，见下方条件；本轮没有用未经验证的实现替换稳定采集链路。 |
| T-04 | 执行当前环境可运行的桌面检查，环境资格与功能结果分别记录，不伪造物理显示或虚拟机结果。 |
| U-01 | 沿用用户明确指定的 Ctrl+1/Ctrl+2；现有 INI 支持改键和禁用，错误提示使注册冲突可见。没有另加首次启动向导。 |
| U-02 | 托盘菜单显示当前配置的真实热键，热重载同步更新。 |
| U-03 | Ctrl+A 当前显示器、Tab/Shift+Tab 顶层窗口候选、Ctrl+方向键尺寸调整。子控件识别暂不加入：并非所有框架暴露 HWND 控件，需确定 UIA/窗口命中规则后单独实现。 |
| U-04/U-05 | C/Shift+C 复制冻结像素 HEX/RGB；放大镜固定 15×15 格，单格按 DPI 缩放。 |
| U-06 | 保留现有六种标注。本轮未加入对象拾取/移动/删除与新标注类型；这些能力会改变对象身份、命中优先级和撤销模型，需作为独立编辑器功能验证，不能算成已完成。 |
| U-07 | 录制暂停/继续；预览按帧设置首尾、重置、选段循环，保存和复制均作用于选段。MP4/WebP 未加入。 |
| U-08 | 保持当前“退出后不保留参考图”行为；可选本地持久化的后续方案见下文。 |
| A-01/A-02 | 暂不统一改写任务调度与 App 状态机。新增剪贴板任务有界且封装生命周期；全局架构调整需要先覆盖模态重入、退出与取消转换，另行完成，不能用局部封装冒充完成。 |
| A-03 | 抽出不依赖窗口句柄的候选循环、尺寸调整、放大镜布局和颜色格式并单测；未重写整个 Overlay 状态机。 |
| A-04 | 实现 `UpdateIniValue` 原子通用写入、编码/注释保留，`CaptureLayeredGif` 新键兼容旧名。描述表驱动的解析/模板生成未实施，避免在添加少量配置时整段替换成熟解析器；现有嵌入模板一致性测试继续把关。 |
| A-05 | 字符串表/全局字体解析尚未实施；保留现有双语行为，后续与本地化需求一起整理。 |
| A-06 | 验收环境检测迁移至 `vispick_spikes`，产品 `Compat` 只保留运行时需要的显示几何、DPI 和能力。 |
| A-07 | clang-format 18.1.8 配置和检查脚本；格式修改独立于功能提交，CI 检查自有 C++。 |
| A-08 | 共享 GDI+ 生命周期，图标抗锯齿；失败释放 Graphics 后回退 GDI。 |
| E-01 | 新增双架构 Release/分析/单元测试/导入/体积 CI；标签触发打包与 SHA256/Release。已有 Release 不覆写。尚未推送运行 GitHub CI。 |
| E-02 | 后续原始数据保留在 `out/`/CI 产物，仓库留摘要。已有宣传媒体按用户先前要求继续保留；不改写历史或突然引入 LFS 下载依赖。仅删除当前文件无法缩小既有历史。 |
| E-03 | 0.5.11 已有标签与 Release；README 仅描述公开版，构建与验收迁移到 DEVELOPMENT；0.6.0 说明单独存放，包内使用对应版本说明。 |
| E-04 | 完成签名和分发渠道评估，未申请账号、购买服务或向外部仓库提交软件包，见下文。 |

## 后续方案与依据

### 采集后端与 HDR

保留旧系统时，新后端必须按运行时能力加载，不能新增会阻止 Win7 启动的静态导入。WGC 基础能力始于 Win10 1803，但本工具从 HMONITOR 创建采集项的 [CreateForMonitor](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createformonitor) 需要 1903。光标开关需要 [2004](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.iscursorcaptureenabled?view=winrt-26100)；[边框开关](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired) 有更高系统要求和授权条件，便携版不能直接承诺无边框。

[DXGI DuplicateOutput](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutput1-duplicateoutput) 在 Win8+可用，Win7 Platform Update 返回 E_NOTIMPL；多显示器还需处理各输出的帧时钟。HDR 需 FP16/scRGB 与明确的 HDR→SDR 映射，再进入现有 BGRA/GIF 管线，不能靠换 API 解决偏色。[微软采集说明](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture)

本轮已获得本地双 4K/175%/60 Hz 证据：1080p CAPTUREBLT P95 17.8 ms，超过设计的 15 ms 后端调研触发线。下一步后端实验已具备实测理由；仍需覆盖设备丢失、锁屏、多屏、混合 DPI、HDR 与 x86 内存峰值，再决定运行时优先顺序。这个结论替代过去“只有 32 Hz RDP、无法判断”的状态。

### 可选参考图持久化

后续采用默认关闭的本地存储：`%LOCALAPPDATA%\VisPick\session` 中保存 PNG 与版本化清单，原子更新，按原图字节数/条数设预算，恢复时按当前显示器重新约束位置。不要直接序列化 C++ 内存或把虚拟桌面 COM 身份当作跨重启稳定标识。不需要为这项功能引入数据库或云同步。[Known Folder 定义](https://learn.microsoft.com/en-us/windows/win32/shell/knownfolderid)

### 签名与分发

优先考虑申请 [SignPath Foundation](https://signpath.org/terms.html)：MIT 符合开源许可方向，但活跃项目、可验证 CI、团队 MFA、批准角色与签名政策仍需正式审核。申请不等于获批。

微软服务现名 Azure Artifact Signing，需要付费订阅和身份验证，地区资格有限制；新签名也不能保证立即消除 SmartScreen 提示，旧系统还需信任链/SHA-2/时间戳验证。[微软签名选择](https://learn.microsoft.com/windows/apps/package-and-deploy/code-signing-options) · [Artifact Signing FAQ](https://learn.microsoft.com/en-us/azure/artifact-signing/faq)

分发可先准备 [Scoop manifest](https://github.com/ScoopInstaller/Scoop/wiki/App-Manifests)，复用现有双架构 ZIP 和哈希；Win7 还需 PowerShell 5.1。winget 客户端要求 Win10 1809+，可在有稳定 Release URL 和 SHA256 后再提交便携 ZIP 清单。[winget 文档](https://learn.microsoft.com/en-us/windows/package-manager/winget/)
