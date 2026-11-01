# Lume 设计文档

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
| 背景 | 自绘面板 + 自绘柔和阴影，**不用**系统云母（13.7 起；`DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_NONE`） | 云母填满整个窗口矩形，会在面板外留一圈实色「框」（窗口比面板大 `shadowMargin`，当时是 24 DIP） |
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
4. 产物：`build/Lume.exe`，当前 280KB。

## 4. P0 实测结论（踩过的坑）

1. **DComp 透明合成成立**：`CreateSwapChainForComposition`（`DXGI_ALPHA_MODE_PREMULTIPLIED`）+ `DCompositionCreateDevice/TargetForHwnd/Visual/Commit` 后，面板 alpha=0.35 时桌面内容清晰透出；alpha 区域与圆角均正确。
2. **云母在 DComp 窗口上生效**：`DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, DWMSBT_MAINWINDOW)` 返回 `S_OK`，且截图中桌面文字不再透出，呈现壁纸模糊底。注意：系统云母是**壁纸级**模糊，不是"窗口后内容"模糊；若要做 Raycast 式实时模糊，需要 `DCompositionCreateBackdropBrush` 或"显示前抓屏 + 高斯模糊"的快照方案（P2 决定）。**13.7 已改为关闭云母**：它填满整个窗口矩形，面板外那圈透明区会变成实色方块。
3. **D2D 必须 `BeginDraw`/`EndDraw`**：不配对时所有绘制被丢弃，纹理保持全透明（表现为"导出空白 PNG"）。
4. **WIC 会改写像素格式**：请求 `GUID_WICPixelFormat32bppBGR` 时 PNG 编码器可能改用 24bpp，若仍按 4 字节/像素写入会得到横向彩条。`SetPixelFormat` 后必须校验返回 GUID；截图路径统一用 `32bppBGRA` 并把 alpha 置 255。
5. **`WM_DESTROY` 只能对主窗口 `PostQuitMessage`**：否则用于测 DPI 的临时窗口会提前终止消息循环（表现为"计时器从不触发、进程秒退"）。
6. **PowerShell 5.1 陷阱**：`$ErrorActionPreference='Stop'` 下，原生命令写 stderr 的无害诊断会被当作终止错误；`Invoke-NativeTool` 内临时降级为 `Continue` 并显式读取 `$LASTEXITCODE`。另外 UTF-8 无 BOM 的中文脚本会被 GBK 解析破坏（报"Missing closing '}'"），因此 `build.ps1` 改为**纯 ASCII 输出**；`[Console]::OutputEncoding=UTF8` 让 g++ 中文诊断正确显示。
7. **binutils 2.47 诊断**：链接含清单资源时输出 `.rsrc merge failure: multiple non-default manifests`，非致命；`findstr permonitorv2 Lume.exe` 可证实清单已嵌入。
8. **`GetProcAddress` 强转**：`reinterpret_cast<Fn>(FARPROC)` 触发 `-Wcast-function-type`，用 `memcpy` 中转。
9. **DPI**：`GetDpiForSystem` 用于初始尺寸，避免为测量 DPI 创建临时窗口；窗口路径按 `dpi/96` 缩放绘制。

## 5. 视觉 token（`ui/theme` 将以此为准）

| Token | 值 |
| --- | --- |
| 窗口宽 | 720 DIP |
| 高度 | 84（输入区+列表起点）+ 结果行 44 + 行距 4，可见 6 行时面板 384 DIP |
| 圆角 | 16（面板）/ 10（行）/ 7（图标底）/ 6（图标） |
| 内描边 | 1px，暗色 `rgba(255,255,255,0.10)`，亮色 `rgba(0,0,0,0.06)` |
| 面板底色 | 暗色 `#1B1B1F`，alpha 0.88（13.7 起窗口无系统背景，直接与桌面内容混合） |
| 输入字号 | 26 Semibold；占位 36% 透明 |
| 标题/副标题 | 14 / 11.5；副标题 50% 透明 |
| 图标 | 32px 候选尺寸，绘制 28 |
| 选中行 | 主题色 16% 填充 + 左侧 3×20、圆角 1.5 的强调条 |
| 强调色 | 默认设计蓝 `#6EA8FF`；`--accent=system` 跟随系统个性化色 |
| 阴影 | 6 层扩散 2.2px 的圆角填充，扩散量夹在 `shadowMargin - 0.5` 内 |
| 阴影边距 | `shadowMargin = 8 DIP`（窗口 = 面板 + 16 DIP；13.7 之前是 24，中途试过 3） |
| 动效 | 唤出 90ms alpha 淡入；选中迁移 120ms ease-out；结果刷新 80ms；光标闪烁 530ms |

## 6. 复现方式

```powershell
node tools/fetch-toolchain.mjs
powershell -ExecutionPolicy Bypass -File .\build.ps1
.\build\Lume.exe --shot-matrix=docs/images/p2
.\build\Lume.exe --theme=dark --capture=docs/images/p2/window-mica-dark.png
.\build\Lume.exe --theme=light --capture=docs/images/p2/window-mica-light.png
.\build\Lume.exe --type=cod --keys=down,down --capture=docs/images/p2/typed.png
.\build\Lume.exe --compose=wei --capture=docs/images/p2/compose.png
.\build\Lume.exe --type=ab --capture=docs/images/p2/fade-30ms.png --capture-ms=30
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
9. **日志**：默认 `%LOCALAPPDATA%\Lume\logs\lume.log`；沙箱/受限账户下 AppData 不可写时自动退回 exe 同目录（本机验证触发过），`--log=` 可显式指定。
10. **沙箱限制**：本环境无法执行 `Add-Type`（csc 阻塞），交互测试改用"启动第二实例 + 读日志"的方式，不依赖 P/Invoke。

## 8. P2 实测结论

1. **输入**：`TextInput` 管理 UTF-16 文本/光标/组字；左右移动与退格按码点处理（跳过代理对），`Ctrl+V` 从剪贴板取 `CF_UNICODETEXT` 并清洗换行；光标闪烁 530ms。
2. **IME**：`WM_IME_START/COMPOSITION/ENDCOMPOSITION` → `ImmGetCompositionStringW` 取 `GCS_COMPSTR`/`GCS_RESULTSTR`/`GCS_CURSORPOS`；显示文本 = 光标前文本 + 组字串 + 光标后文本，组字区间用 `IDWriteTextLayout::SetUnderline` 画下划线；`ImmSetCompositionWindow(CFS_POINT)` 把候选窗贴到光标，`ImmSetCompositionFontW` 同步字体字号。
3. **自动化验证**：新增 `--type`（逐字符 `PostMessage(WM_CHAR)`）、`--keys`（`WM_KEYDOWN` 序列）、`--compose`（直接注入组字状态，验证渲染）、`--capture-ms`（任意时刻截图）。实测 `cod` + 两次下移 + 回车：日志依次 `查询 "c"/"co"/"cod"`、`选中 [1]/[2]`、`回车 [2] 下载`；30ms 截图可见整体淡入中间态，组字截图 `wei` 带下划线且光标跟随。
4. **虚拟化**：12 条样例、可见 6 行，仅绘制可见行；选中越界自动滚动；滚轮可独立滚动列表。
5. **动效**：90ms 淡入、120ms 选中迁移、80ms 结果淡入，全部由 8ms QPC 定时器驱动，收敛后自动 `KillTimer`；`Scene` 每帧只做布局缓存查找与少量笔刷设置。
6. **`com_ptr` 需要移动语义**：`std::vector<ComPtr<T>>` 扩容要求可移动，已补移动构造/赋值。
7. **`mingw` 的 `dcomp.h` 缺失 `IDCompositionVisual::SetOpacity`**：真实 COM 接口有该方法，但头文件未声明。不采用 vtable 偏移 hack，改为场景级 `globalAlpha` 对所有颜色做整体淡出；代价是云母底色不会随内容淡入（只淡面板与内容），视觉上等价于"面板渐显"。
8. **`--glass` 在云母下不可见桌面窗口内容**：云母是壁纸级模糊，透明像素显示的是壁纸而非窗口；该参数只用于诊断渲染 alpha。（13.7 关闭云母后，`--glass` 透出的就是桌面真实内容。）
9. **真实 IME 待人工确认**：自动化无法安全切换/驱动系统输入法，组字渲染与候选窗定位代码路径已就绪，需在微软拼音/搜狗下人工输入一次中文验证候选窗位置。
10. **HUD 指标**：P2 下首帧仍为 13.1–13.6ms（120 DPI），构建体积 359KB。

## 9. P3 实测结论

1. **索引三源**：开始菜单 `.lnk`（`IShellLinkW` 解析真实目标，用于去重与副标题）、`shell:AppsFolder`（`IPropertyStore` 读 `PKEY_AppUserModel_ID`）、`HKLM/HKCU ...\App Paths`。本机得到 228 条。
2. **去重策略**：标题（忽略大小写）+ 解析后目标双键。修复了 `7zFM`（App Paths）与 `7-Zip File Manager`（.lnk）同时出现的问题。
3. **索引成本与缓存**：首次 650ms → 缓存 1.1ms（`%LOCALAPPDATA%\Lume\apps.idx`，不可写时退回 exe 同目录；开始菜单目录 mtime 更新则重建）。缓存格式 `v2`，字段以 `\t` 分隔、UTF-8，读写时清洗制表符/换行。
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

1. **设置**：`settings.conf`（UTF-8 `key=value`，`fileRoot` 可重复）。路径候选 `%APPDATA%\Lume` → exe 同目录，读写都逐个尝试；`theme/accent/hotkey/autostart/tray/fileRoot`。CLI `--theme/--accent` 用 `themeSpecified/accentSpecified` 区分“显式覆盖”与用户设置。实测写入自定义设置后启动日志为 `theme=dark accent="#FF6A00" hotkey=Ctrl+Alt+Space`，热键成功注册 `Ctrl+Alt+Space`。
2. **托盘**：`Shell_NotifyIconW(NIM_ADD + NIM_SETVERSION v4)`，`NOTIFYICON_VERSION_4` 回调用 `LOWORD(lParam)` 判断事件；左键切换窗口、右键弹菜单（显示/开机自启/设置/退出）；监听 `RegisterWindowMessage("TaskbarCreated")` 在资源管理器重启后重挂。沙箱内 `NIM_ADD` 失败，但真实桌面会话自检 `tray=PASS`。
3. **自启**：写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Lume = "<exe>" --autostart`；沙箱中 `RegOpenKeyExW(KEY_SET_VALUE)` 被拒绝（Win32 5），真实会话自检 `autostart=PASS (set=1 read=1 restore=1)`。
4. **设置窗**：P5 为原生 Win32 控件（ComboBox/Checkbox/ListBox/Button），按 `DpiForPoint` 缩放；P5.1 已替换为自绘深色控件（见 §12），保存回调链不变：窗口 `ApplySettings` → 主题重绘、`files_.Stop/SetRoots/Start`、`SetAutostart`，再由 app 持久化并重注册热键。
5. **动态高度**：`visibleRows_ = clamp(rows.size(),1,6)`，变化时 `ResizeBuffers + SetWindowPos`；实测 3 条结果窗口 960×360（125% DPI），6 条 960×540。结果为空且有查询时给占位行，高度 1 行。
6. **MRU**：`usage.conf` 每行 `count\tlastUnix\t<target>`；`UsageStore::UsageScore` = `min(120, count*6)` + 近期加分（1 天 +80 / 7 天 +40 / 30 天 +10），`Bonus()` 与非空查询加分同口径。**空查询不再列出全部应用**，改为 `Top()`（按 score 降序、同分更近优先）逐条还原：命中应用 `launchTarget` → 应用行（带图标与「应用 · 」副标题），否则目标仍是现存文件/目录 → 文件行，已删除的目标跳过；没有使用记录时返回空列表。非空查询仍按 `quality/tier/score` 排序并叠加 Bonus。启动 7-Zip 后写入一条记录。
7. **快照多路径修复**：`files.idx` 可能同时存在于 AppData 与 exe 目录（根目录不同）。原先只读第一份，根不匹配就放弃；现改为逐个候选解析并只在根目录列表匹配时采用。
8. **体积/回归**：21 个编译单元，exe 约 1.72MB；设置加载后首帧仍 ~13ms；查询 0.3–2ms（含图标命中差异）。
9. **P5.1 已完成**：图标磁盘缓存、深色自绘设置窗、自定义热键、自检脚本；剩余：索引目录选择器内联校验、内存/CPU 基线。

## 12. P5.1 实测结论

1. **图标磁盘缓存**：路径 `%LOCALAPPDATA%\Lume\icons\<fnv1a64(parsingName)>.png`（不可写时 exe 同目录 `icons\`）。worker 先解码 PNG（WIC→PBGRA 像素缓冲），未命中再 `IShellItemImageFactory` 取 HBITMAP，统一转 PBGRA 后写 PNG；UI 线程用 `ID2D1DeviceContext::CreateBitmap` 直接建位图，避免跨线程 D2D。实测冷启动 12 次 shell 取图、首帧 **29.4ms**；二次启动 12 次磁盘命中、0 次 shell、首帧 **15.6ms**，与无缓存基线一致。缓存按解析名哈希，图标尺寸固定 32。
2. **自绘深色设置窗**：背景 `#202024`、控件 `#2A2A30`、文字 `#F0F0F2`、强调 `#6EA8FF`；主题/强调色为自绘循环按钮（去掉 ComboBox 的白色箭头区），热键为自绘捕获框，按钮/复选框 `WM_DRAWITEM` 自绘，列表与文字 `WM_CTLCOLOR*` 深色。窗口按 `DpiForPoint` 缩放（125% → 675×625）。
3. **自定义热键**：`ParseHotkey` 支持 Ctrl/Alt/Shift/Win + 字母、数字、F1–F24、Space、方向键、Home/End/PgUp/PgDn 等；`FormatHotkey` 规范化为 `Ctrl+Alt+K` 形式。设置窗捕获时用 `RegisterHotKey` 预检冲突，冲突显示「该组合已被占用」并阻止保存；启动时 `RegisterConfiguredHotkey` 失败会按预设回退并记日志。实测 `--hotkey=Ctrl+Alt+K`：日志 `热键已注册: Ctrl+Alt+K`。
4. **自检/回归**：`--self-check` 输出 `hotkey/tray/autostart/open/settings_save` 五项 PASS/FAIL 与退出码；`tools/regression.ps1` 调用它并打印人工确认清单。真实桌面会话（2026-10-31，Win11 25H2 26200）实测**五项全部 PASS**，exit=0；沙箱内 tray/autostart/open 会 FAIL，属环境限制。
5. **设置保存实测**：真实会话中通过设置窗保存了 `theme=light, accent=system, fileRoot=F:\, E:\`，说明保存链路与多路径回退可用；`--self-check` 会打印实际写入路径而不是首选路径，并原地重写同一份设置（不改内容）。
6. **体积**：exe 约 1.76MB；图标 PNG 6 个共约 5KB；启动读取 PNG 后首帧恢复 15.6ms。
7. **热键捕获修复**：设置窗捕获时窗口只收到 `VK_PROCESSKEY`（Ctrl+Space 被输入法切换占用），拼出残缺的 `Ctrl`。现于捕获框获得焦点时安装 `WH_KEYBOARD_LL` 钩子：在输入法处理前拿原始按键、命中组合时吞掉该键及其 keyup（避免触发 IME 切换），投递到 UI 线程做格式化和冲突预检；放行 Alt+Tab/Alt+F4/Esc。`--test-capture=Ctrl+Space` 通过 `SendInput` 合成按键做端到端验证：日志 `热键捕获测试: 合成 Ctrl+Space -> 捕获框 "Ctrl+Space"`。
8. **自启勾选框修复**：自绘复选框原先写成 `BS_AUTOCHECKBOX | BS_OWNERDRAW`。按钮样式类型占低位 4 位（`BS_AUTOCHECKBOX=0x3`、`BS_OWNERDRAW=0xB`），按位或后类型变成 `BS_OWNERDRAW`：控件不再自动切换勾选，`BM_GETCHECK`/`BM_SETCHECK` 也全部失效——表现为「点不动」，且保存时永远读到未勾选，`autostart` 无法从设置窗开启。现只用 `BS_OWNERDRAW`，勾选状态由 `settings_.autostart` 维护：`WM_COMMAND` 里翻转并 `InvalidateRect`，`DrawButton` 按该值画（勾选为强调色实心块 + 深色对勾，未勾选按下时描边强调色），`OnSave` 直接取该值。探针（真实鼠标消息点击 + 保存回调 + 截图）三例均通过。

## 13. P6 搜索质量修复（2026-10-31）

1. **配额确定化**：`perRootQuota = 上限 / roots.size()`；默认上限 **520,000**（F:/E: 各 260,000），覆盖本机两盘全部 443,618 条。每个 `EnumerateRoot` 用独立配额，触顶分别记日志「达到上限 N 条，已截断」。两次重建快照 SHA256 相同，解决了此前两线程共享 200,000 配额导致的“谁先抢到算谁的”、结果集合不稳定的问题。上限可用 `fileCap` / `--file-cap` 调到 20 万–100 万。
2. **路径命中降级**：`ScorePathOnly` 要求查询 ≥3 字符、命中前一个字符是 `\ / 空格 - _ .` 边界，分数 `÷6`（约 160–200）；全路径不再做子序列匹配。`Search` 先扫文件名，只有文件名命中 <6 条时才补扫路径，且路径命中最多收 2000 条。修复前 `steamapps` 这类查询会把 `\Games\` 下几千个文件顶进前 80。
3. **稳定排序**：同分比较“文件名长度升序 → 文件优先于目录 → 路径长度升序 → 路径字典序”，`partial_sort` / `sort` 使用同一比较器；两次 `--search-files=gam` 输出一致、无重复。
4. **文件名位图预筛**：`FileEntry.charMask` 记录文件名中出现的 a-z/0-9（36 位），查询为纯 ASCII 字母数字时先做包含检查，跳过不可能命中的名字；1–2 字符只做子串匹配。200k 实测从 17.5–35.5ms 降到 5.7–15.7ms。
5. **实测（512MB 预算 / 99.98 万条）**：`AlanWake2.exe` 5.4ms（1 条命中）；47.2 万条时 `game` 6.4ms。
6. **调试入口**：`--search-files=<query>`（文件索引）、`--search-rows=<query>`（合并 quality/tier/score）、`--search-live=<query>`（现场重建并确认文件是否被收录）。
7. **仍未解决**：99.98 万条约 328MB 常驻（读快照后重建峰值约 560MB）；快照 230MB；子序列噪声；非精确目录仍排在文件后（字面精确目录已置顶）。
8. **排序（质量优先 + 类型层级）**：`quality` 为主键——5 直接路径，4 字面完全一致（`name == query`），3 忽略扩展名一致（`game.exe` 对 `game`），2 前缀，1 子串，0 模糊/仅路径；同 quality 再看 `tier`（0 应用/可执行文件、1 其他文件、2 目录、3 仅路径），最后比分数。`game` 先出 20 个 `Game` 目录（同层按路径长度），`AlanWake2.exe` 字面精确置顶。
9. **快照优先 + 大小上限（2026-10-31）**：`Start` 先读最新的 `files.idx`（按候选路径 mtime 取最新，ETF4 紧凑格式 100 万条 656ms），再后台全量重建；常驻模式只在首次/手动清除时写回。上限改为**索引文件大小**：设置窗“索引大小”MB，默认 **512**，范围 64–262144（256GB），`indexSizeMB=`/`--index-size-mb`；4 个根平分预算，按完整路径 `4 + len*2 + 1` 保守累计（实际 ETF4 文件约小一半），达到即截断。设置窗“清除索引并重建”（删快照、清内存、重置冷却）。watcher 忽略自己的快照/日志/图标，目录高频变化 60s 冷却；退出可取消，快照流式写 `.tmp` 后原子替换。实测 512MB 预算：999,842 条、快照 **124.4MB**、重建 20.8s、进程 323MB 常驻 / 410MB 峰值（无快照首次）、读快照后再重建峰值约 560MB；`AlanWake2.exe` 查询 4.8ms。
10. **紧凑索引**：目录表 `{path, lowerPath}` 每目录一份；条目 32 字节 `{父目录 id, nameOffset/len, lowerOffset/len, charMask, pathHash32, isDirectory}`。文件名/小写名放连续 `wchar_t` arena，`name == lowerName` 时（本机 78%）复用同一 offset。目录索引从字符串 map 换成 64 位哈希 `unordered_multimap` + 路径校验；路径哈希压到 32 位。并行枚举各根用本地 `RootData`，合并时逐个根重映射并立即释放，峰值从 130MB 降到 109MB。Entry 从 96B 降到 32B。
11. **“C 盘内容”解释**：应用索引与文件索引分离——`apps.idx` 收集开始菜单/AppsFolder/App Paths（含 C 盘系统应用），应用行副标题前缀 `应用 · `；`files.idx` 只扫设置根目录（C/F/E/D）。启动逐条打印 `文件索引根: …`。`--file-stats/--list-files` 未指定根时仍用默认桌面/文档/下载（C 盘），属诊断命令默认值。
12. **存储占用（512MB 预算、999,842 条）**：ETF4 快照 **124.4MB**（130,480,000 字节：目录树 + 名字 arena + 条目数组）、`apps.idx` 48KB、图标/日志/配置 0.83MB，正常会话总计约 **125MB**。加载 656ms。沙箱写不进 AppData 时落在 exe 同目录，且可能残留旧 AppData 快照（启动取最新一份）。进一步可用 UTF-8 名字/压缩把快照压到 <100MB。

## 13.1 设置窗「索引」页（2026-11-01）

1. **拆分原因**：设置窗原来是单页，索引上限、根目录、清除按钮和主题/热键挤在一起；索引相关的管理动作（状态、路径、清除、重建、打开目录）需要独立空间，因此改成「常规 / 索引」两页的自绘页签（同一窗口 `540x545`，`SetPage()` 用 `ShowWindow` 显隐两组控件，选中页签文字高亮 + 2px 强调色下划线），窗口尺寸与 DPI 缩放逻辑不变。
2. **接口**：`SettingsWindow::Show(instance, owner, current, Callbacks)`，`Callbacks{onSave, queryIndex, onClearIndex, onRebuildIndex, onOpenIndexDirectory}`。`IndexStatus` 是只读视图模型（`directory/defaultDirectory/snapshotPath/snapshotBytes/entries/limitMB/lastRebuildDurationMs/rebuilding/snapshotWritable`），由 `LauncherWindow::QueryIndexStatus()` 从 `FileIndex` 汇总，UI 不直接依赖 `FileIndex`。
3. **索引层新增 API**：`SetIndexDirectory/indexDirectory/snapshotPath/snapshotBytes/snapshotWritable`、`ClearIndex()`（删快照 + 清内存，**不**重建）、`RebuildIndex()`（请求重建，**跳过** 60s 冷却）、`rebuilding()/lastRebuildDurationMs()`；删除 `ClearAndRebuild()`。`SnapshotPaths()` 在配置了自定义目录时只认该目录（不静默回退默认目录），默认目录仍保留 `%LOCALAPPDATA%\Lume` → exe 目录 → 旧 eTools 三个候选。
4. **并发细节**：`WorkerLoop` 用 `rebuilding_` 标记在跑的重建，并记录本次耗时；`ClearIndex()` 若在重建途中被点下则置 `discardBuild_`，`RebuildAndSwap()` 在**交换 storage 之前**检查并丢弃该次结果，避免索引“自己回来”。手动重建把 `lastRebuildMs_` 归零以绕过冷却（该时间戳同时是冷却基准）。
5. **配置**：`settings.conf` 新增 `indexDir=`（留空不写）与 `indexDirHistory=`（可重复，最多 5 条，按最近使用排序），`--index-dir=<dir>` 可临时覆盖；应用前校验绝对路径、目标是目录、`EnsureDirectory` 成功、并写一个 `FILE_FLAG_DELETE_ON_CLOSE` 探针文件确认可写，失败则拒绝应用并在状态行提示。
6. **索引目录下拉框**：`CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS`——不可手打、鼠标可选中；`WM_MEASUREITEM` 定行高（`Scale(24)`），`WM_DRAWITEM`（`ODT_COMBOBOX`）自绘深色项，配合 `SetWindowTheme(combo, L"DarkMode_CFD")` 让关闭态也是深色。候选项 = 当前值（即使目录暂时不可用也显示）+ 默认目录 + 最近使用中**仍然存在**的目录（`GetFileAttributes` 过滤，去重、大小写不敏感）；「修改目录」会把选中的新目录插到历史最前并重建候选列表并选中它。
7. **应用 / 保存 / 取消**：`ApplyConfiguration(close)` 统一校验并写回 `settings_.*`，再回调 `onSave`（→ `LauncherWindow::ApplySettings` + 热键重注册 + 持久化）；「应用」`close=false` 只提示「已应用」并刷新一次状态块，「保存」`close=true` 应用后 `DestroyWindow`，「取消」丢弃副本。索引目录等于默认目录时归一化为空值；历史在应用时按「存在的目录 + 前 5 条」重写。
8. **状态刷新（手动）**：早期版本在索引页可见时用 `SetTimer(500ms)` 轮询状态，但每次 `SetWindowTextW` 都会重绘静态文本框，状态块（快照/大小/状态/目录四行）一直闪烁。现在**不自动刷新**：只在打开页面、点「清除索引 / 重建索引 / 刷新状态」、应用配置后读一次 `FileIndex`。watcher 的忽略规则额外判断“路径是否在快照目录之下”，自定义目录落在被监听根内部时不会自触发重建。
9. **验证**：`build/probe/probe_settings_pages.cpp`（gitignored，带 app 清单以便渲染 comctl32 v6 下拉框）用真实鼠标消息驱动两页：页签切换、状态文本、三个动作回调、下拉框只读/候选过滤（不存在的历史不进列表）/选中应用、应用不关窗且记录历史、保存关窗且默认目录归一化为空，以及索引层「自定义目录写入 → 清除 → 立即重建（47–63ms，绕过冷却）」全部通过；CLI `--index-dir` 实测 `--file-stats` 写在自定义目录、`--search-files` 从该目录读回（`snapshot=1`），默认目录行为不变。

## 13.2 空查询推荐 = 最近常用（2026-11-01）

1. **行为**：空查询（刚唤出、Esc 清空、删光输入）只显示使用记录里的条目，按“频次 + 近期”排序；**没有使用记录时一行都不显示**，面板收缩成只有输入框。想浏览全部应用请直接输入（子串/前缀匹配）。
2. **还原规则**（`BuildMergedRows`，merge.cpp）：`UsageStore::Top()` 逐条还原，`launchTarget` 与某条应用记录大小写不敏感相等 → 复用应用行（图标、`应用 · ` 副标题、UWP 动作）；否则目标仍是现存文件/目录 → 文件行（目录给文件夹图标与 `OpenFolder`）；目标已删除 → 跳过。命中前 50 条即停，空查询分支直接按使用热度返回，不再参与 `quality/tier` 重排。
3. **0 行面板**：`PanelHeight(theme, 0) = listTop + bottomPadding`（72 输入区 + 28 空白）；`Scene::Draw` 的 `visibleRows` 下界从 1 改成 0，分隔线只在 `state.rows` 非空时绘制；`LauncherWindow::UpdateLayoutForRows` 的 `clamp` 下界同样改成 0，窗口高度随行数 0–6 收缩/展开。
4. **唤出时刷新**：`UsageStore::Record` 发生在启动成功之后、`usage.conf` 在退出时落盘；`ShowAndFocus()` 在空查询时先 `RebuildRows()`，保证推荐顺序是刚刚用过的顺序，而不是上次隐藏时的快照。
5. **验证**：`build/probe/probe_suggestions.cpp`（gitignored）覆盖 空记录→0 行、应用/文件还原、缺失目标跳过、热度排序（count 6 次 > 1 次）、`PanelHeight(0)=100 < PanelHeight(1)=144`、非空查询与占位行不变；真实进程 `--capture` 两张图：无 `usage.conf` 时面板仅 960×185（只有输入框），预置 3 条记录时 960×360（notepad.exe 7 次 / eTools 目录 4 次 / explorer.exe 2 次，按热度排列）。

## 13.3 文件搜索来源：本地索引 / Everything（2026-11-01）

1. **为什么是「换后端」而不是「加一条结果流」**：文件搜索原来只有一个出口——`merge.cpp` 里的 `files->Search(query, kMaxFileHits)`，且 `FileIndex::Search()` 返回的 `FileHit{path, isDirectory, score, pathOnly, quality}` 已经是合并层需要的全部信息。因此把 `BuildMergedRows(apps, FileIndex*, ...)` 改成接收 `const std::vector<FileHit>&`，由调用方（`LauncherWindow::UpdateFileHits()`）按来源收集，合并/排序/图标/MRU/启动链路一行没改。`FileHit` 与评分口径（`ScoreLowerText`/`NameQuality`/`ScorePathOnly`/`QueryMask`）从 `file_index.cpp` 抽到 `file_hit.h`/`file_score.*`，两个后端共用同一套打分，保证切换后排序规则一致（重构后 `--search-rows=game` 输出与重构前**逐行一致**）。
2. **为什么编源码而不是带 DLL**：官方 SDK 同时提供 `dll/Everything64.dll`（+ 导入库 `lib/Everything64.lib`）与 `src/Everything.c`（DLL 的完整实现，MIT）。链接 `.lib` 只是导入库，exe 运行时仍依赖 DLL，缺 DLL 直接起不来；改带 DLL 则发布包多一个文件、多一条「找不到 DLL → 功能降级」的失败路径。最终把 `third_party/everything/{include/Everything.h, ipc/Everything_IPC.h, src/Everything.c}` 三个官方文件**逐字节原样**入库，用 `gcc` 以 C 语义编译进 exe：
   - 第一方 TU 加 `-DEVERYTHINGUSERAPI=`（`Everything.h` 用 `#ifndef` 守卫，默认 `__declspec(dllimport)`，静态链接必须清空否则出现 `__imp_Everything_*` 未定义）；
   - `Everything.c` 自己 `#define EVERYTHINGUSERAPI __declspec(dllexport)`，在 exe 里只是导出符号，因此**无需打补丁**；
   - 第三方 TU 用独立的 `$thirdPartyCommon`：`-std=c11`、`-w`、**不定义** `WIN32_LEAN_AND_MEAN`（源码要用 `shellapi.h` 里的 `ShellExecuteEx`，该头被 LEAN_AND_MEAN 排除），不加 `-Werror`（源码有 MSVC 的 `#pragma warning` 与 `stricmp`）；
   - 增量构建同时扫描 `src/` 与 `third_party/` 的头文件时间戳。体积代价 +27 KB。
3. **非阻塞查询**：用 `Everything_SetReplyWindow(hwnd)` + `Everything_SetReplyID(gen)` + `Everything_Query(FALSE)`——`SendMessage` 到 Everything 的 IPC 窗口只花 <5 ms，结果以 `WM_COPYDATA`（`dwData == replyID`）回到主窗口，`Everything_IsQueryReply(msg,w,l,gen)` 命中后 SDK 把结果拷进自己的全局列表再读出来。`LauncherWindow::Handle()` 里比较生成号，过期回复（含用 `Everything_IsQueryReply` 不匹配的旧 id）直接丢弃。回复缓冲区只在消息处理期间有效，因此字符串立刻拷进 `std::wstring`。
4. **打字链路**：`Refilter()` → `RefreshResults()` → `UpdateFileHits()`：本地模式同步 `files_.Search()`；Everything 模式只**发请求**并沿用上一批命中，命中到达后再走一次 `RebuildRows()`。实测「过滤」耗时 0.3–2.5 ms（不含 Everything 的异步等待），Everything 往返 ~25 ms（本机 1.4.1.1028 / 116 万文件；把 `SetMax` 从 500 降到 120 后往返不变，说明是本机固定开销，不是回复体积）。
5. **查询净化**（`EverythingIndex::SanitizeQuery`）：SDK 把字符串原样交给 Everything，Everything 语法仍然生效，所以按空白切词 → 去掉 `& | ! < > * ? "` → 含 `:` 或 `\` `/` 的词用双引号包成字面量 → 空格连接（AND）。实测 `regex:notepad` / `content:hello` 都被当成字面词（0 命中），不会触发正则或内容搜索。打分用的是**去引号后的**词（`StripQuotes`），否则带引号的路径查询永远算不出分数。
6. **评分与分层**：Everything 只做候选集（`EVERYTHING_REQUEST_FULL_PATH_AND_FILE_NAME`、`EVERYTHING_SORT_NAME_ASCENDING`、`SetMax(500)`；名字升序是 Everything 的最快路径）。文件名命中走 `ScoreFileName()`（单词查询与本地同款，多词查询取各词最高分再降权一半）；文件名没命中时退到 `ScorePathOnly()` 并标 `pathOnly`（与本地索引的兜底一致）。命中分统一 `+40` 后再交给合并层做 `quality → tier → score` 排序。
7. **切换语义**（`LauncherWindow::ApplySettings`）：
   - 本地 → Everything（Everything 可用且 DB 就绪）：先取 `snapshotBytes()`，再 `files_.ClearIndex()`（删除 `<目录>\files.idx` 的全部候选路径 + 清内存 + 丢弃在途重建），`files_.Stop()`，`everything_.Attach(hwnd_)`，日志「已切到 Everything，删除本地快照 X MB」；
   - 本地 → Everything（不可用）：只停本地索引并保留 `files.idx`，置 `pendingSnapshotDelete_`，下次请求发现它回来时一次性补删——避免用户切过去后既没有 Everything 也没有本地索引；
   - Everything → 本地：`Detach()`（`Everything_CleanUp`）→ 重新 `Start()`，无快照则后台重建（512 MB 预算约 20.8 s），完成前结果是空的但 UI 正常。
   - 设置窗里「文件搜索来源」是**待应用值**（`pendingFileSearch_`），点「应用/保存」才生效；Everything 模式下本地专用控件（索引大小、索引目录及三个动作、根目录列表与增删、清除/重建）整体 `EnableWindow(FALSE)`，只留来源按钮、刷新状态和应用/保存/取消。
8. **状态块**：`IndexStatus` 扩了 `fileSearch` 与 `evAvailable/evDbLoaded/evVersion/evError`，`RefreshIndexInfo()` 按待应用来源渲染三态——本地（快照/大小/占用率/条目/重建状态/目录）、Everything 已连接（版本 + 索引就绪 + 本地快照是否已删）、Everything 未连接（原因 + 「切回本地索引需重建」提示）。`Everything_GetMajorVersion()` 返回 0 即视为未运行，错误码经 `EverythingErrorText()` 映射成中文。
9. **验证**：`--everything-check` 打印 `available/db_loaded/version` 与一次阻塞查询的命中数/耗时/前 10 条，并用同一批命中跑 `BuildMergedRows` 打印 `tier/quality/score`（排序口径回归）；`--self-check` 加 `everything=PASS|SKIP|FAIL`（未装/未运行只算 SKIP）；`--file-search=local|everything` 覆盖本次来源。端到端用临时 `--index-dir` + 假 `files.idx` 验证：切到 Everything 后假快照被删、`settings.conf` 写入 `fileSearch=everything`（沙箱里 AppData 不可写时落在 exe 目录）、切回本地后日志出现「已切回本地索引（无快照时后台重建）」并启动后台重建；设置窗三态截图见 `docs/images/p6/settings-everything.png`。

## 13.4 设置窗「关于」页（2026-11-01）

1. **动机**：版本号原先只出现在 `--version`、帮助文本与 exe 版本资源里，用户反馈问题时拿不到「版本 + 系统 + 哪个数据目录 + Everything 状态」这一组信息。新增第三个页签「关于」（放在最后；P6.4 加入「筛选」页后它变为第四个页签，仍在最后），只读展示这些内容，并提供一键复制。
2. **版本单一来源**：新增 `src/app/version.h`（`kAppName`/`kAppVersion`），`--version`、`PrintUsage()` 与关于页都读它；`assets/app.rc`（VERSIONINFO）、`assets/app.manifest`、`build.ps1 -Version` 仍是独立来源，所以 `version.h` 顶部写明「四处一起改」。没有改成运行时读 exe 版本资源，是为了不给 CLI 路径引入额外的 `GetFileVersionInfo` 依赖。
3. **数据流**：`Callbacks` 新增 `std::function<AboutInfo()> queryAbout`，`LauncherWindow::QueryAboutInfo()` 汇总 `kAppVersion`、`QueryOsVersion()`（build ≥ 22000 记为 Windows 11）、`dpi_`、当前热键、`SettingsStore::Path()`、`UsageStore::DefaultPath()`（本次把 `UsageStore::Path()` 提成公开静态方法）、`LogPath()`、`files_.indexDirectory()`，以及 Everything 模式下 `everything_.Status()` 的版本/就绪状态。设置窗只做渲染，不碰这些来源。
4. **交互**：`BuildAboutText()` 生成一段多行文本（关于页的 STATIC 与剪贴板共用同一份），「复制版本信息」走 `OpenClipboard`/`SetClipboardData(CF_UNICODETEXT)`，失败时在状态行说明原因（剪贴板被占用 / 内存不足）。页签与其它页一致：`SetPage()` 用 `ShowWindow` 显隐 `aboutControls_`（当时关于页是第 3 页、`SetPage(2)`；加了「筛选」页后是第 4 页、`SetPage(3)`），`DrawButton` 里页签共用一个「选中项高亮 + 底部强调色条」分支。与索引页一样**不自动刷新**，只在切到该页时读一次。
5. **布局**：窗口高度不变（578），关于页复用索引页的纵向区间：信息块 `(20,64,500,330)` 多行 STATIC，动作行 `(20,412)` 一个「复制版本信息」按钮 + 右侧一行说明；常规页的提示文字补了一句「版本与数据目录在「关于」页」。
6. **验证**：`--settings` + 合成 `WM_COMMAND(132)` 切页截图（`docs/images/p6/settings-about.png`，675×723）；合成 `WM_COMMAND(133)` 后读系统剪贴板，内容为完整信息块（当时为 `Lume 0.2.0`，含 `Windows 11（build 26200） · x64 · DPI 120 (125%)`、`Everything 1.4.1.1028（已连接，索引就绪）` 与四个路径），状态行显示「版本信息已复制到剪贴板」。

## 13.5 搜索筛选组（2026-11-01）

1. **数据模型**（`search/filter.{h,cpp}`）：`FilterGroup{name, extensions[], includeApps, includeFolders, includeNoExtension}` + `FilterSet{enabled, active, groups[]}`。`active` 是**芯片序号**，`0` 固定为内置「全部」（不落盘、不可改名/删除），`1..N` 映射 `groups[active-1]`；`ActiveFilterGroup()` 在「全部」或 `enabled=false` 时返回 `nullptr`，全链路用「`nullptr` = 不过滤」表达，调用点不需要分支判断「是不是全部」。
2. **扩展名口径**：扩展名统一小写、不带点，只允许 `[a-z0-9-_]`（`appref-ms` 是真实存在且默认组要用的扩展名，所以放行 `-`/`_`）；`*.PNG`、`.png` 都归一成 `png`；`FilterMatchesPath()` 取最后一个 `\`/`/` 之后的基名、最后一个 `.` 之后的尾巴，无扩展名（含 `.gitignore` 这种只有前缀点的）走 `includeNoExtension`；目录走 `includeFolders`。**匹配只发生在已经命中的候选上**，所以不会给全表扫描加开销。
3. **本地索引**：`FileIndex::Search(query, limit, const FilterGroup*)` 在「`score > 0` 之后、`push_back` 之前」过滤（路径兜底循环同样处理）。这个位置很关键：如果放到合并层之后过滤，`limit=80` 的截断已经发生，前 80 条若是别的类型，真正的同类型命中会被截掉——用户在「图片」里会看到空白。
4. **Everything**：`Request(query, gen, filter, allowPushdown)` 在 `SanitizeQuery()` 之后追加 ` ext:a;b;c`（只对**纯扩展名白名单**组下推：`FilterIsExtensionWhitelist()`，即没有文件夹/无扩展名开关）；含这两个开关的组没法用 `ext:` 表达，只做本地后过滤。两处都做：下推缩小候选集，后过滤保证即使 `ext:` 语义有出入也不会漏出别的类型。`CollectHits()` 用**组副本**（不是指针）过滤，因为请求在途时设置可能已经改了、`groups` 的 vector 会重分配。
5. **零结果重试（以及一个 3 秒卡顿的坑）**：纯扩展名组下推后 0 条时（某个扩展名在 `ext:` 里表现异常），去掉下推、加一代生成号重发一次；`pushdownRetryKey_ = "查询\n组名"` 保证「同一查询 + 同一组」只重试一次，不会来回抖动。**这次重发必须从 `WM_COPYDATA` 处理里延后出去**（`PostMessage(kMsgEverythingRetry)`）：Everything 把结果投给回复窗口用的是**同步** `SendMessage`（其内部超时 3 s），我们处理这条消息时它的 UI 线程正阻塞着；此时再 `Everything_Query()` 就是两个线程互相等待。实测症状是切到「没有结果的组」时界面僵 3005.8 ms（日志 `Everything 发送偏慢: "notepad" 3005.8 ms`），期间排队的按键在 3 秒后一次性涌出——正是用户报的「切到没结果的组再切下一个组会卡一下」。现在：`WM_COPYDATA` 里只拷贝命中 + 重排 + `PostMessage` 重试；`kMsgEverythingRetry` 处理时先比对 `pushdownRetryKey_` 确认「查询 + 组」没变再发；`handlingEverythingReply_` 作为防御闸门，任何在这段里误发的 `RequestEverythingRows()` 都会被自动改成延后。修复后同一操作的往返回到 15–30 ms，UI 线程 `WM_NULL` 响应时间恒为 0 ms。
6. **合并层**：`BuildMergedRows(..., const FilterGroup* filter)` 对应用行看 `includeApps`（应用索引条目不是文件，无法用扩展名判断），文件行/空查询的 MRU 文件行再做一次 `FilterMatchesPath()` 兜底；**用户直接输入的完整路径行不受筛选影响**（否则输入 `C:\x\y.png` 在「应用程序」组里就打不开了）；无匹配时的占位行改成「没有符合「图片」的结果 / 按 Tab 换一组，或切回「全部」」。
7. **持久化**（`settings.conf`，`version=3`）：`filterEnabled=0|1`、`filterActive=<芯片序号>`、`filterGroup=名称|exe;png|apps|folders|noext`（可重复，顺序即芯片顺序；名称里的 `|` 在编码时被剥掉）。读侧容错：非法的 `filterGroup` 行只跳过该行并记日志，重名/超限的组丢弃，`filterActive` 最后 `ClampFilterActive()` 收敛；**一条组行都没有**时才用 `DefaultFilterSet()`（老配置升级即得默认组）。切组时立即 `onSettingsChanged_` → `SettingsStore::Save()`（一次约 1 KB），其它时候不写盘。
8. **设置窗第 3 页**（`kIdTabFilter=134`，页序 常规/索引/筛选/关于，「关于」仍在最后）：工作副本 `filters_` 与索引页一样「点应用/保存才生效」。列表首项固定「全部」，选中它时右侧编辑区与删除/上移/下移整体禁用（`UpdateFilterControlState()`）；`CommitFilterEdits()` 在切项、新建、删除、上移/下移、应用/保存前把编辑区写回工作副本；`SanitizeFilterSet()` 负责重名、空规则、`全部` 保留名、组数 ≤12 的校验，失败时状态行给中文原因并且**不写盘、不关窗**。三个开关与「启用」都是 `BS_OWNERDRAW` 自绘勾选框（沿用自启开关的做法：`BS_AUTOCHECKBOX | BS_OWNERDRAW` 会互相覆盖类型位，所以状态自己维护）。
9. **面板几何**：`Theme::filterRow=40`、`chipHeight=26`、`chipGap=8`、`chipPaddingX=12`；`PanelHeight(theme, rows, withChips)` 在 `withChips` 时把 `listTop` 从 84 抬到 124，关掉筛选就完全回到旧几何。芯片宽度按 DirectWrite 实测文本宽 + 2×padding，超宽时整体左移并 `PushAxisAlignedClip` 裁剪，**保证当前组完整可见**；`Scene::ChipAt()` 与绘制共用同一份 `chipRects_`，避免「看着在这里、点不到」。绘制/命中测试都走 `EnsureChipLayouts()` 的「标签 + active」签名缓存，切组才重建布局。
10. **交互**：`Tab`/`Shift+Tab` 循环、`Ctrl+1..9` 直达、`WM_LBUTTONUP` 命中芯片即切组（这一版鼠标只做了芯片这一小块，结果行的点击在 13.7 补上）；`WM_MOUSEMOVE` + `TrackMouseEvent(TME_LEAVE)` 维护 hover 高亮。IME 组字期间 `Tab` 不切组（组字时 Everything 请求本来也不发）。
11. **验证**：`--list-filters` 打印 `enabled/active/groups` 与每组的规则（`ext=` 用 `FilterEverythingSpec()`），并顺带跑一次 `SanitizeFilterSet()`（失败打 `error=` 并返回 2）；`--filter=<名称|序号|all|off>` 覆盖本次运行（`--search-rows` / `--search-files` / `--search-live` / `--everything-check` 都生效并打印 `filter=`）；`--self-check` 的 `filters=PASS` 做默认组校验 + 每组编解码往返 + 扩展名抽检（png∈图片、exe∈应用程序、目录/无扩展名被排除、`FilterIsExtensionWhitelist()` 对含文件夹开关的组为 false）。实测：`--everything-check=photo --filter=图片` 的 `search="photo ext:png;jpg;…"`、80 条 / 22 ms；`--search-live=photo --file-root=<临时目录> --filter=图片` 只返回 `photo.png`/`photo.jpg`（同一目录 5 个 photo.* 里挑出 2 个）；合成 `WM_KEYDOWN(VK_TAB)` 后日志依次出现「筛选: 应用程序（第 2 组）-> 14 条」「筛选: 图片（第 3 组）-> 1 条」并各自触发一次 Everything 重查；设置窗侧用 `WM_COMMAND` 合成「新建/上移/删除/保存」，空组保存被拒（状态行「「筛选组7」没有任何规则…」，窗口不关），删掉空组后保存成功并写出 `filterGroup=` 六行。

## 13.6 搜索压力测试（`--bench-search`，2026-11-01）

1. **动机**：过滤/评分放在「候选打分之后」能省多少、线性度如何、内存涨到哪，光靠真实索引不好问（受磁盘、缓存、后台重建干扰）。做一个**纯内存**的合成索引，直接压 `FileIndex::Search()` 本身。
2. **注入点**：`FileIndex::LoadSyntheticForBenchmark(count, seed)` 直接填 `storage_`（目录表 / 名字 arena / 条目数组），`charMask`、`pathHash`、`lowerOffset==nameOffset` 共用名字等细节都按 `AddEntryLocked()` 的写法生成，因此走的是线上同一份「位图预筛 → 打分 → 过滤 → 排序」代码，而不是另写一遍模拟循环。名字分布：1–3 个词 + 可选数字、6% 无扩展名、4% 目录、约 12% 中文名，扩展名覆盖默认筛选组的全部类型。xorshift32 固定种子 → 可复现。
3. **计时口径**：造数据不计时；每个（查询 × 组）用例先预热一次，再跑 N 次取 min/med/p95；`limit = kMaxFileHits(80)` 与线上一致；同时打印峰值常驻内存（`K32GetProcessMemoryInfo`）与扫描吞吐（条目数 / med）。
4. **结果**（本机 100 万 / 400 万 / 800 万条，单位 ms）：`report` 8.34/31.80/63.55，`2024` 3.94/16.24/32.53，`a` 28.40/106.74/210.55，`文档` 11.36/43.76/86.85；吞吐 120–128 M/s（数字查询 245 M/s），严格线性。常驻 76MB / 244MB / 467MB（≈76 B/条），造数据 0.46/1.84/3.57 s。
5. **过滤成本**：常规查询加组 +0.1–6 ms；唯一明显的是「单字符 + 几乎命中全表」：800 万条时 `a` 从 210 → 322 ms（+112 ms）。谓词单次约 70 ns（2M 次重复调用，8/9/12 个扩展名的组几乎同速），说明成本在「取基名 + 找最后一个点」，不在扩展名比较；这也解释了为什么不需要位图/哈希加速。
6. **结论**：过滤不是热点；真实 116 万条的 22–35 ms 里，扫描/打分占大头，过滤按命中数计费（几千条 → <1 ms）。若将来要提速，方向是预筛与目录树紧凑化（减少每条的扫描/缓存开销），而不是过滤。合成数据比真实快，是因为名字更短、arena 连续、缓存局部性好，且线上那条日志还含合并层与窗口布局。
7. **防退化闸门**：`tools/regression.ps1` 默认跑 100 万条并检查 `bench_report` ≤ 60 ms、`bench_a_1char` ≤ 200 ms、`filter_check=ok`，超阈值计入失败（`-SkipBench` / `-BenchMs` / `-BenchShortMs` / `-BenchEntries` 可调；`-File` 传参用空格而不是 `=`）。阈值相对本机实测（8.3 / 26 ms）留了约 7 倍余量，只拦「数量级变慢」，避免不同 CPU 上误报。顺带修掉脚本的老问题：PowerShell 不会等待 GUI 子系统程序，`& $exe --self-check` 之后 `$LASTEXITCODE` 可能是空的（此前打印的 `self-check exit=` 就是空的，`exit $code` 也就白退 0），现在统一经 `cmd` 重定向捕获输出与退出码。

## 13.7 结果行鼠标操作与去掉窗口外框（2026-11-01）

1. **补上鼠标这条腿**：筛选组那版只让芯片吃鼠标，列表仍然只能键盘。现在 `WM_MOUSEMOVE` 维护 `hoverRow_`（浅色高亮）、`WM_SETCURSOR` 在芯片/可点行上给 `IDC_HAND`、`WM_LBUTTONDOWN` + `WM_LBUTTONUP` 完成单击启动。
2. **抬起才执行**：按下只记 `pressedRow_`，抬起时重算命中行并要求「与按下同一行」才 `ClickRow()`。原因：在按下瞬间就 `ShellExecuteEx` 的话，启动的窗口会在鼠标键仍按着时拿到前台，紧接着的抬起落进新窗口（可能触发它里面的按钮/选中）。拖到别的行或拖出面板即取消。芯片仍在按下时切组（不启动任何东西，没有这个问题）。
3. **命中与绘制同一套几何**：`Scene::RowAt(state, xDip, yDip)` 与列表绘制共用 `listTop`（有芯片时 `84+40=124`）、`rowHeight`、`rowGap`、`scroll`，行间 4 DIP 间隙不算命中；`RowAction::None` 的占位行不返回命中（既不高亮也不给手型），避免「点了个提示行什么都没发生」。`kRowHitLeft=12` 让命中区与选中/悬停圆角块的左右内缩保持一致。
4. **悬停路径不重抄数据**：`UpdateSceneState()` 会把 `rows_`（最多 200 行、每行 4 个字符串）整份拷进 `SceneState`，拿来做悬停太重。`WM_MOUSEMOVE` 只改 `sceneState_.hoverRow` / `hoverChip` 两个字段再 `InvalidateRect`；结构性变化（换查询、换组、重建行）才走 `UpdateSceneState()`，并在 `RebuildRows()` / `SelectFilter()` / `Hide()` 里把 `hoverRow_`、`pressedRow_` 清成 -1（行序号变了就失效）。
5. **回车与点击共用 `ActivateRow(index, shift, ctrl)`**：原 `VK_RETURN` 分支里的启动/打开、`usage_.Record`、失败日志整段抽出来，键盘与鼠标只差「怎么选中这一行」。`ClickRow()` 额外把选中挪到被点的行（不跑滑动动画），这样启动失败时用户能看见选中的是哪一行。日志统一成 `激活: [i] 标题 action=… target=…`，前面再加一条 `点击: [i] 标题`。
6. **面板外面那圈「框」= 系统云母（不是 1px 描边）**：先按「DWM 沿窗口矩形画的 1px 边框」修了一版（`DWMWA_BORDER_COLOR = DWMWA_COLOR_NONE`），但用户看到的框有 **20–30px**、很明显——那圈宽度恰好等于 `shadowMargin=24 DIP`（125% 缩放 = 30px），说明填满整块窗口的是**系统背景**本身：`DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_MAINWINDOW`（Mica）会把**整个窗口矩形**涂成不透明表面（暗色下就是 `#202020`），而我们的窗口为了放自绘阴影本来就比面板大一圈，于是面板外面那圈本该透明的区域被涂实，看着就是一个方框。
   - **确认手段（可复现）**：在窗口背后垫一块纯品红、无边框、6000×4000 的 WinForms 窗口（`build/magenta.ps1`，gitignored），再 `--capture` 抓窗口区域。修复前窗口边缘那一圈是 `(32,32,32)` 的死色——品红**一点都透不出来**；修复后同一圈变成 `(255,0,255) → (241,0,241) → (232,0,232) → (211,0,211)` 直到面板边缘：品红透出来了，只是被自绘阴影逐层压暗。这个对比同时排除了「阴影本身看起来像框」的可能（阴影是渐变，不是死色）。
   - **修法**：`ApplyBackdrop()` 里把 `DWMWA_SYSTEMBACKDROP_TYPE` 设为 `DWMSBT_NONE`。注意 `DWMSBT_NONE` 的值是 **1**，`0` 是 `DWMSBT_AUTO`（交给 DWM 自己挑材料，等于没关）。保留 `DWMWA_USE_IMMERSIVE_DARK_MODE`、`DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND` 与 `DWMWA_BORDER_COLOR = DWMWA_COLOR_NONE`；阴影、面板都还是自己画，面板自身的 1px 边线（`theme.border`）保留。
   - **代价**：面板 0.88 的 alpha 现在直接与桌面内容混合（以前是叠在云母的壁纸模糊上）。常规壁纸下差别肉眼很难看出来，`--glass` 诊断时会更明显——透出来的是真实窗口内容而不是壁纸模糊。
   - **教训**：`--capture` 只看「窗口区域内」的像素，如果背景本身是均匀深色，Mica 的 `#202020` 和桌面几乎同色，很容易把「Mica 涂实」误判成「窗口透明」。要验证透明必须让背后有**高对比、非均匀**的内容。
7. **验证**：沙箱里跨进程窗口自动化不通（`FindWindow` 找不到同会话里刚起的窗口，`CreateMutex` 之外还有 UI 限制），所以加了两个进程内钩子——`--hover-row=<n>` / `--click-row=<n>`（1 起）：按面板几何算出该行中心，`PostMessage(WM_MOUSEMOVE)` 与 `WM_LBUTTONDOWN/UP`，走的是和真人一样的消息处理。实测日志：`注入鼠标: 行 2 -> 客户端 (480, 273) + 单击` → `悬停: 行 2 click-me-2.log` → `点击: [1] click-me-2.log` → `激活: [1] click-me-2.log action=3 target=…` → `打开失败: … ShellExecuteEx 失败 (Win32 5)`（沙箱拒绝 shell 打开，正好验证到「点击链路」而不弹窗口）。DPI 120 / scale 1.25 下 (480,273) 落在第 2 行，说明 DIP↔像素换算正确。
8. **悬停视觉的验证走离屏渲染**：合成 `WM_MOUSEMOVE` 之后系统会立刻回一条 `WM_MOUSELEAVE`（真实光标不在窗口内），`TME_LEAVE` 一触发就清掉悬停，所以活的窗口抓不到稳定悬停帧。`--shot` 也支持 `--hover-row`，直接渲染 `SceneState::hoverRow` 出图（`docs/images/p6/hover-row.png`，920×310）：选中的第 1 行是强调色块，悬停的第 2 行是 `hoverFill`（暗色 = 白 6%，亮色 = 黑 4%）。
9. **透明边距 24 → 8 DIP**：云母关掉之后，那圈边距变成真正的「毛玻璃透明带」，但 24 DIP（125% 下 30px）宽得离谱——它本来就是给 6 层柔和阴影（最远扩散 13.2 px）留的位置。先按用户要求试到 3 DIP（贴边、4px 左右），确认「大半是边距、不是阴影」之后定在 **8 DIP**：既留出能看出投影的呼吸感，又不会让人误以为多套了一个框。阴影扩散量夹成 `std::min(layer * 2.2f, shadowMargin - 0.5f)`，边距小时阴影跟着收、不至于被窗口边缘切成硬边；面板轮廓由 `theme.border` 的 1px + 16 DIP 圆角承担。
   - 几何随之变化：窗口 = 面板 + 16 DIP（125% 下 920×550，之前 960×590；3 DIP 时是 908×538）；`--shot` / `--shot-matrix` 的画布也跟着变小（面板像素尺寸不变），仓库里 13.7 之前生成的截图仍是旧边距。
   - 角部安全：DWM 的 `DWMWCP_ROUND` 会按窗口矩形裁圆角，半径约 8–10px；面板圆角中心在 inset 8 DIP + 半径 16 DIP 处，对角线方向上离窗口角约 26px，远在裁剪弧（约 2.3–2.9px）之外，所以**面板圆角不会被 DWM 裁掉**，只有最外那条阴影带的尖角会被削一点（看不出来）。
   - 阴影与边距是耦合的：`shadowMargin` 同时决定透明带宽度和阴影扩散上限，改它即可整体放大/收紧投影；代价是边距越宽越容易看成「多套了一个框」。

## 14. 后续方向

- 子序列匹配加“跨度/间隙”惩罚，压掉 `steamapps → StreamingBar` 这类噪声；或对长查询要求连续片段。
- 目录树 + 快照紧凑化：目录改存“父目录 id + 名字”，快照随之压到 <100MB；同时可把目录 arena 做掉。
- Everything 后续：等 1.5 普及后换 SDK3（命名管道，MIT C 源码，可 vendor），同接口替换传输层；可选「按目录限定结果」与「Everything 未运行时自动回退本地索引」。
- 筛选组后续：多组同时生效（AND/OR 组合）、按大小/修改时间过滤、每组自定义快捷键；芯片行放不下时加一枚「更多」溢出芯片（现在是裁剪 + 键盘可达）。
- 鼠标后续：输入框的拖选/双击选词/右键菜单（当前鼠标覆盖芯片、结果行点击、滚轮），结果行的右键「打开所在位置 / 复制路径」，以及按下态的视觉反馈。
- USN 全盘索引；文件内容搜索。
- 打包：图标资源、便携模式、可选自签证书；真实会话回归已完成（`tools\regression.ps1` 全 PASS），后续改动重跑即可。
