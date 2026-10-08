# TIHelper

以 **TrustedInstaller** 身份删除文件 / 执行程序的轻量 C 接口 DLL。
只依赖 QtCore 的导出宏，**不链接任何 Qt 运行库**，因此 Python `ctypes`、
C# `DllImport`、原生 C++ 宿主都能直接加载。

- Qt 版本：**6.12**（源码同时兼容 6.2+ / Qt 5.15，只用 `<QtCore/qglobal.h>`）
- 语言标准：C++17
- 平台：Windows 7 及以上（`_WIN32_WINNT=0x0601`）

---

## 1. 构建

用 Qt Creator 打开 `TIHelper.pro`，或命令行：

```bat
qmake TIHelper.pro -spec win32-g++
mingw32-make -j4
```

MSVC：

```bat
qmake TIHelper.pro -spec win32-msvc
nmake
```

产物为 `TIHelper.dll`（固定文件名，不带版本号后缀）。

### 与 czl_installer 同一个会话

两个工程由顶层 `../czl_suite.pro` 组成一个 subdirs 工程，
在 Qt Creator 里打开它即可同时看到 `TIHelper` 与 `czl_installer`：

```bat
qmake ..\czl_suite.pro -spec win32-g++
mingw32-make -j4          :: 先构建 TIHelper，再构建 czl_installer
```

`TIHelper.pro` 里的 `QMAKE_POST_LINK` 会在构建完成后**自动把
`TIHelper.dll` 拷到 `../czl_installer/`**，因此不需要手工拷贝：

```
[TIHelper] copied TIHelper.dll to E:/codes/cpp/czl_installer
```

拷贝目标可覆盖：

```bat
qmake "TIHELPER_CONSUMER_DIR=D:/other/czl_installer" TIHelper.pro
```

随后 `czl_installer` 构建时会把该 DLL 再拷到自己的 exe 输出目录，
最终由 `czl_installer/deploy/deploy_to_dist.bat` 收进 `dist/`。

### 验证

**建议的验证方式**（确认导出符号与 UAC 清单都在）：

```bat
objdump -p TIHelper.dll | findstr /C:"ti_"
```

应看到 4 个未修饰的 C 符号：`ti_delete_path`、`ti_execute_file`、
`ti_execute_file_args`、`ti_helper_abi_version`。

---

## 2. 导出接口

完整声明见 `tihelper.h`。

```c
/* 接口 ABI 版本，当前为 2 */
int  ti_helper_abi_version(void);

/* 以 TI 权限删除文件或目录（目录递归删除）。path 为 UTF-8 */
int  ti_delete_path(const char *path);

/* 以 TI 权限执行文件。exe_path 为 UTF-8 */
int  ti_execute_file(const char *exe_path);

/* 以 TI 权限执行文件并附加参数（新增，推荐）。args 可为 NULL */
int  ti_execute_file_args(const char *exe_path, const char *args);
```

### 返回值约定

| 返回值 | 含义 |
| ------ | ---- |
| `0`    | 成功 |
| `-1`   | 参数非法（空指针 / 空串 / UTF-8 解码失败） |
| `-2`   | 路径不存在 |
| `-3`   | 权限不足（取所有权与提权均失败） |
| `-4`   | 无法创建子进程 |
| `> 0`  | 子进程真实退出码（仅 `ti_execute_*` 系列） |

> 旧版只有 `0 / -1` 两种返回值，调用方无法区分「路径不存在」「没权限」
> 「子进程失败」，现在可以按需分别处理。

---

## 3. 调用示例

### Python

```python
import ctypes, os

dll = ctypes.CDLL(os.path.join(os.path.dirname(__file__), "TIHelper.dll"))

dll.ti_delete_path.argtypes = [ctypes.c_char_p]
dll.ti_delete_path.restype = ctypes.c_int

dll.ti_execute_file_args.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
dll.ti_execute_file_args.restype = ctypes.c_int

# 删除文件或目录（路径必须 UTF-8 编码）
ret = dll.ti_delete_path(r"C:\Windows\Temp\TestFolder".encode("utf-8"))
print("ti_delete_path:", ret)

# 以 TI 权限执行并带参数（参数串由 DLL 负责加引号）
ret = dll.ti_execute_file_args(
    r"C:\Tools\setup.exe".encode("utf-8"),
    b"/quiet /norestart",
)
print("ti_execute_file_args:", ret)
```

### C#

```csharp
using System;
using System.Runtime.InteropServices;
using System.Text;

class Program
{
    [DllImport("TIHelper.dll", CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "ti_delete_path")]
    private static extern int TiDeletePath(byte[] pathUtf8);

    [DllImport("TIHelper.dll", CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "ti_execute_file_args")]
    private static extern int TiExecuteFileArgs(byte[] exePathUtf8, byte[] argsUtf8);

    static void Main()
    {
        // 路径可能含中文时务必传 UTF-8 字节数组，
        // 用 CharSet.Ansi 会按系统 ANSI 编码转换而丢掉非 ANSI 字符。
        int ret = TiDeletePath(Encoding.UTF8.GetBytes(@"C:\测试目录\文件.txt"));
        Console.WriteLine($"ti_delete_path: {ret}");

        ret = TiExecuteFileArgs(
            Encoding.UTF8.GetBytes(@"C:\Tools\setup.exe"),
            Encoding.UTF8.GetBytes("/quiet /norestart"));
        Console.WriteLine($"ti_execute_file_args: {ret}");
    }
}
```

### 原生 C++

```cpp
#include <cstdio>
#include <windows.h>
#include "tihelper.h"

int main()
{
    HMODULE dll = LoadLibraryW(L"TIHelper.dll");
    if (!dll) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }

    auto del = reinterpret_cast<decltype(&ti_delete_path)>(
        GetProcAddress(dll, "ti_delete_path"));
    auto exec = reinterpret_cast<decltype(&ti_execute_file_args)>(
        GetProcAddress(dll, "ti_execute_file_args"));

    if (!del || !exec) { printf("GetProcAddress failed\n"); FreeLibrary(dll); return 1; }

    printf("abi=%d\n", reinterpret_cast<int(*)()>(
        GetProcAddress(dll, "ti_helper_abi_version"))());

    printf("delete=%d\n", del("C:\\Windows\\Temp\\TestFolder"));
    printf("exec=%d\n", exec("C:\\Tools\\setup.exe", "/quiet /norestart"));

    FreeLibrary(dll);
    return 0;
}
```

---

## 4. UAC 清单

`TIHelper.manifest` 声明 `requireAdministrator` 并已嵌入 DLL。

**注意**：清单只对 **exe 宿主**生效。用 `python`、`rundll32` 或
`LoadLibrary` 加载本 DLL 时，宿主自身的权限不会因这份清单而提升 ——
此时 `SeDebugPrivilege` 拿不到，DLL 只能以调用方身份执行。

---

## 5. 本次改造修正的问题

### 5.1 UAC 清单实际处于失效状态

```xml
<!-- 旧版：第三段被拼成了 "asm vener3"（v 后的点变成空格） -->
<trustInfo xmlns="urn:schemas-microsoft-com:asm vener3">
```

命名空间非法，Windows 会忽略整段 `trustInfo`，`requireAdministrator`
从未生效。另外旧版缺少 `<assemblyIdentity>`，部分 Windows 版本会直接
拒绝整份清单。现已修正命名空间、补上 `assemblyIdentity`、`compatibility`
（Vista→Win11）与 `longPathAware`。

### 5.2 特权启用结果被误判为成功

`AdjustTokenPrivileges` 即使只部分成功也返回 `TRUE`。旧版：

```cpp
bool ok = (AdjustTokenPrivileges(...) != FALSE);   // 漏掉了 GetLastError()
```

正确做法是再检查 `GetLastError() == ERROR_NOT_ALL_ASSIGNED`，否则
`SeDebugPrivilege` 没真正拿到也当成功了，后面偷令牌必然失败且无从排查。

### 5.3 命令行拼接导致的注入与路径截断

旧版把路径直接拼进命令串：

```cpp
std::wstring cmd = L"cmd.exe /c takeown /f \"" + path + L"\" /r /d Y";
run_as_ti(cmd.c_str());          // CreateProcessW 会把整串当 lpCommandLine
```

- 路径含 `&`、`|`、`^`、`(` 时会被 `cmd.exe` 当成控制符执行；
- 路径含 `"` 时命令被提前截断；
- `cmd /c "带空格的程序" 参数` 还会被 `cmd` 的引号剥离规则改写。

现在：删除用原生 `DeleteFileW` / `RemoveDirectoryW`（不经 shell），
执行用 `lpApplicationName` + 独立的 `lpCommandLine`，新增的
`ti_execute_file_args` 把参数按 Windows 规则加引号，`cmd` 仅在最后兜底。

### 5.4 只读属性导致删除必定失败

`DeleteFileW` 对带 `FILE_ATTRIBUTE_READONLY` 的文件一定返回
`ERROR_ACCESS_DENIED`。旧版从未清除该属性。现在夺权后递归清除
只读 / 隐藏 / 系统属性，并处理长路径（`\\?\` 前缀）。

### 5.5 依赖 `takeown` / `icacls` 的本地化名称

旧版通过 `icacls ... /grant administrators:F` 授权，在中文 / 日文系统上
`administrators` 这个名称并不存在，命令静默失败，调用方却收到 `true`。
现在直接用 `SetNamedSecurityInfoW` 改 owner 与 DACL，被授权者写成
**SID**（当前用户 SID + TrustedInstaller SID），与系统语言无关。

### 5.6 TrustedInstaller 未运行时直接放弃

旧版 `find_trusted_installer_pid()` 找不到进程就返回 0，随即降级为
管理员身份执行 —— 明明有权限却拿不到 TI 令牌。现在会主动
`StartServiceW` 启动 `TrustedInstaller` 服务并等待，最多 15 秒。

### 5.7 `LOGON_WITH_PROFILE` 单点失败

服务令牌通常没有可加载的用户配置文件，`CreateProcessWithTokenW`
配 `LOGON_WITH_PROFILE` 会失败。现在按
`LOGON_WITH_PROFILE` → `LOGON_NETCREDENTIALS_ONLY` → `0` 依次退让。

### 5.8 执行 `exe` 时顺手改了目标文件的所有权

旧版 `ti_execute_file()` 也会先调用
`take_ownership_and_grant_full_control()`。执行只需要读/执行权限，
改动目标文件的属主与 ACL 是没有必要的副作用（并把文件永久暴露给
TrustedInstaller）。现已移除。

### 5.9 其他

- `TIHelper_global.h` 定义的是 `TIHELPER_EXPORT`，而 `tihelper.h` 用的是
  `TIHELPER_API`，两个头文件互不相干；已删除无用的 `TIHelper_global.h`，
  导出宏集中在 `tihelper.h`。
- 返回 `PSID` 的 `ConvertStringSidToSidW` 每次调用都会泄漏；现在用
  `static` 只转换一次并全程复用。
- `query_service_state()`、`process_is_elevated()` 等定义后从未使用的函数
  会让 `-Wall -Wextra` 报警；已清理，当前 **0 warning**。
- 删除彻底失败时增加 `MoveFileExW(..., MOVEFILE_DELAY_UNTIL_REBOOT)`
  兜底，重启后自动清理，并返回成功。

---

## 6. 与本 DLL 配合的调用方

`czl_installer`（同目录仓库）通过 `core/TIHelperWrapper.*` 使用本 DLL：
`ti_execute_file_args` 缺失时会自动回退到旧版 `ti_execute_file`
（此时安装参数被忽略并打印警告），因此新程序可以兼容旧 DLL。
