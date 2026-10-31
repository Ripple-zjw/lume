# eTools

Windows 11 上的高性能启动器（Spotlight / Raycast 风格）：热键唤出、输入即搜、回车启动应用与文件。C++20 + Win32 + Direct2D/DirectWrite + DirectComposition，免安装、免管理员、单 exe。

**当前进度：P6 搜索质量修复（紧凑索引、可配置上限、精确匹配优先），P5.1 已完成。**

![自绘深色设置窗](docs/images/p5/settings-dark.png)

![磁盘缓存图标与动态高度](docs/images/p5/icons-cached.png)

## 系统要求

- **Windows 11 22H2（build 22621）及以上，x64**。只使用系统自带的云母/圆角与 DirectComposition，不提供 Win10 视觉回退。
- 免管理员、免安装；无外部运行库依赖（静态链接，UCRT 为系统组件）。
- 启动时校验系统版本，低于要求弹出明确提示并退出。

## 已实现

**P0：工具链与渲染闸门**

- 免安装 MinGW-w64 工具链获取脚本：下载、SHA256（GitHub API digest）校验、解压到 `.toolchain/`。
- 一键增量构建 `build.ps1`：资源/清单嵌入、`-static -s` 单文件；当前产物约 315KB，链接约 170ms。
- DirectComposition + D3D11 翻转模型合成交换链 + Direct2D：透明圆角窗口，真实桌面内容可透过面板（已用屏幕合成截图验证）。
- Win11 云母背景：`DWMWA_SYSTEMBACKDROP_TYPE` 在 DComp 窗口上生效。
- 离屏 PNG 自检：窗口与离屏共用同一份绘制代码。

**P1：视觉骨架 + 唤出链路**

- 模块化拆分：`core/`、`platform/`、`ui/`、`app/`（见下方目录）。
- 主题 token 化：暗/亮/跟随系统；尺寸、颜色、字号集中定义；默认强调色为设计蓝 `#6EA8FF`，可 `--accent=system` 跟随系统。
- 系统版本门禁（build ≥ 22621）、Per-Monitor V2 DPI、按光标显示器定位（水平居中、垂直 18%）。
- 全局热键 `Alt+Space`（冲突时自动尝试 `Ctrl+Alt+Space`、`Ctrl+Shift+Space`）；失焦自动隐藏、Esc 隐藏。
- 单实例：重复启动会把已有实例唤到前台。
- 隐藏预热渲染 + 同步出帧：**显示→首帧实测 13–14ms**（ShowWindow ~12ms + 绘制 ~1.7ms）。
- 3 DPI × 2 主题基线矩阵。

**P2：输入 / IME / 列表交互 / 动效**

- 自绘单行输入：UTF-16 码点级左右移动、退格/删除、Home/End、Ctrl+V 粘贴（清洗换行）、光标闪烁。
- IME 接入：`WM_IME_START/COMPOSITION/ENDCOMPOSITION`、`GCS_COMPSTR`/`GCS_RESULTSTR`/`GCS_CURSORPOS`；组字串带下划线渲染，`ImmSetCompositionWindow(CFS_POINT)` 跟随光标定位候选窗，`ImmSetCompositionFontW` 同步字号。
- 虚拟化列表：12 条样例、可见 6 行、滚动窗口只绘制可见行；↑↓/PgUp/PgDn/Home/End 选择并自动滚动。
- 键盘语义：Enter 记录启动（P3 接真实启动）、Shift+Enter/Ctrl+Enter 预留、Esc 取消组字或隐藏。
- 动效：唤出 90ms 淡入、选中迁移 120ms 缓动、结果刷新 80ms 淡入；QPC 帧计时器按需启停。
- 资源缓存：查询布局按「显示文本 + 组字区间」缓存，行布局按内容签名缓存，光标/组字只在变化时重建。
- 自动化验证钩子：`--type`（输入）、`--keys`（按键序列）、`--compose`（注入组字）、`--capture-ms`（任意时刻截图）。
- 3 DPI × 2 主题基线 + 输入/组字/淡入状态截图：`docs/images/p2/`。

**P3：应用索引 / 评分 / 真实启动 / 图标**

- 索引三源合并：开始菜单 `.lnk`（`IShellLinkW` 解析真实目标）、`shell:AppsFolder`（`IPropertyStore` 读 `PKEY_AppUserModel_ID`，覆盖商店应用）、注册表 `App Paths`；按标题与解析后目标双重去重。
- 磁盘缓存：`%LOCALAPPDATA%\eTools\apps.idx`（不可写时退回 exe 同目录），开始菜单目录有更新则自动重建；实测 **首次 650ms → 缓存 1.1ms**。
- 模糊评分：子串命中（前缀加分）、子序列命中（连续/词首加分）、长度惩罚；`--list-apps` 可直接检查索引内容。
- 真实启动：Shell 条目走 `ShellExecuteEx`（`.lnk` 保留参数/工作目录/清单），商店应用走 `IApplicationActivationManager::ActivateApplication`（失败回退 `shell:AppsFolder\<AUMID>`）；`Ctrl+Enter` 以管理员运行，`Shift+Enter` 打开所在位置。
- shell 图标：`IShellItemImageFactory` 取 32px → WIC 转 `32bppPBGRA` → D2D 位图，内存缓存（失败也缓存避免重试），无图标时回退首字色块。
- 过滤结果实时刷新，空查询按名称列出全部应用；无匹配时显示占位行。

**P4：文件/文件夹搜索**

- 默认索引桌面、文档、下载；`FindFirstFileExW(FIND_FIRST_EX_LARGE_FETCH)` 多线程枚举，跳过 reparse point 与 `.git/node_modules` 等噪声目录，上限 20 万条。
- 二进制快照 `files.idx`（AppData 不可写时自动退回 exe 同目录）；仅 `--file-stats`/`--shot` 等诊断模式加载，常驻启动会删除旧快照并全量重建（不再落盘）。
- 每个根目录 `ReadDirectoryChangesW` 递归监听：新增文件立即入索引、删除即摘除、目录/重命名触发防抖重建；窗口 1s 内自动刷新结果。
- 查询与应用共用评分器（文件名命中加权，路径命中降权）；实测 **0.4–0.6ms**（1361 文件 / 228 应用），路径直通输入可直接打开。
- 异步图标：worker 取 shell 位图、UI 线程转 D2D，先画首字色块兜底；查询延迟从 56.7ms 降到 0.6ms。
- `--list-files` / `--file-stats` / `--file-root` 可检查索引；`--open` 可单独验证打开路径。

**P5：托盘、设置、自启、动态高度、MRU**

- 托盘图标 + 右键菜单（显示 / 开机自启 / 设置… / 退出），左键切换窗口；资源管理器重启后通过 `TaskbarCreated` 自动重挂。
- 设置窗（原生控件）：主题（跟随系统/深色/浅色）、强调色（设计蓝/系统）、热键预设（Alt+Space / Ctrl+Alt+Space / Ctrl+Shift+Space）、开机自启、文件索引目录增删（文件夹选择器）。
- 设置持久化 `settings.conf`（UTF-8 key=value，AppData 不可写时退回 exe 同目录）；改动即时应用：主题/强调色重绘、热键重新注册、文件索引按新根目录重建。
- 开机自启写 `HKCU\...\Run`（命令 `"eTools.exe" --autostart`）。
- 动态面板高度：结果 1–6 行时窗口高度跟随收缩/展开，不再固定留白。
- MRU：`usage.conf` 记录目标的使用次数与最近时间；空查询按常用/最近排序，非空查询叠加 0–200 分。
- 修复：多份索引快照并存时，按“根目录匹配”逐个尝试，而不是读到第一份就放弃。
- 说明：本 agent 沙箱内 `Shell_NotifyIcon` 与 `HKCU\...\Run` 写入被系统拒绝（托盘/自启代码路径需在真实会话验证）；设置窗仍是原生浅色控件，深色主题留作 P5.1。

**P5.1：图标磁盘缓存 / 深色设置窗 / 自定义热键 / 自检**

- 图标磁盘缓存：worker 先查 `%LOCALAPPDATA%\eTools\icons\<hash>.png`（不可写时 exe 同目录），未命中才走 shell 取图并写回 PNG；`Warm()` 启动预热、异步队列走同一路径。实测冷启动 **12 次 shell 取图、首帧 29.4ms**，二次启动 **12 次磁盘命中、0 次 shell、首帧 15.6ms**。
- 设置窗自绘深色：统一 `#202024` 背景/白色文字，主题与强调色改为自绘循环按钮（跟随系统 → 深色 → 浅色），文件目录列表与按钮全部深色，去掉原生 ComboBox 的白色箭头块。
- 自定义热键：新增 `ParseHotkey`/`FormatHotkey`（Ctrl/Alt/Shift/Win + 字母/数字/F1–F24/方向键等），设置窗内点击热键框后直接按下组合键捕获，并用 `RegisterHotKey` 预检冲突；配置里可写任意组合（如 `Ctrl+Alt+K`）。
- 捕获期间用 `WH_KEYBOARD_LL` 低级钩子取原始按键，**修复 Ctrl+Space 被输入法吞掉、只显示 Ctrl 的问题**；同时放行 Alt+Tab / Alt+F4 / Esc，避免捕获时卡住系统快捷键。实测 Ctrl+Space、Ctrl+Alt+K 均能正确捕获。
- `--hotkey=<combo>` 可临时覆盖热键（不影响已保存设置），用于验证解析与注册。
- `--self-check`：一次性检查热键注册、托盘、自启读写、文件打开、设置写入并输出 `PASS/FAIL`；配合 `tools/regression.ps1` 在**真实桌面会话**做回归。
- 真实会话自检（2026-10-31，Windows 11 25H2 26200）：`hotkey/tray/autostart/open/settings_save` 全部 PASS；沙箱里 tray/autostart/open 会失败属环境限制。

**P6（进行中）：搜索质量修复**

![层级排序：game](docs/images/p6/game-ranking.png)

![精确命中：AlanWake2.exe](docs/images/p6/alanwake2.png)

![设置窗里的索引上限（默认 100 万）](docs/images/p6/settings-cap.png)

![应用索引里的 Notepad（C 盘）与文件索引（F:/E:）](docs/images/p6/app-index-c-drive.png)

- **应用索引与文件索引是两套**：`apps.idx` 来自开始菜单/AppsFolder/App Paths，天然包含 C 盘的系统应用；`files.idx` 只扫设置里的根目录（当前 **C:/F:/E:/D:**）。所以搜 `notepad` 第一条是 Notepad 应用，副标题显示 `应用 · C:\Program Files\WindowsApps\...`，下面的才是文件索引结果。启动日志会逐条打印 `文件索引根: C:\ / F:\ / E:\ / D:\`。
- 注意：`--file-stats` / `--list-files` 在未指定 `--file-root` 时使用默认的桌面/文档/下载（C 盘），这是诊断命令的默认值，不是常驻应用实际使用的根目录；要验证应用索引请用 `--search-live=<query>` 或 `--file-root=`。
- **快照优先 + 后台重建**：启动先读 `files.idx`（72 万条实测 **875ms** 可用），随后后台全量重建（C/E/F/D，每根 25 万上限，约 **13.8s**）。启动不再删除或重写快照；只有首次运行或手动“清除索引并重建”才写回。重建完成后不会因为写快照触发自己的 watcher（已忽略 `files.idx/.tmp` 等），目录高频变化有 60s 冷却，退出时重建可取消、快照用临时文件原子替换。
- **手动清除**：设置窗新增“清除索引并重建”按钮，删除快照、清空内存索引并立即重建；`--file-stats` 也会重写快照。
- **紧凑索引**：目录表 + 文件名 arena；47.2 万条时索引 83MB、进程 123MB / 峰值 149MB。加上 C 盘后 72.2 万条：进程 **273MB 常驻**；无快照首次重建峰值 **335MB**，读快照后再重建（新旧两份索引并存）峰值约 **486MB**。
- **上限可改，默认 100 万**：设置窗“索引上限”输入框（20,000–1,000,000），也可写 `settings.conf` 的 `fileCap=` 或 `--file-cap`；旧默认值自动迁移到 100 万。
- **匹配质量优先**：`quality`（5 直接路径 / 4 字面完全一致 / 3 忽略扩展名一致 / 2 前缀 / 1 子串 / 0 模糊或路径）→ 类型 tier（应用、可执行文件 → 其他文件 → 目录 → 仅路径）→ 分数。`game` 先出 20 个 `Game` 目录（短路径优先）；`AlanWake2.exe` 精确命中。
- **路径命中只做最后兜底**：仅当文件名完全没有结果时才扫路径，分数 `÷6` 且最多 2000 条。
- 查询实测（72.2 万条）：`AlanWake2.exe` **4.2ms**。
- **存储占用**（本机 72.2 万条）：`files.idx` **170.0MB**（178,303,881 字节）、`apps.idx` **48KB**、日志/图标/配置合计 **0.83MB**，总计约 **171MB**。沙箱写不进 AppData 时快照落在 exe 同目录；正常会话在 `%LOCALAPPDATA%\eTools\files.idx`。
- 快照读取上限 512MB；同分排序、`--search-files/--search-rows/--search-live` 调试命令保持不变。
- 仍未解决：子序列匹配仍可能产生 `steamapps → StreamingBar` 这类噪声；非精确目录仍排在文件之后（字面完全一致的目录已置顶）；47 万条约 123MB，目录路径仍以两份 `wstring` 保存，下一步可做目录 arena 再压 ~20MB。

## 构建

```powershell
# 仅首次：下载并校验 MinGW-w64（264MB，解压约 1.5GB 到 .toolchain/，不写入系统 PATH）
node tools/fetch-toolchain.mjs

# 构建（执行策略受限时用 Bypass 形式；脚本输出为 ASCII，避免 PS 5.1 编码问题）
powershell -ExecutionPolicy Bypass -File .\build.ps1

# 调试构建 / 测试 / 构建后启动
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Debug
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Test
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Run
```

## 运行

```powershell
.\build\eTools.exe                       # 常驻：按 Alt+Space 唤出/隐藏
.\build\eTools.exe --show --ms=3000      # 直接显示 3 秒（便于测试）
.\build\eTools.exe --capture=cap.png     # 显示并截取真实合成结果
.\build\eTools.exe --shot=out.png --scale=1.5 --theme=light
.\build\eTools.exe --shot-matrix=docs\images\p2   # 生成 3×2 基线矩阵
.\build\eTools.exe --type=cod --keys=down,down    # 输入与选择（配合 --capture）
.\build\eTools.exe --list-apps                    # 打印真实应用索引（UTF-8，每行一条）
```

| 参数 | 说明 |
| --- | --- |
| `--show` | 启动后立即显示（否则等待热键） |
| `--ms=<n>` | 启动后 n 毫秒自动退出 |
| `--capture=<png>` | 显示约 0.45s 后截取窗口区域（含 DWM 合成结果） |
| `--shot=<png>` | 离屏渲染（模拟壁纸），不创建窗口 |
| `--shot-matrix=<dir>` | 生成 3 DPI × 2 主题基线 |
| `--theme=dark\|light\|system` | 主题，默认跟随系统 |
| `--accent=system\|#RRGGBB` | 强调色，默认设计蓝 `#6EA8FF` |
| `--query=<text>` | 预填输入框文字 |
| `--scale=<factor>` | 截图缩放（1 = 100%） |
| `--glass` | 面板不透明度降到 0.35，检查桌面透出 |
| `--type=<text>` | 显示后依次注入字符（输入验证） |
| `--keys=up,down,enter` | 显示后注入按键序列（支持 up/down/left/right/home/end/pgup/pgdn/enter/esc/back/delete） |
| `--compose=<text>` | 注入 IME 组字串，验证组字渲染 |
| `--capture-ms=<n>` | 截图延迟（默认 450；用 30 观察淡入帧） |
| `--selected=<n>` | 离屏截图时预选中第 n 行 |
| `--list-apps` | 打印应用索引（UTF-8，每行一条） |
| `--list-files` | 重建文件索引并打印统计与前 20 条路径 |
| `--file-stats` | 重建文件索引并只打印统计 |
| `--file-root=<dir>` | 覆盖文件索引根目录（测试用） |
| `--open=<path>` | 单独验证打开文件/路径 |
| `--set-autostart=1\|0` | 设置开机自启并打印状态 |
| `--hotkey=<combo>` | 临时覆盖热键（如 `Ctrl+Alt+K`） |
| `--test-capture=<combo>` | 测试用：打开设置窗并合成组合键，验证捕获 |
| `--search-files=<query>` | 搜索文件索引并打印耗时与前 20 条（分数+路径） |
| `--search-rows=<query>` | 打印合并后的 tier/分数/标题/路径（验证排序） |
| `--file-cap=<n>` | 覆盖索引总上限（20k–1M，测试/调优） |
| `--search-live=<query>` | 现场重建索引并搜索（诊断上限/排序） |
| `--settings` | 启动时打开设置窗 |
| `--self-check` | 检查热键/托盘/自启/打开文件/设置写入并打印 PASS/FAIL |
| `--no-hotkey` | 不注册全局热键 |
| `--log=<path>` | 指定日志文件（默认 `%LOCALAPPDATA%\eTools\logs`，不可写时退回 exe 同目录） |

## 目录

```
src/main.cpp               CLI 解析与入口分发
src/app/                   常驻应用：单实例、热键、显示/隐藏、截图模式、测试注入
src/app/settings.cpp       设置读写（settings.conf，多路径回退）
src/ui/window.cpp          窗口、托盘、设置入口、定位/激活、输入与 IME、键盘、动效、动态高度
src/ui/settings_window.cpp 设置窗（主题/强调色/热键/自启/索引目录）
src/ui/input.cpp           单行输入状态（码点移动、粘贴、组字）
src/ui/renderer.cpp        D3D11/DXGI/DirectComposition/D2D/DWrite/WIC 与出图
src/ui/scene.cpp           面板绘制（阴影、输入、虚拟化列表、组字下划线、动画 Alpha）
src/ui/theme.cpp           暗/亮主题 token、强调色解析
src/platform/system.cpp    系统版本门禁、DPI、工作区、暗色/强调色读取
src/platform/hotkey.cpp    全局热键注册与预设组合
src/platform/autostart.cpp 开机自启注册表读写
src/platform/tray.cpp      托盘图标与右键菜单
src/search/app_entry.h     应用条目模型与索引/过滤接口
src/search/app_index.cpp   开始菜单/AppsFolder/App Paths 收集、去重、磁盘缓存
src/search/scorer.cpp      模糊评分与过滤
src/search/file_index.cpp  文件枚举、快照、watcher、文件检索
src/search/merge.cpp       应用/文件/路径直通合并排序
src/search/usage.cpp       使用频次/最近性（usage.conf）
src/search/result.h        显示层结果行
src/ui/icon_cache.cpp      异步 shell 图标加载（worker 取图 + UI 转 D2D）
src/launch/launcher.cpp    ShellExecuteEx / UWP 激活 / 定位 / 管理员运行 / 打开路径
src/core/log.*             日志（文件/调试器/控制台，UTF-8）与通用工具
assets/app.rc              版本/清单资源
assets/app.manifest        Per-Monitor V2、长路径、UTF-8、asInvoker
tools/fetch-toolchain.mjs  工具链获取与校验（锁定版本）
tools/regression.ps1       真实桌面会话回归（托盘/自启/热键/文件打开）
build.ps1                  增量构建脚本
docs/DESIGN.md             架构决策、实测数据、视觉 token
docs/images/p2/            P2 设计基线与交互状态截图
docs/images/p3/            P3 真实索引/图标/选中态截图
docs/images/p4/            P4 文件搜索/watcher/窗口截图
docs/images/p5/            P5 动态高度截图
```

## 已知事项

- 链接时 binutils 2.47 会输出 `.rsrc merge failure: multiple non-default manifests`，属**非致命诊断**：清单与版本资源均已正确嵌入（`permonitorv2` 字符串可从 exe 中检出）。
- `build.ps1` 需以 `-ExecutionPolicy Bypass` 运行，或自行调整本机执行策略。
- 探针时代的 `--mica` 参数已移除（云母默认开启）；`--shot` 不校验系统版本，便于任意机器生成设计图。
- 组字渲染已通过注入验证，**真实微软拼音/搜狗输入法的候选窗位置仍需人工确认**（自动化无法安全驱动系统 IME）。
- 云母背景下 `--glass` 看到的是壁纸模糊底而非桌面窗口内容，属预期。
- **UWP 启动无法在本 agent 沙箱内验证**：`ActivateApplication` 与 `shell:AppsFolder\<AUMID>` 都被系统拒绝（Win32 5），连 PowerShell `Start-Process` 同样失败，属环境限制；普通 exe/.lnk 启动已实测成功。请在真实会话里用「记事本/计算器」确认。
- **用 shell 打开文件**：沙箱内 `.txt` 返回 Win32 5、`.exe` 返回 1223（取消）；**真实会话已通过**（`open=PASS`，默认编辑器成功打开自检文件）。
- 文件索引只覆盖桌面/文档/下载（可 `--file-root` 覆盖），不是全盘；全盘秒搜需要 USN 日志/服务（管理员），与免安装定位冲突，后续可选接 Everything。
- **托盘与开机自启**：在 agent 沙箱内会被系统拒绝（`Shell_NotifyIcon` 返回失败、`HKCU\...\Run` 不可写），但**真实桌面会话已实测通过**（`regression.ps1` 五项全 PASS），不影响正常使用。
- 设置窗已改为自绘深色（主题/强调色循环按钮、热键捕获、深色列表与按钮）；设置保存路径优先 `%APPDATA%\eTools\settings.conf`，不可写时退回 exe 同目录。
- 图标缓存位于 `%LOCALAPPDATA%\eTools\icons`（沙箱等不可写环境退回 exe 同目录 `icons\`），按解析名哈希存 PNG；删掉目录会重新取图。
- 动态面板高度已实现（1–6 行）；全盘/更多根目录的设置项、文件内容搜索、Everything 适配器仍未做。
- **真实会话回归**：`powershell -ExecutionPolicy Bypass -File tools\regression.ps1` 已跑通（exit=0）；后续改托盘/自启/启动链路时重跑即可。人工项（托盘菜单、设置重启生效、注销后自启）仍建议过一遍。
- 唤出淡入通过场景整体 alpha 实现（mingw 的 `dcomp.h` 漏声明 `IDCompositionVisual::SetOpacity`，未用 ABI hack）。

## 路线

| 阶段 | 内容 |
| --- | --- |
| P0 ✅ | 工具链、构建、DComp/D2D 透明窗口、离屏自检 |
| P1 ✅ | 模块化、主题 token、热键/定位/显隐、单实例、系统门禁、多 DPI 基线 |
| P2 ✅ | 自绘输入 + IME、虚拟列表、键盘/滚动、唤出与选中动效、布局缓存 |
| P3 ✅ | 应用索引（开始菜单/AppsFolder/App Paths）、评分、真实启动、shell 图标、索引缓存 |
| P4 ✅ | 文件/文件夹索引、快照、watcher 增量、路径直通、异步图标 |
| P5 ✅ | 托盘/设置窗/自启、动态高度、MRU 排序、快照多路径修复 |
| P5.1 ✅ | 图标磁盘缓存、深色自绘设置窗、自定义热键、真实会话自检全 PASS |
| P6（进行中） | 搜索质量修复（配额/路径降权/稳定排序/预筛）、全盘搜索（Everything 可选适配器）、文件内容搜索、打包与签名 |
