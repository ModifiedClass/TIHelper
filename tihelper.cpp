#include "tihelper.h"

// Windows 核心头文件，提供基础 API
#include <windows.h>
// ACL 相关 API：SetFileSecurity、SetNamedSecurityInfo 等
#include <aclapi.h>
// Shell 轻量工具函数（本代码中主要作为链接依赖保留）
#include <shlwapi.h>
// 进程快照 API：CreateToolhelp32Snapshot、Process32First/Next
#include <tlhelp32.h>
// SID 字符串转换 API：ConvertStringSidToSidW
#include <sddl.h>
// ShellExecute 等 Shell API
#include <shellapi.h>

#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// 内部工具函数
// ---------------------------------------------------------------------------

/**
 * @brief 将 UTF-8 编码的窄字符串转换为 Windows 宽字符串（UTF-16）。
 *
 * Windows API 的 W 后缀版本（如 GetFileAttributesW、CreateProcessW）
 * 使用 UTF-16 编码，因此需要转换。
 *
 * @param utf8 UTF-8 编码的 C 字符串。
 * @return 转换后的 std::wstring，失败返回空字符串。
 */
static std::wstring utf8_to_wide(const char *utf8)
{
    if (!utf8) return std::wstring();

    // 第一次调用：获取所需缓冲区大小（包含结尾的 '\0'）
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) return std::wstring();

    // 分配缓冲区并转换
    std::wstring ws(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &ws[0], len);

    // 去除尾部多余的 '\0'（MultiByteToWideChar 已包含结尾符）
    while (!ws.empty() && ws.back() == L'\0') ws.pop_back();
    return ws;
}

/**
 * @brief 提升当前进程令牌中的指定特权（Privilege）。
 *
 * 常用特权：
 *   SeDebugPrivilege       - 调试进程，可打开任意进程 Token
 *   SeTakeOwnershipPrivilege - 取得文件/对象所有权
 *   SeRestorePrivilege     - 恢复文件时绕过权限检查
 *   SeBackupPrivilege      - 备份文件时绕过权限检查
 *
 * @param priv_name 特权名称（宽字符串）。
 * @return 成功返回 true，失败返回 false。
 */
static bool enable_privilege(const wchar_t *priv_name)
{
    // 打开当前进程的访问令牌，请求调整和查询权限
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;

    // 将特权名称转换为 LUID（本地唯一标识符）
    if (!LookupPrivilegeValueW(nullptr, priv_name, &tp.Privileges[0].Luid)) {
        CloseHandle(hToken);
        return false;
    }

    // 启用该特权
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    // 应用特权调整
    bool ok = (AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp),
                                     nullptr, nullptr) != FALSE);
    CloseHandle(hToken);
    return ok;
}

/**
 * @brief 获取 TrustedInstaller 服务的 SID。
 *
 * TrustedInstaller 的固定 SID 为：
 *   S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464
 *
 * 使用 static 变量保证只转换一次，后续调用直接返回缓存值。
 *
 * @return TrustedInstaller 的 PSID，失败返回 nullptr。
 */
static PSID get_trusted_installer_sid()
{
    static PSID ti_sid = nullptr;
    if (ti_sid) return ti_sid;

    const wchar_t *ti_sid_str =
        L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

    if (!ConvertStringSidToSidW(ti_sid_str, &ti_sid))
        return nullptr;

    return ti_sid;
}

/**
 * @brief 遍历当前系统进程，查找 TrustedInstaller.exe 的进程 ID。
 *
 * TrustedInstaller.exe 是 Windows Modules Installer 服务的主进程，
 * 拥有对受保护系统文件的完全控制权。
 *
 * @return 找到返回进程 PID，未找到返回 0。
 */
static DWORD find_trusted_installer_pid()
{
    // 创建进程快照
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;

    // 遍历所有进程，按文件名匹配
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"TrustedInstaller.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }

    CloseHandle(hSnap);
    return pid;
}

/**
 * @brief 从 TrustedInstaller 进程窃取 Token（主令牌副本）。
 *
 * 步骤：
 *   1. 找到 TrustedInstaller.exe 的 PID
 *   2. 打开该进程（PROCESS_QUERY_INFORMATION）
 *   3. 打开其进程令牌（TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY）
 *   4. 复制令牌为 Primary Token（可用于 CreateProcessWithTokenW）
 *
 * 注意：此操作需要当前进程具有 SeDebugPrivilege 特权，
 * 通常要求当前进程以管理员身份运行。
 *
 * @return 成功返回复制的 Token 句柄（调用者负责 CloseHandle），失败返回 nullptr。
 */
static HANDLE steal_ti_token()
{
    DWORD pid = find_trusted_installer_pid();
    if (pid == 0) return nullptr;

    // 打开 TrustedInstaller 进程
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) return nullptr;

    // 打开其进程令牌
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(hProc,
                          TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY,
                          &hToken)) {
        CloseHandle(hProc);
        return nullptr;
    }

    // 复制为 Primary Token，安全级别为 SecurityImpersonation
    HANDLE hDup = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);

    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, &sa,
                          SecurityImpersonation, TokenPrimary, &hDup)) {
        CloseHandle(hToken);
        CloseHandle(hProc);
        return nullptr;
    }

    // 关闭中间句柄，返回复制的令牌
    CloseHandle(hToken);
    CloseHandle(hProc);
    return hDup;
}

/**
 * @brief 以 TrustedInstaller 身份执行命令行。
 *
 * 执行流程：
 *   1. 启用多项敏感特权（SeDebugPrivilege 等）
 *   2. 尝试窃取 TrustedInstaller Token
 *   3. 若成功，使用 CreateProcessWithTokenW 以 TI 身份执行
 *   4. 若失败（TI 未运行或无权窃取），降级为当前进程权限执行
 *   5. 等待子进程结束，返回退出码是否为 0
 *
 * 窗口设为 SW_HIDE + CREATE_NO_WINDOW，避免弹出黑框。
 *
 * @param cmd_line 要执行的命令行（宽字符串）。
 * @return 子进程退出码为 0 返回 true，否则返回 false。
 */
static bool run_as_ti(const wchar_t *cmd_line)
{
    if (!cmd_line || cmd_line[0] == L'\0')
        return false;

    // 启用敏感特权，为窃取 Token 做准备
    enable_privilege(L"SeDebugPrivilege");
    enable_privilege(L"SeTakeOwnershipPrivilege");
    enable_privilege(L"SeRestorePrivilege");
    enable_privilege(L"SeBackupPrivilege");

    // 尝试窃取 TrustedInstaller Token
    HANDLE hToken = steal_ti_token();

    if (!hToken) {
        // ---- 降级路径：以当前进程权限执行 ----
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};

        // CreateProcessW 会修改命令行缓冲区，必须拷贝
        std::vector<wchar_t> buf(cmd_line, cmd_line + wcslen(cmd_line) + 1);

        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return false;
        }

        // 等待子进程结束
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return exitCode == 0;
    }

    // ---- 主路径：以 TrustedInstaller Token 执行 ----
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    // 同样需要拷贝命令行
    std::vector<wchar_t> buf(cmd_line, cmd_line + wcslen(cmd_line) + 1);

    BOOL ok = CreateProcessWithTokenW(
        hToken,                // 使用窃取的 TI Token
        LOGON_WITH_PROFILE,    // 加载用户配置文件
        nullptr,               // 不使用单独的应用名
        buf.data(),            // 命令行
        CREATE_NO_WINDOW,      // 不创建控制台窗口
        nullptr,               // 使用当前环境变量
        nullptr,               // 使用当前工作目录
        &si,
        &pi
    );

    // 立即关闭 Token，不再需要
    CloseHandle(hToken);

    if (!ok) {
        // ---- CreateProcessWithTokenW 失败，降级执行 ----
        STARTUPINFOW si2 = {};
        si2.cb = sizeof(si2);
        si2.dwFlags = STARTF_USESHOWWINDOW;
        si2.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi2 = {};

        std::vector<wchar_t> buf2(cmd_line, cmd_line + wcslen(cmd_line) + 1);

        if (!CreateProcessW(nullptr, buf2.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi2)) {
            return false;
        }

        WaitForSingleObject(pi2.hProcess, INFINITE);

        DWORD exitCode = 0;
        GetExitCodeProcess(pi2.hProcess, &exitCode);

        CloseHandle(pi2.hThread);
        CloseHandle(pi2.hProcess);
        return exitCode == 0;
    }

    // 等待 TI 身份的子进程结束
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

/**
 * @brief 对指定路径执行 takeown + icacls，夺取所有权并赋予管理员完全控制。
 *
 * 使用 cmd.exe 调用系统内置工具：
 *   takeown /f "path" /r /d Y  - 递归取得所有权，默认选择 Y
 *   icacls "path" /grant administrators:F /t /c /q
 *     - 递归授予 Administrators 组完全控制（F）
 *     - /t 递归，/c 遇到错误继续，/q 安静模式
 *
 * 这两个命令本身会通过 run_as_ti 尝试以 TI 身份执行，
 * 从而绕过系统文件的权限保护。
 *
 * @param path 文件或目录的宽字符串路径。
 * @return 始终返回 true（命令执行结果不影响后续操作）。
 */
static bool take_ownership_and_grant_full_control(const std::wstring &path)
{
    PSID ti_sid = get_trusted_installer_sid();
    if (!ti_sid) return false;

    // 递归取得所有权
    std::wstring takeown_cmd =
        L"cmd.exe /c takeown /f \"" + path + L"\" /r /d Y";
    run_as_ti(takeown_cmd.c_str());

    // 递归授予管理员完全控制
    std::wstring icacls_cmd =
        L"cmd.exe /c icacls \"" + path + L"\" /grant administrators:F /t /c /q";
    run_as_ti(icacls_cmd.c_str());

    return true;
}

// ---------------------------------------------------------------------------
// 导出函数实现
// ---------------------------------------------------------------------------

/**
 * @brief 以 TrustedInstaller 权限删除文件或目录（递归）。
 *
 * 执行步骤：
 *   1. 参数校验（空指针或空字符串返回 -1）
 *   2. UTF-8 路径转宽字符串
 *   3. 调用 take_ownership_and_grant_full_control 夺取权限
 *   4. 判断是文件还是目录
 *   5. 目录：cmd /c rd /s /q "path"  递归静默删除
 *   6. 文件：cmd /c del /f /q "path" 强制静默删除
 *   7. 通过 run_as_ti 执行删除命令
 *
 * @param path UTF-8 编码的文件或目录路径。
 * @return 0 成功，-1 失败。
 */
TIHELPER_API int ti_delete_path(const char *path)
{
    if (!path || path[0] == '\0')
        return -1;

    std::wstring wpath = utf8_to_wide(path);
    if (wpath.empty())
        return -1;

    // 先夺取所有权和权限
    take_ownership_and_grant_full_control(wpath);

    // 获取文件属性，判断是文件还是目录
    DWORD attr = GetFileAttributesW(wpath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return -1;

    bool is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

    std::wstring cmd;
    if (is_dir) {
        // 递归删除目录及其所有内容
        cmd = L"cmd.exe /c rd /s /q \"" + wpath + L"\"";
    } else {
        // 强制删除文件
        cmd = L"cmd.exe /c del /f /q \"" + wpath + L"\"";
    }

    return run_as_ti(cmd.c_str()) ? 0 : -1;
}

/**
 * @brief 以 TrustedInstaller 权限执行指定文件。
 *
 * 执行步骤：
 *   1. 参数校验
 *   2. UTF-8 路径转宽字符串
 *   3. 夺取文件所有权和权限（防止文件被系统保护无法执行）
 *   4. 构建命令行：用双引号包裹路径，支持含空格的路径
 *   5. 通过 run_as_ti 执行
 *
 * 支持的文件类型：exe、msi、bat、cmd、ps1（需配合 powershell）等。
 * 对于 bat/cmd，cmd.exe 会自动识别；对于 msi，建议外部用 msiexec 调用。
 *
 * @param exe_path UTF-8 编码的可执行文件路径。
 * @return 0 成功，-1 失败。
 */
TIHELPER_API int ti_execute_file(const char *exe_path)
{
    if (!exe_path || exe_path[0] == '\0')
        return -1;

    std::wstring wpath = utf8_to_wide(exe_path);
    if (wpath.empty())
        return -1;

    // 夺取文件权限
    take_ownership_and_grant_full_control(wpath);

    // 用双引号包裹路径，防止路径中含空格导致解析错误
    std::wstring cmd = L"\"" + wpath + L"\"";

    return run_as_ti(cmd.c_str()) ? 0 : -1;
}
