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
| 高度 | 84（输入区+列表起点）+ 结果行 44 + 行距 4，可见 6 行时面板 384 DIP |
| 圆角 | 16（面板）/ 10（行）/ 7（图标底）/ 6（图标） |
| 内描边 | 1px，暗色 `rgba(255,255,255,0.10)`，亮色 `rgba(0,0,0,0.06)` |
| 面板底色 | 暗色 `#1B1B1F`，alpha 0.88（有云母时） |
| 输入字号 | 26 Semibold；占位 36% 透明 |
| 标题/副标题 | 14 / 11.5；副标题 50% 透明 |
| 图标 | 32px 候选尺寸，绘制 28 |
| 选中行 | 主题色 16% 填充 + 左侧 3×20、圆角 1.5 的强调条 |
| 强调色 | 默认设计蓝 `#6EA8FF`；`--accent=system` 跟随系统个性化色 |
| 阴影 | 6 层扩散 2.2px 的圆角填充 |
| 动效 | 唤出 90ms alpha 淡入；选中迁移 120ms ease-out；结果刷新 80ms；光标闪烁 530ms |

## 6. 复现方式

```powershell
node tools/fetch-toolchain.mjs
powershell -ExecutionPolicy Bypass -File .\build.ps1
.\build\eTools.exe --shot-matrix=docs/images/p2
.\build\eTools.exe --theme=dark --capture=docs/images/p2/window-mica-dark.png
.\build\eTools.exe --theme=light --capture=docs/images/p2/window-mica-light.png
.\build\eTools.exe --type=cod --keys=down,down --capture=docs/images/p2/typed.png
.\build\eTools.exe --compose=wei --capture=docs/images/p2/compose.png
.\build\eTools.exe --type=ab --capture=docs/images/p2/fade-30ms.png --capture-ms=30
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

## 8. P2 实测结论

1. **输入**：`TextInput` 管理 UTF-16 文本/光标/组字；左右移动与退格按码点处理（跳过代理对），`Ctrl+V` 从剪贴板取 `CF_UNICODETEXT` 并清洗换行；光标闪烁 530ms。
2. **IME**：`WM_IME_START/COMPOSITION/ENDCOMPOSITION` → `ImmGetCompositionStringW` 取 `GCS_COMPSTR`/`GCS_RESULTSTR`/`GCS_CURSORPOS`；显示文本 = 光标前文本 + 组字串 + 光标后文本，组字区间用 `IDWriteTextLayout::SetUnderline` 画下划线；`ImmSetCompositionWindow(CFS_POINT)` 把候选窗贴到光标，`ImmSetCompositionFontW` 同步字体字号。
3. **自动化验证**：新增 `--type`（逐字符 `PostMessage(WM_CHAR)`）、`--keys`（`WM_KEYDOWN` 序列）、`--compose`（直接注入组字状态，验证渲染）、`--capture-ms`（任意时刻截图）。实测 `cod` + 两次下移 + 回车：日志依次 `查询 "c"/"co"/"cod"`、`选中 [1]/[2]`、`回车 [2] 下载`；30ms 截图可见整体淡入中间态，组字截图 `wei` 带下划线且光标跟随。
4. **虚拟化**：12 条样例、可见 6 行，仅绘制可见行；选中越界自动滚动；滚轮可独立滚动列表。
5. **动效**：90ms 淡入、120ms 选中迁移、80ms 结果淡入，全部由 8ms QPC 定时器驱动，收敛后自动 `KillTimer`；`Scene` 每帧只做布局缓存查找与少量笔刷设置。
6. **`com_ptr` 需要移动语义**：`std::vector<ComPtr<T>>` 扩容要求可移动，已补移动构造/赋值。
7. **`mingw` 的 `dcomp.h` 缺失 `IDCompositionVisual::SetOpacity`**：真实 COM 接口有该方法，但头文件未声明。不采用 vtable 偏移 hack，改为场景级 `globalAlpha` 对所有颜色做整体淡出；代价是云母底色不会随内容淡入（只淡面板与内容），视觉上等价于"面板渐显"。
8. **`--glass` 在云母下不可见桌面窗口内容**：云母是壁纸级模糊，透明像素显示的是壁纸而非窗口；该参数只用于诊断渲染 alpha。
9. **真实 IME 待人工确认**：自动化无法安全切换/驱动系统输入法，组字渲染与候选窗定位代码路径已就绪，需在微软拼音/搜狗下人工输入一次中文验证候选窗位置。
10. **HUD 指标**：P2 下首帧仍为 13.1–13.6ms（120 DPI），构建体积 359KB。

## 9. P3 实测结论

1. **索引三源**：开始菜单 `.lnk`（`IShellLinkW` 解析真实目标，用于去重与副标题）、`shell:AppsFolder`（`IPropertyStore` 读 `PKEY_AppUserModel_ID`）、`HKLM/HKCU ...\App Paths`。本机得到 228 条。
2. **去重策略**：标题（忽略大小写）+ 解析后目标双键。修复了 `7zFM`（App Paths）与 `7-Zip File Manager`（.lnk）同时出现的问题。
3. **索引成本与缓存**：首次 650ms → 缓存 1.1ms（`%LOCALAPPDATA%\eTools\apps.idx`，不可写时退回 exe 同目录；开始菜单目录 mtime 更新则重建）。缓存格式 `v2`，字段以 `\t` 分隔、UTF-8，读写时清洗制表符/换行。
4. **图标**：`IShellItemImageFactory::GetImage(32, ICONONLY|RESIZETOFIT)` → HBITMAP → WIC `CreateBitmapFromHBITMAP(UseAlpha)` → **必须转 `32bppPBGRA`** 再 `CreateBitmapFromWicBitmap`（直接传原格式报 `WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT 0x88982F80`）；失败也写入缓存避免每帧重试。
5. **启动**：Shell 条目 `ShellExecuteEx`（`.lnk` 保留参数/工作目录/清单）；UWP 走 `IApplicationActivationManager::ActivateApplication`，失败回退 `shell:AppsFolder\<AUMID>`；`Ctrl+Enter` 用 `runas`，`Shift+Enter` 用 `explorer /select`。
6. **本环境限制**：agent 沙箱拒绝 UWP 激活（`E_ACCESSDENIED`），且 PowerShell `Start-Process shell:AppsFolder\...` 同样失败 → 属沙箱令牌限制，不是代码问题。普通 exe/`.lnk` 启动已实测成功（7zFM 进程启动后手动结束）。
7. **过滤**：`ScoreEntry` 子串优先（前缀 +200），子序列匹配按连续/词首加分；标题不命中且查询 ≥3 字符时回退匹配详情/AUMID（`calc` → `WindowsCalculator` 的 AUMID），权重 180 以下不抢标题命中；`FilterEntries` 排序后截断 200 条。空查询按名称排序；无匹配插入"未找到匹配的应用"占位行。
8. **细节**：副标题里的 `%windir%` 等环境变量已展开；`.lnk` 的副标题显示真实目标而不是"应用"。
9. **待办**：面板高度固定 6 行（结果少时留白）、空查询的最近/常用排序（MRU）、图标磁盘缓存。

## 10. P4 实测结论

1. **索引范围与实现**：默认 `Desktop/Documents/Downloads`（`SHGetFolderPathW` + Downloads 注册表项）；`FindFirstFileExW(FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH)` 每个根目录一个线程，BFS 跳过 reparse point、`$RECYCLE.BIN`、`System Volume Information`、`.git/.svn/.hg/node_modules`，上限 20 万条。
2. **实测规模**：本机默认根目录 1361 条、重建 31ms；`--file-root=C:\Windows\System32` 压测 **29640 条 / 515ms**（目标 5 万 ≤15s 有大量余量）；快照加载 1361 条 0ms。
3. **快照**：二进制 `ETF3`（magic + 版本 + 根目录 + 路径/类型），`files.idx`；AppData 写入失败（沙箱返回 Win32 5）时自动换 exe 同目录，读取时按候选路径依次尝试；应用缓存 `apps.idx` 同样处理。
4. **watcher**：每根目录 `ReadDirectoryChangesW` 递归监听；新增文件**立即**入索引、删除按前缀摘除、目录新增/重命名置 `rebuildRequested_` 交后台重建；窗口每 1s 比对 `version()` 并自动重新过滤。实测新建 `watchtest.txt` 后日志出现「增量更新」与「文件索引已更新」。
5. **查询性能陷阱**：最初每次按键 `PreloadIcons` 同步拉 6 个 shell 图标，查询耗时 56.7ms（远超 16ms 目标）。改为**异步图标**：worker 线程 `IShellItemImageFactory` 取 HBITMAP，UI 线程 `DrainCompleted()` 转 D2D；`Get()` 未命中即入队并返回空（先画首字色块），120ms 定时器补图。查询降到 **0.4–0.6ms**；启动时仍用 `Warm()` 同步预热首屏 6 个图标，避免首次唤出闪字块。
6. **合并排序**：应用（`ScoreEntry`）、文件（文件名命中 +40，路径命中降权 /3）、路径直通（分数 1e6）统一 `stable_sort`，同分保持应用在前；上限 200 行。
7. **路径直通**：查询本身是存在的路径时置顶一行「打开此路径」；`Enter` 打开、`Shift+Enter` 定位。
8. **打开文件的沙箱限制**：窗口内 `ShellExecuteEx` 打开 `.txt` 返回 Win32 5、打开 `.exe` 返回 1223（取消）；而 `--open` 单独调用可成功拉起进程，说明是沙箱对 shell 关联/激活的限制，桌面会话需人工验证。
9. **待办**：面板固定 6 行、图标磁盘缓存、MRU、文件内容搜索、索引根目录设置界面（P5）。

## 11. P5 实测结论

1. **设置**：`settings.conf`（UTF-8 `key=value`，`fileRoot` 可重复）。路径候选 `%APPDATA%\eTools` → exe 同目录，读写都逐个尝试；`theme/accent/hotkey/autostart/tray/fileRoot`。CLI `--theme/--accent` 用 `themeSpecified/accentSpecified` 区分“显式覆盖”与用户设置。实测写入自定义设置后启动日志为 `theme=dark accent="#FF6A00" hotkey=Ctrl+Alt+Space`，热键成功注册 `Ctrl+Alt+Space`。
2. **托盘**：`Shell_NotifyIconW(NIM_ADD + NIM_SETVERSION v4)`，`NOTIFYICON_VERSION_4` 回调用 `LOWORD(lParam)` 判断事件；左键切换窗口、右键弹菜单（显示/开机自启/设置/退出）；监听 `RegisterWindowMessage("TaskbarCreated")` 在资源管理器重启后重挂。沙箱内 `NIM_ADD` 失败，但真实桌面会话自检 `tray=PASS`。
3. **自启**：写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\eTools = "<exe>" --autostart`；沙箱中 `RegOpenKeyExW(KEY_SET_VALUE)` 被拒绝（Win32 5），真实会话自检 `autostart=PASS (set=1 read=1 restore=1)`。
4. **设置窗**：P5 为原生 Win32 控件（ComboBox/Checkbox/ListBox/Button），按 `DpiForPoint` 缩放；P5.1 已替换为自绘深色控件（见 §12），保存回调链不变：窗口 `ApplySettings` → 主题重绘、`files_.Stop/SetRoots/Start`、`SetAutostart`，再由 app 持久化并重注册热键。
5. **动态高度**：`visibleRows_ = clamp(rows.size(),1,6)`，变化时 `ResizeBuffers + SetWindowPos`；实测 3 条结果窗口 960×360（125% DPI），6 条 960×540。结果为空且有查询时给占位行，高度 1 行。
6. **MRU**：`usage.conf` 每行 `count\tlastUnix\t<target>`；`Bonus` = `min(120, count*6)` + 近期加分（1 天 +80 / 7 天 +40 / 30 天 +10）。空查询按 Bonus 降序（同分按名称），非空查询叠加到应用/文件评分。实测启动 7-Zip 后写入一条记录。
7. **快照多路径修复**：`files.idx` 可能同时存在于 AppData 与 exe 目录（根目录不同）。原先只读第一份，根不匹配就放弃；现改为逐个候选解析并只在根目录列表匹配时采用。
8. **体积/回归**：21 个编译单元，exe 约 1.72MB；设置加载后首帧仍 ~13ms；查询 0.3–2ms（含图标命中差异）。
9. **P5.1 已完成**：图标磁盘缓存、深色自绘设置窗、自定义热键、自检脚本；剩余：索引目录选择器内联校验、内存/CPU 基线。

## 12. P5.1 实测结论

1. **图标磁盘缓存**：路径 `%LOCALAPPDATA%\eTools\icons\<fnv1a64(parsingName)>.png`（不可写时 exe 同目录 `icons\`）。worker 先解码 PNG（WIC→PBGRA 像素缓冲），未命中再 `IShellItemImageFactory` 取 HBITMAP，统一转 PBGRA 后写 PNG；UI 线程用 `ID2D1DeviceContext::CreateBitmap` 直接建位图，避免跨线程 D2D。实测冷启动 12 次 shell 取图、首帧 **29.4ms**；二次启动 12 次磁盘命中、0 次 shell、首帧 **15.6ms**，与无缓存基线一致。缓存按解析名哈希，图标尺寸固定 32。
2. **自绘深色设置窗**：背景 `#202024`、控件 `#2A2A30`、文字 `#F0F0F2`、强调 `#6EA8FF`；主题/强调色为自绘循环按钮（去掉 ComboBox 的白色箭头区），热键为自绘捕获框，按钮/复选框 `WM_DRAWITEM` 自绘，列表与文字 `WM_CTLCOLOR*` 深色。窗口按 `DpiForPoint` 缩放（125% → 675×625）。
3. **自定义热键**：`ParseHotkey` 支持 Ctrl/Alt/Shift/Win + 字母、数字、F1–F24、Space、方向键、Home/End/PgUp/PgDn 等；`FormatHotkey` 规范化为 `Ctrl+Alt+K` 形式。设置窗捕获时用 `RegisterHotKey` 预检冲突，冲突显示「该组合已被占用」并阻止保存；启动时 `RegisterConfiguredHotkey` 失败会按预设回退并记日志。实测 `--hotkey=Ctrl+Alt+K`：日志 `热键已注册: Ctrl+Alt+K`。
4. **自检/回归**：`--self-check` 输出 `hotkey/tray/autostart/open/settings_save` 五项 PASS/FAIL 与退出码；`tools/regression.ps1` 调用它并打印人工确认清单。真实桌面会话（2026-10-31，Win11 25H2 26200）实测**五项全部 PASS**，exit=0；沙箱内 tray/autostart/open 会 FAIL，属环境限制。
5. **设置保存实测**：真实会话中通过设置窗保存了 `theme=light, accent=system, fileRoot=F:\, E:\`，说明保存链路与多路径回退可用；`--self-check` 会打印实际写入路径而不是首选路径，并原地重写同一份设置（不改内容）。
6. **体积**：exe 约 1.76MB；图标 PNG 6 个共约 5KB；启动读取 PNG 后首帧恢复 15.6ms。
7. **热键捕获修复**：设置窗捕获时窗口只收到 `VK_PROCESSKEY`（Ctrl+Space 被输入法切换占用），拼出残缺的 `Ctrl`。现于捕获框获得焦点时安装 `WH_KEYBOARD_LL` 钩子：在输入法处理前拿原始按键、命中组合时吞掉该键及其 keyup（避免触发 IME 切换），投递到 UI 线程做格式化和冲突预检；放行 Alt+Tab/Alt+F4/Esc。`--test-capture=Ctrl+Space` 通过 `SendInput` 合成按键做端到端验证：日志 `热键捕获测试: 合成 Ctrl+Space -> 捕获框 "Ctrl+Space"`。

## 13. P6 搜索质量修复（2026-10-31）

1. **配额确定化**：`perRootQuota = 上限 / roots.size()`；默认上限 **520,000**（F:/E: 各 260,000），覆盖本机两盘全部 443,618 条。每个 `EnumerateRoot` 用独立配额，触顶分别记日志「达到上限 N 条，已截断」。两次重建快照 SHA256 相同，解决了此前两线程共享 200,000 配额导致的“谁先抢到算谁的”、结果集合不稳定的问题。上限可用 `fileCap` / `--file-cap` 调到 20 万–100 万。
2. **路径命中降级**：`ScorePathOnly` 要求查询 ≥3 字符、命中前一个字符是 `\ / 空格 - _ .` 边界，分数 `÷6`（约 160–200）；全路径不再做子序列匹配。`Search` 先扫文件名，只有文件名命中 <6 条时才补扫路径，且路径命中最多收 2000 条。修复前 `steamapps` 这类查询会把 `\Games\` 下几千个文件顶进前 80。
3. **稳定排序**：同分比较“文件名长度升序 → 文件优先于目录 → 路径长度升序 → 路径字典序”，`partial_sort` / `sort` 使用同一比较器；两次 `--search-files=gam` 输出一致、无重复。
4. **文件名位图预筛**：`FileEntry.charMask` 记录文件名中出现的 a-z/0-9（36 位），查询为纯 ASCII 字母数字时先做包含检查，跳过不可能命中的名字；1–2 字符只做子串匹配。200k 实测从 17.5–35.5ms 降到 5.7–15.7ms。
5. **实测（47.2 万条，紧凑索引）**：`AlanWake2.exe` 2.7ms（1 条命中）、`game` 10.6ms、`steamapps` 13.8ms、`zzz` 4.7ms（无文件名命中，走路径兜底）。
6. **调试入口**：`--search-files=<query>`（文件索引）、`--search-rows=<query>`（合并 quality/tier/score）、`--search-live=<query>`（现场重建并确认文件是否被收录）。
7. **仍未解决**：47 万条约 216MB（索引 176MB）；子序列噪声；非精确目录仍排在文件后（字面精确目录已置顶）。
8. **排序（质量优先 + 类型层级）**：`quality` 为主键——5 直接路径，4 字面完全一致（`name == query`），3 忽略扩展名一致（`game.exe` 对 `game`），2 前缀，1 子串，0 模糊/仅路径；同 quality 再看 `tier`（0 应用/可执行文件、1 其他文件、2 目录、3 仅路径），最后比分数。`game` 先出 20 个 `Game` 目录（同层按路径长度），`AlanWake2.exe` 字面精确置顶。
9. **启动即重建 + 上限（2026-10-31）**：`Start` 删除所有候选 `files.idx`、清空内存索引后全量重建；常驻模式 `persistSnapshot=false`。默认上限 **1,000,000**，设置窗可直接改（20,000–1,000,000），也支持 `fileCap=`/`--file-cap`；旧配置的 20/30/52 万默认值自动迁移到 100 万。快照读取上限 512MB（44 万条约 102MB）。实测 3 个根（F:/E:/D:）47.2 万条：重建 2.9s、索引 176MB、进程 216MB 常驻 / 266MB 峰值。
10. **紧凑索引**：目录表 `{path, lowerPath}` 每目录一份；条目 `{父目录 id, name, lowerName, charMask, pathHash, isDirectory}`，删除/去重先用 `pathHash` 过滤再校验路径。并行枚举各根用本地 `RootData`，合并时重映射目录 id 并释放 `RootData`；实测内存从 424MB 降到 216MB（47 万条）。`--search-live` 可验证 30 万上限时缺失、100 万时命中 `AlanWake2.exe`。
11. **“C 盘内容”解释**：应用索引与文件索引分离——`apps.idx` 收集开始菜单/AppsFolder/App Paths（含 C 盘系统应用），应用行副标题现在前缀 `应用 · ` 并显示启动路径；`files.idx` 只扫设置根目录（F/E/D），`--search-live=notepad.exe` 的 `exact_name=0` 可证明。启动时逐条打印 `文件索引根: …`。`--file-stats/--list-files` 未指定根时仍用默认桌面/文档/下载（C 盘），属诊断命令默认值。

## 14. 后续方向

- 子序列匹配加“跨度/间隙”惩罚，压掉 `steamapps → StreamingBar` 这类噪声；或对长查询要求连续片段。
- 字符串 arena：把文件名/目录路径存进连续 `wchar_t` 缓冲，条目只存 offset/len，目标把 47 万条压到 ~100MB，为 100 万上限留空间。
- 文件搜索接 Everything 可选适配器（检测到本机安装则优先全盘结果）；USN 全盘索引；文件内容搜索。
- 打包：图标资源、便携模式、可选自签证书；真实会话回归已完成（`tools\regression.ps1` 全 PASS），后续改动重跑即可。
