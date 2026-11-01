# Everything SDK (vendored)

Everything 官方 SDK 的**客户端源码**，用于把"全盘文件搜索"作为 Lume 的可选文件搜索来源。
这里只放编译 Lume 所需的三个文件，全部**逐字节未修改**。

## 来源与校验

| 项 | 值 |
| --- | --- |
| 下载地址 | https://www.voidtools.com/Everything-SDK.zip |
| SDK 版本 | Everything SDK v2（`Everything.h` 中 `EVERYTHING_SDK_VERSION` = 2，对应 Everything 1.4+） |
| 整包大小 | 238,231 bytes |
| 整包 SHA256 | `F5716D9513CCE6B462B5170A0A2E7E081E191D9BCAC6774F5DECC061497DF443` |

| 本地路径 | 来源条目 | 大小 | SHA256 |
| --- | --- | --- | --- |
| `include/Everything.h` | `include/Everything.h` | 15,300 | `09CBE00C0E2B024CE49EC098BACFC4272F1D998F334E19E1D427AA8357D3AF78` |
| `ipc/Everything_IPC.h` | `ipc/everything_ipc.h` | 50,671 | `A9DA767356AD89F676AE142A3FA611F529AE680CF79759170E6C6336D10722C9` |
| `src/Everything.c` | `src/Everything.c` | 71,123 | `691FD71D4BAEB99774DC0F6B0BED35218684DA5C2F36FE02E73FF5C1E11B6BF2` |

`ipc/` 里的文件只改了**文件名大小写**（`everything_ipc.h` → `Everything_IPC.h`），因为
`Everything.c` 里写的是 `#include "../ipc/Everything_IPC.h"`；NTFS 不区分大小写，改名后既能在
Windows 上原样编译，将来放到大小写敏感的 CI 上也不会断。文件内容与官方完全一致（可用上面的
SHA256 核对：解压官方包后对 `ipc/everything_ipc.h` 取 SHA256 与此表比较）。

## 为什么编进 exe，而不是带 `Everything64.dll`

官方提供两种形态：`dll/Everything64.dll`（+ `lib/Everything64.lib` 导入库）或 `src/Everything.c`
（DLL 的完整源码，MIT 式许可）。本项目选择**编源码**：

- 保持 README 承诺的「免安装单 exe」，发布包不新增文件；
- 没有"找不到 DLL / 位数不匹配 → 功能降级"的运行期失败模式；
- 协议实现由官方代码承担，我们自己不写 IPC。

`lib/Everything64.lib` 是导入库而不是静态库，链接它之后 exe 仍然运行时依赖 DLL，因此不使用。

## 构建方式

`build.ps1` 把 `src/Everything.c` 当**第三方源码**编译：

- 用 `gcc`（不是 g++）以 C 语义编译；`-w` 关掉全部警告（源码里的 MSVC `#pragma warning(disable : 4996)`
  与 `stricmp` 弃用告警都在这层消化），不加 `-Werror`；
- 第一方 TU 额外定义 `-DEVERYTHINGUSERAPI=`，让 `Everything.h` 里的
  `#ifndef EVERYTHINGUSERAPI → __declspec(dllimport)` 归零，否则静态链接会出现 `__imp_Everything_*` 未定义；
- `Everything.c` 自己 `#define EVERYTHINGUSERAPI __declspec(dllexport)`，在 exe 里只是导出符号，**不需要打补丁**；
- 增量构建同时扫描 `src/` 与 `third_party/` 的头文件时间戳。

## 使用前提

SDK 只是 Everything 的 IPC 客户端包装：**必须有一个正在运行的 Everything（1.4/1.5）**。
未运行时 `Everything_GetMajorVersion()` 返回 0，Lume 会把它显示成"未检测到正在运行的 Everything"并降级为
只搜应用，不会崩溃、也不会删本地快照。

## 升级步骤

1. 下载新版 `Everything-SDK.zip`，记录新的整包 SHA256；
2. 更新 `tools/fetch-everything-sdk.mjs` 里的 `SDK_URL` / `SDK_SHA256`；
3. `node tools/fetch-everything-sdk.mjs` 重新解出这三个文件；
4. 更新本文件的版本、大小、SHA256 表；
5. 重新构建并在真实会话里跑 `Lume.exe --everything-check`。

## 有意未入库的部分

`dll/`、`lib/`、`example/`、`vs/`、`src/*.def` 都不需要（改用"DLL 随包"方案时才需要 `dll/Everything64.dll`），
`tools/fetch-everything-sdk.mjs` 也只解出上表三个文件。许可全文见同目录 `LICENSE.txt`。
