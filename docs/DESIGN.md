# eTools 设计文档

> 阶段：P0（工具链与渲染闸门）完成。本文记录已生效的架构决策、P0 实测结论与视觉 token，后续阶段在此累积。

## 1. 目标与约束

- Windows 10/11 x64，免安装、免管理员、单 exe，常驻托盘。
- 热键→首帧 ≤50ms；每键查询 ≤16ms（文件源 1 字符 ≤40ms）；空闲内存 ≤80MB；exe ≤4MB。
- 界面按明确 token 设计，可用离屏 PNG 逐轮审查，不靠"凭感觉调样式"。

## 2. 技术决策

| 决策 | 选择 | 理由 |
| --- | --- | --- |
| 语言/UI | C++20 + 自研 Win32 组件 | 视觉完全可控；本机工具链最轻；编译迭代 1–3s |
| 工具链 | MinGW-w64 GCC 16.2（UCRT, posix-seh），静态链接 | 免管理员、免 Windows SDK；MSVC 需 C++ 工作负载且本机不可用 |
| 图形 | D3D11 + DXGI 翻转模型**合成**交换链 + DirectComposition + Direct2D 1.1 DeviceContext | 逐像素 alpha、圆角/半透明、平滑动画；DeviceContext 可绑定 WIC/纹理做离屏自检 |
| 文本 | DirectWrite（`Segoe UI Variable Text`，灰度抗锯齿） | 中文回退与字重控制；透明背景必须灰度 AA |
| 窗口 | `WS_POPUP` + `WS_EX_NOREDIRECTIONBITMAP|TOOLWINDOW|TOPMOST` | 无 GDI 重定向表面的合成窗口 |
| 背景 | 首选 DWM 云母（`DWMWA_SYSTEMBACKDROP_TYPE`），回退自绘 | 原生 Win11 观感；云母不可用时用实色/渐变 |
| 配置 | 自写极简 JSON | 零第三方依赖 |
| 排除 | C#/WPF、Qt、Rust/Tauri、Rust/Slint | 与体积/启动/授权目标冲突（C# 由需求方明确排除） |

### 2.1 支持范围（已定：Win11 22H2+ / x64）

- 仅支持 **Windows 11 build 22621（22H2）及以上，x64**；不提供 Win10 视觉回退。
- 云母（`DWMWA_SYSTEMBACKDROP_TYPE`）与圆角（`DWMWA_WINDOW_CORNER_PREFERENCE`）视为硬依赖，因此不写 Win10 分支，减少测试面。
- 落地要求：
  1. 启动时用 `RtlGetVersion` 取 build，`< 22621` 弹 MessageBox 提示"需要 Windows 11 22H2 及以上"并退出；
  2. 工具链固定在已验证 tag（`16.2.0posix-23.1.3-14.0.0-ucrt-r3`），重建不漂移；
  3. 正式版用 GUI 子系统（`-mwindows`），清单保留 `asInvoker`/Per-Monitor V2/长路径/UTF-8。
- 兼容性说明（对其他 PC）：静态链接、零外部运行库；不依赖 VC++ Redist/.NET；ARM64 仅能靠 x64 模拟，无原生构建。

## 3. 构建链路

1. `node tools/fetch-toolchain.mjs`：经 `api.github.com` 资源端点下载 winlibs（264MB），用 GitHub 资产 `digest`（sha256）校验，解压到 `.toolchain/mingw64`。
2. `build.ps1`：按源文件增量编译（头文件 mtime 参与判断）→ `windres` 编 `assets/app.rc`（清单 ID 1、版本信息）→ `g++ -static -s` 链接。
3. 链接库：`d2d1 dwrite d3d11 dxgi dwmapi dcomp windowscodecs ole32 uuid oleaut32 shell32 shlwapi propsys uxtheme comctl32 gdi32 imm32 shcore version`。
4. 产物：`build/eTools.exe`，当前 280KB。

## 4. P0 实测结论（踩过的坑）

1. **DComp 透明合成成立**：`CreateSwapChainForComposition`（`DXGI_ALPHA_MODE_PREMULTIPLIED`）+ `DCompositionCreateDevice/TargetForHwnd/Visual/Commit` 后，面板 alpha=0.35 时桌面内容清晰透出；alpha 区域与圆角均正确。
2. **云母在 DComp 窗口上生效**：`DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, DWMSBT_MAINWINDOW)` 返回 `S_OK`，且截图中桌面文字不再透出，呈现壁纸模糊底。注意：系统云母是**壁纸级**模糊，不是"窗口后内容"模糊；若要做 Raycast 式实时模糊，需要 `DCompositionCreateBackdropBrush` 或"显示前抓屏 + 高斯模糊"的快照方案（P2 决定）。
3. **D2D 必须 `BeginDraw`/`EndDraw`**：不配对时所有绘制被丢弃，纹理保持全透明（表现为"导出空白 PNG"）。
4. **WIC 会改写像素格式**：请求 `GUID_WICPixelFormat32bppBGR` 时 PNG 编码器可能改用 24bpp，若仍按 4 字节/像素写入会得到横向彩条。`SetPixelFormat` 后必须校验返回 GUID；截图路径统一用 `32bppBGRA` 并把 alpha 置 255。
5. **`WM_DESTROY` 只能对主窗口 `PostQuitMessage`**：否则用于测 DPI 的临时窗口会提前终止消息循环（表现为"计时器从不触发、进程秒退"）。
6. **PowerShell 5.1 陷阱**：`$ErrorActionPreference='Stop'` 下，原生命令写 stderr 的无害诊断会被当作终止错误；`Invoke-NativeTool` 内临时降级为 `Continue` 并显式读取 `$LASTEXITCODE`。另外 UTF-8 无 BOM 的中文脚本会被 GBK 解析破坏（报"Missing closing '}'"），因此 `build.ps1` 改为**纯 ASCII 输出**；`[Console]::OutputEncoding=UTF8` 让 g++ 中文诊断正确显示。
7. **binutils 2.47 诊断**：链接含清单资源时输出 `.rsrc merge failure: multiple non-default manifests`，非致命；`findstr permonitorv2 eTools.exe` 可证实清单已嵌入。
8. **`GetProcAddress` 强转**：`reinterpret_cast<Fn>(FARPROC)` 触发 `-Wcast-function-type`，用 `memcpy` 中转。
9. **DPI**：`GetDpiForSystem` 用于初始尺寸，避免为测量 DPI 创建临时窗口；窗口路径按 `dpi/96` 缩放绘制。

## 5. 视觉 token（`ui/theme` 将以此为准）

| Token | 值 |
| --- | --- |
| 窗口宽 | 720 DIP |
| 高度 | 72（输入区）+ 结果行 44×N + 12（N ≤ 8） |
| 圆角 | 16（面板）/ 10（行）/ 7（图标底）/ 6（图标） |
| 内描边 | 1px，暗色 `rgba(255,255,255,0.10)`，亮色 `rgba(0,0,0,0.06)` |
| 面板底色 | 暗色 `#1B1B1F`，alpha 0.88（有云母时） |
| 输入字号 | 28 Semibold；占位 36% 透明 |
| 标题/副标题 | 14 / 11.5；副标题 50% 透明 |
| 图标 | 32px 候选尺寸，绘制 28 |
| 选中行 | 主题色 16% 填充 + 左侧 3×20、圆角 1.5 的强调条 |
| 强调色 | 默认设计蓝 `#6EA8FF`；`--accent=system` 跟随系统个性化色 |
| 阴影 | 6 层扩散 2.2px 的圆角填充（待 P2 评估是否换 GaussianBlur 效果） |
| 动效 | 唤出 90ms（scale 0.985→1、opacity 0→1，`cubic-bezier(0.2,0,0,1)`）；选中迁移 120ms；结果刷新 80ms；hover 60ms |

## 6. 复现方式

```powershell
node tools/fetch-toolchain.mjs
powershell -ExecutionPolicy Bypass -File .\build.ps1
.\build\eTools.exe --shot-matrix=docs/images/p1
.\build\eTools.exe --theme=dark --capture=docs/images/p1/window-mica-dark.png
.\build\eTools.exe --theme=light --capture=docs/images/p1/window-mica-light.png
.\build\eTools.exe --theme=dark --glass --capture=docs/images/p1/window-glass.png
```

## 7. P1 实测结论

1. **模块拆分**（9 个编译单元）：`core/{log}`、`platform/{system,hotkey}`、`ui/{theme,renderer,scene,window}`、`app/app`、`main`。渲染器把窗口目标与离屏目标抽象成 `AttachWindow/AttachOffscreen/Resize`，场景绘制只依赖 `Renderer::context()`。
2. **唤出链路耗时（QPC 实测，120 DPI）**：定位 0.4ms + `ShowWindow` 12.2ms + 同步绘制 1.7ms ≈ **14.1ms** 到首帧；`SetForegroundWindow` 一次成功。
3. **首帧优化**：创建后先隐藏渲染一帧（`RenderNow`）预热 Direct2D/DWrite；`Present(1,0)` 改为 `Present(0,0)` 避免等待垂直同步；`ShowWindow` 后 `UpdateWindow` 同步出帧。
4. **前台激活**：`SetForegroundWindow` 失败时用 `AttachThreadInput` 附加前台线程重试；日志记录是否成功，便于后续判定 UAC/全屏场景。
5. **测量陷阱**：用"启动第二个实例"触发唤起时，第二个进程是控制台程序，会短暂抢走前台并让首帧膨胀到 ~100ms（WM_PAINT 晚 42ms、Present 阻塞 46ms）。真实热键路径（单进程 `--show`）为 14.1ms。**性能验收必须用单进程路径测**。
6. **强调色决策**：本机系统强调色为 `#D0000C`（红），直接跟随会把选中行染成酒红，破坏启动器观感；因此默认用设计蓝，`--accent=system` 供原生观感需求。
7. **系统门禁**：`RtlGetVersion` 取 build，< 22621 弹 MessageBox 退出；`--shot`/`--shot-matrix` 不校验，便于任意机器生成设计图。
8. **单实例**：命名互斥体 + `FindWindowW` + `WM_APP+1` 唤起；实测第二实例 1.2s 内退出，第一实例日志出现"显示窗口"。
9. **日志**：默认 `%LOCALAPPDATA%\eTools\logs\etools.log`；沙箱/受限账户下 AppData 不可写时自动退回 exe 同目录（本机验证触发过），`--log=` 可显式指定。
10. **沙箱限制**：本环境无法执行 `Add-Type`（csc 阻塞），交互测试改用"启动第二实例 + 读日志"的方式，不依赖 P/Invoke。

## 8. 下一阶段（P2）入口

- 自绘单行输入框（`WM_CHAR`/`WM_IME_*`、`ImmSetCompositionWindow`、组字下划线），失败则回退透明 `EDIT`。
- 虚拟化结果列表与键盘语义（↑↓/PgUp/PgDn/Enter/Shift+Enter/Ctrl+Enter/Esc）。
- 唤出 90ms 动效、选中迁移 120ms、结果刷新 80ms 淡入，全部基于 QPC 帧计时器。
- 把 `Scene` 的文本布局与渐变资源改为缓存，避免每帧重建。
