/*
 * TIHelper —— 以 TrustedInstaller 身份执行/删除文件的 C 接口实现。
 *
 * 与旧版相比的主要修正见 README.MD 的「已修正问题」章节，要点：
 *   1. manifest 命名空间拼写错误（asm vener3 -> asm.v3）导致 UAC 段被忽略；
 *   2. AdjustTokenPrivileges 成功但特权未授予时旧代码误判为成功；
 *   3. 命令行用字符串拼接、路径未加引号，含空格/&/|/^ 的路径会被 cmd 解析破坏；
 *   4. DeleteFileW 在只读属性存在时必然失败，旧代码未清理属性；
 *   5. take_ownership_and_grant_full_control 通过 two 次 cmd 调用 takeown/icacls，
 *      在非英文系统上 administrators 名称不匹配而静默失败；
 *   6. 全部改为原生 Win32 API 直删/直执行，cmd 仅作为最后的兜底路径。
 */

#include "tihelper.h"

#ifndef _WIN32
#  error "TIHelper 仅支持 Windows 平台"
#endif

#ifndef WINVER
#  define WINVER 0x0601          /* Windows 7 及以上 */
#endif
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0601
#endif

#include <winsdkver.h>
#include <sdkddkver.h>
#include <windows.h>
#include <aclapi.h>       /* SetNamedSecurityInfoW / SetEntriesInAclW */
#include <sddl.h>         /* ConvertStringSidToSidW */
#include <shellapi.h>     /* CommandLineToArgvW（由 shell32 提供） */
#include <tlhelp32.h>     /* CreateToolhelp32Snapshot */
#include <shlobj.h>       /* SHDeleteFile 相关声明 */

#include <string>
#include <vector>
#include <algorithm>

// ===========================================================================
// 1. 基础工具
// ===========================================================================

namespace {

/// 把当前线程的 LastError 归零，便于区分「函数失败」与「上一次残留错误」。
inline void clear_last_error() noexcept
{
    ::SetLastError(ERROR_SUCCESS);
}

/// UTF-8 -> UTF-16。非法序列直接失败（不使用替换字符，避免静默改路径）。
std::wstring utf8_to_wide(const char *utf8)
{
    if (utf8 == nullptr)
        return {};

    const int len = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          utf8, -1, nullptr, 0);
    if (len <= 0)
        return {};

    std::wstring out(static_cast<std::size_t>(len - 1), L'\0');
    if (len > 1) {
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                              utf8, -1, out.data(), len);
    }
    return out;
}

/**
 * @brief 需要时给路径加 \\?\ 长路径前缀。
 * 仅在绝对路径、未带前缀、且长度接近 MAX_PATH 时添加，
 * 避免影响 CreateProcess 等不接受该前缀的 API。
 */
std::wstring extended_path(const std::wstring &path)
{
    if (path.size() < MAX_PATH - 16)
        return path;
    if (path.rfind(L"\\\\?\\", 0) == 0)
        return path;
    if (path.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

/// 转换为原生反斜杠分隔形式（用于展示与 cmd 兜底）。
std::wstring to_backslash(std::wstring value)
{
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return value;
}

/// 路径是否已存在。
bool path_exists(const std::wstring &path)
{
    return ::GetFileAttributesW(extended_path(path).c_str())
           != INVALID_FILE_ATTRIBUTES;
}

/// 目标是否为目录。
bool is_directory(const std::wstring &path)
{
    const DWORD attr = ::GetFileAttributesW(extended_path(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

/// 递归清除只读/隐藏/系统属性（-1 表示本层也处理）。
bool clear_readonly_recursive(const std::wstring &path)
{
    const std::wstring ext = extended_path(path);
    const DWORD attr = ::GetFileAttributesW(ext.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return false;

    if (attr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN
                | FILE_ATTRIBUTE_SYSTEM)) {
        ::SetFileAttributesW(ext.c_str(),
                             attr & ~(FILE_ATTRIBUTE_READONLY
                                      | FILE_ATTRIBUTE_HIDDEN
                                      | FILE_ATTRIBUTE_SYSTEM));
    }

    if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
        return true;

    /* 目录：递归处理所有子项 */
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = ext + L"\\*";
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return true;    // 空目录或无法枚举，交给后续删除步骤判断

    bool ok = true;
    do {
        const wchar_t *name = fd.cFileName;
        if (::wcscmp(name, L".") == 0 || ::wcscmp(name, L"..") == 0)
            continue;
        if (!clear_readonly_recursive(path + L"\\" + name))
            ok = false;
    } while (::FindNextFileW(hFind, &fd));

    ::FindClose(hFind);
    return ok;
}

// ===========================================================================
// 2. 权限：超级特权、当前用户 SID、TrustedInstaller 令牌
// ===========================================================================

/**
 * @brief 启用当前进程令牌中的指定特权。
 *
 * 注意：AdjustTokenPrivileges 即使「部分成功」也返回 TRUE，
 * 必须再检查 GetLastError() == ERROR_NOT_ALL_ASSIGNED，否则会把
 * 「特权不存在/未授予」误判为成功（旧版即有此缺陷）。
 */
bool enable_privilege(const wchar_t *privilege)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    LUID luid{};
    if (!::LookupPrivilegeValueW(nullptr, privilege, &luid)) {
        ::CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    clear_last_error();
    const BOOL adjusted = ::AdjustTokenPrivileges(token, FALSE, &tp,
                                                  sizeof(tp), nullptr, nullptr);
    const DWORD error = ::GetLastError();
    ::CloseHandle(token);

    if (!adjusted)
        return false;
    if (error == ERROR_NOT_ALL_ASSIGNED)
        return false;               // 特权未真正启用
    return true;                    // 成功，或 ERROR_SUCCESS（已启用）
}

/// 一次性启用本库需要的全部特权（按需调用，未授予的忽略）。
void enable_all_privileges()
{
    static const wchar_t *const kPrivileges[] = {
        SE_DEBUG_NAME,              /* 打开 TrustedInstaller 进程令牌 */
        SE_TAKE_OWNERSHIP_NAME,     /* 取得受保护文件的所有权 */
        SE_RESTORE_NAME,            /* 绕过 ACL 重建受保护对象 */
        SE_BACKUP_NAME,             /* 备份/还原语义 */
        SE_INCREASE_QUOTA_NAME,     /* CreateProcessWithTokenW 需要 */
        SE_ASSIGNPRIMARYTOKEN_NAME, /* 同上（未授予则忽略） */
    };
    for (const wchar_t *name : kPrivileges)
        (void)enable_privilege(name);
}

/// 复制当前进程令牌中的用户 SID，调用方负责 LocalFree。
PSID duplicate_current_user_sid()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return nullptr;

    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) {
        ::CloseHandle(token);
        return nullptr;
    }

    std::vector<BYTE> buffer(size);
    PSID result = nullptr;
    if (::GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        const auto *user = reinterpret_cast<TOKEN_USER *>(buffer.data());
        const DWORD sidLen = ::GetLengthSid(user->User.Sid);
        result = ::LocalAlloc(LPTR, sidLen);
        if (result != nullptr && !::CopySid(sidLen, result, user->User.Sid)) {
            ::LocalFree(result);
            result = nullptr;
        }
    }

    ::CloseHandle(token);
    return result;
}

/// TrustedInstaller 的内置 SID。
PSID trusted_installer_sid()
{
    static PSID cached = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        (void)::ConvertStringSidToSidW(
            L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
            &cached);
    }
    return cached;
}

/// 在进程快照里按可执行文件名查找 PID。
DWORD find_process_by_name(const wchar_t *exeName)
{
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;

    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (::_wcsicmp(entry.szExeFile, exeName) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (::Process32NextW(snapshot, &entry));
    }

    ::CloseHandle(snapshot);
    return pid;
}

/**
 * @brief 取得 TrustedInstaller 进程的主令牌副本。
 *
 * 若服务未运行则先尝试启动它并等待（旧版直接放弃并降级，导致
 * 明明有权限却以管理员身份执行，受保护文件操作失败）。
 * 无论启动成功与否，调用方的 try/catch 与降级逻辑都必须存在。
 */
HANDLE open_trusted_installer_token()
{
    /* 先启用特权，否则 OpenProcess / OpenProcessToken 会被拒绝 */
    (void)enable_privilege(SE_DEBUG_NAME);
    (void)enable_privilege(SE_INCREASE_QUOTA_NAME);
    (void)enable_privilege(SE_ASSIGNPRIMARYTOKEN_NAME);

    DWORD pid = find_process_by_name(L"TrustedInstaller.exe");

    if (pid == 0) {
        /* 服务没起来：尝试启动并等待最多 15 秒 */
        SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scm != nullptr) {
            SC_HANDLE service = ::OpenServiceW(scm, L"TrustedInstaller",
                                               SERVICE_START | SERVICE_QUERY_STATUS);
            if (service != nullptr) {
                (void)::StartServiceW(service, 0, nullptr);
                for (int i = 0; i < 30 && pid == 0; ++i) {
                    ::Sleep(500);
                    pid = find_process_by_name(L"TrustedInstaller.exe");
                }
                ::CloseServiceHandle(service);
            }
            ::CloseServiceHandle(scm);
        }
    }

    if (pid == 0)
        return nullptr;

    /* 打开目标进程时申请最小必要权限，减少被拒绝的概率 */
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
        process = ::OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process == nullptr)
        return nullptr;

    HANDLE token = nullptr;
    const BOOL opened = ::OpenProcessToken(
        process, TOKEN_DUPLICATE | TOKEN_QUERY, &token);
    ::CloseHandle(process);
    if (!opened)
        return nullptr;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;

    HANDLE primary = nullptr;
    const BOOL duplicated = ::DuplicateTokenEx(
        token, TOKEN_ALL_ACCESS, &sa,
        SecurityImpersonation, TokenPrimary, &primary);

    ::CloseHandle(token);
    return duplicated ? primary : nullptr;
}

/**
 * @brief 夺取 path（含子项）的所有权并把完全控制权授予当前用户。
 *
 * 直接用 ACL API 完成，不依赖 takeown/icacls 的本地化输出。
 * 授权对象使用 SID 而不是 "administrators" 名称，避免中文/日文等
 * 系统上因账户名不匹配而静默失败（旧版缺陷）。
 */
bool take_ownership_and_grant_full_control(const std::wstring &path)
{
    const std::wstring ext = extended_path(path);

    (void)enable_privilege(SE_TAKE_OWNERSHIP_NAME);
    (void)enable_privilege(SE_RESTORE_NAME);
    (void)enable_privilege(SE_BACKUP_NAME);

    PSID userSid = duplicate_current_user_sid();
    PSID tiSid = trusted_installer_sid();

    /* 1) 打开对象，拿到安全描述符（文件、目录走同一条路径） */
    DWORD result = ERROR_SUCCESS;
    PSECURITY_DESCRIPTOR sd = nullptr;

    result = ::GetNamedSecurityInfoW(
        const_cast<LPWSTR>(ext.c_str()), SE_FILE_OBJECT,
        WRITE_OWNER | WRITE_DAC | READ_CONTROL,
        nullptr, nullptr, nullptr, nullptr, &sd);

    if (result != ERROR_SUCCESS)
        return false;

    /* 2) 在现有 DACL 上追加「当前用户 + TrustedInstaller」完全控制 */
    EXPLICIT_ACCESSW entries[2]{};
    int entryCount = 0;

    auto fill = [](EXPLICIT_ACCESSW &ea, PSID sid) {
        ea.grfAccessPermissions = GENERIC_ALL;
        ea.grfAccessMode = SET_ACCESS;
        ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        ea.Trustee.pMultipleTrustee = nullptr;
        ea.Trustee.MultipleTrusteeOperation = NO_MULTIPLE_TRUSTEE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    };

    if (userSid != nullptr) {
        fill(entries[entryCount], userSid);
        entries[entryCount].Trustee.TrusteeType = TRUSTEE_IS_USER;
        ++entryCount;
    }
    if (tiSid != nullptr) {
        fill(entries[entryCount], tiSid);
        entries[entryCount].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ++entryCount;
    }
    if (entryCount == 0) {
        ::LocalFree(sd);
        return false;
    }

    PACL newDacl = nullptr;
    result = ::SetEntriesInAclW(static_cast<ULONG>(entryCount), entries,
                                nullptr, &newDacl);
    if (result == ERROR_SUCCESS) {
        /* 3) 同时调整 owner 与 DACL：owner 交给当前用户，
         *    这样后续进程即便只以管理员身份运行也能删除目标。 */
        result = ::SetNamedSecurityInfoW(
            const_cast<LPWSTR>(ext.c_str()), SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION
                | PROTECTED_DACL_SECURITY_INFORMATION,
            userSid, nullptr, newDacl, nullptr);
    }

    if (newDacl != nullptr)
        ::LocalFree(newDacl);
    ::LocalFree(sd);
    if (userSid != nullptr)
        ::LocalFree(userSid);

    if (result != ERROR_SUCCESS)
        return false;

    /* 4) 递归去掉只读/隐藏/系统属性 —— 否则 DeleteFileW 必定失败 */
    return clear_readonly_recursive(ext);
}

// ===========================================================================
// 3. 进程创建：以 TrustedInstaller 令牌执行
// ===========================================================================

/// 等待进程结束并返回退出码；失败返回 -4。
int wait_and_get_exit_code(PROCESS_INFORMATION &pi)
{
    ::WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    const BOOL ok = ::GetExitCodeProcess(pi.hProcess, &exitCode);

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    return ok ? static_cast<int>(exitCode) : TI_ERR_SPAWN;
}

/// 组装隐藏窗口的 STARTUPINFOW。
STARTUPINFOW hidden_startup_info()
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    return si;
}

/**
 * @brief 以指定令牌创建进程；token 为 nullptr 时使用当前进程身份。
 *
 * 使用 lpApplicationName + 可变的 lpCommandLine，
 * 避免旧版把整个命令行拼成一个字符串导致 Windows 重新解析出偏差。
 */
int spawn(const std::wstring &application,
          std::wstring commandLine,          /* 需要可写缓冲区 */
          HANDLE token)
{
    STARTUPINFOW si = hidden_startup_info();
    PROCESS_INFORMATION pi{};

    const DWORD creationFlags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;

    BOOL created = FALSE;
    if (token != nullptr) {
        /* 依次尝试更宽松的登录标志：某些服务令牌没有可加载的配置文件，
         * LOGON_WITH_PROFILE 会直接失败，此时退回 NETCREDENTIALS_ONLY。 */
        static const DWORD kLogonFlags[] = {
            LOGON_WITH_PROFILE,
            LOGON_NETCREDENTIALS_ONLY,
            0,
        };
        for (DWORD flags : kLogonFlags) {
            created = ::CreateProcessWithTokenW(
                token, flags,
                application.empty() ? nullptr : application.c_str(),
                commandLine.data(), creationFlags,
                nullptr, nullptr, &si, &pi);
            if (created)
                break;
            ::Sleep(50);
        }
    } else {
        created = ::CreateProcessW(
            application.empty() ? nullptr : application.c_str(),
            commandLine.data(), nullptr, nullptr, FALSE,
            creationFlags, nullptr, nullptr, &si, &pi);
    }

    if (!created)
        return TI_ERR_SPAWN;

    return wait_and_get_exit_code(pi);
}

/// 在令牌不可用时返回 «按当前身份执行» 的结果；token 为 nullptr 表示当前身份。
int run_with_token_or_self(const std::wstring &application,
                           const std::wstring &commandLine,
                           HANDLE token)
{
    /* CreateProcessW / CreateProcessWithTokenW 会就地修改命令行缓冲区，
     * 因此每次都复制一份独立的可写副本。 */
    std::wstring buffer = commandLine;
    return spawn(application, std::move(buffer), token);
}

/// 把命令交给 cmd.exe，使用 /s 语义并把整条命令包成 ""...""。
int run_via_cmd(const std::wstring &command, HANDLE token)
{
    /* /s 让 cmd 剥掉首尾引号后原样执行；""..."" 保证路径含空格时不被拆散，
     * 这也是 Windows 计划任务 / 服务命令行里最稳妥的写法。 */
    const std::wstring cmdLine = L"/s /c \"" + command + L"\"";
    return run_with_token_or_self(L"cmd.exe", cmdLine, token);
}

} // namespace

// ===========================================================================
// 4. 导出函数
// ===========================================================================

extern "C" TIHELPER_API int TIHELPER_CALL ti_helper_abi_version(void)
{
    return TIHELPER_ABI_VERSION;
}

extern "C" TIHELPER_API int TIHELPER_CALL ti_delete_path(const char *path)
{
    if (path == nullptr || path[0] == '\0')
        return TI_ERR_ARG;

    std::wstring wide = utf8_to_wide(path);
    if (wide.empty())
        return TI_ERR_ARG;

    wide = to_backslash(wide);

    if (!path_exists(wide))
        return TI_ERR_NOT_FOUND;

    const bool directory = is_directory(wide);
    const std::wstring ext = extended_path(wide);

    /* ---- 第 0 步：启用敏感特权，否则后面夺权/删文件都会被拒绝 ---- */
    enable_all_privileges();

    /* ---- 第 1 步：夺取所有权 + 授予当前用户完全控制 + 清属性 ---- */
    bool granted = take_ownership_and_grant_full_control(wide);

    /* ---- 第 2 步：原生 API 删除（不经过 shell，无注入面） ---- */
    auto try_native_delete = [&]() -> bool {
        clear_last_error();
        if (directory)
            (void)::RemoveDirectoryW(ext.c_str());
        else
            (void)::DeleteFileW(ext.c_str());
        return !path_exists(wide);
    };

    if (try_native_delete())
        return TI_OK;

    /* ---- 第 3 步：首次失败时再夺一次权（ACL 可能刚被系统回收） ---- */
    if (!granted) {
        granted = take_ownership_and_grant_full_control(wide);
        if (try_native_delete())
            return TI_OK;
    }

    /* ---- 第 4 步：以 TrustedInstaller 身份用 cmd 兜底 ---- */
    /* 路径里的 " 对 cmd 无解，此时跳过 cmd 兜底，避免命令被截断执行错对象 */
    if (wide.find(L'"') == std::wstring::npos) {
        const std::wstring command = directory
            ? L"rd /s /q \"" + wide + L"\""
            : L"del /f /q /a \"" + wide + L"\"";

        HANDLE token = open_trusted_installer_token();
        (void)run_via_cmd(command, token);
        if (token != nullptr)
            ::CloseHandle(token);

        if (!path_exists(wide))
            return TI_OK;
    }

    /* ---- 第 5 步：彻底失败时登记「重启后删除」，至少保证最终清干净 ---- */
    if (::MoveFileExW(ext.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
        return TI_OK;

    return TI_ERR_ACCESS;
}

extern "C" TIHELPER_API int TIHELPER_CALL ti_execute_file(const char *exe_path)
{
    return ti_execute_file_args(exe_path, nullptr);
}

extern "C" TIHELPER_API int TIHELPER_CALL ti_execute_file_args(const char *exe_path,
                                                              const char *args)
{
    if (exe_path == nullptr || exe_path[0] == '\0')
        return TI_ERR_ARG;

    std::wstring application = utf8_to_wide(exe_path);
    if (application.empty())
        return TI_ERR_ARG;

    application = to_backslash(application);

    if (!path_exists(application))
        return TI_ERR_NOT_FOUND;

    /* 参数同样来自 UTF-8，拼接顺序：exe + 空格 + args */
    std::wstring commandLine = application;
    if (args != nullptr && args[0] != '\0') {
        const std::wstring extra = utf8_to_wide(args);
        if (extra.empty())
            return TI_ERR_ARG;
        commandLine += L' ';
        commandLine += extra;
    }

    /* 先尝试取得 TrustedInstaller 令牌；失败则退回当前（已提升）身份，
     * 保证「至少能执行」而不是直接失败。
     *
     * 注意：这里不再像旧版那样先对目标文件夺取所有权 —— 执行只需要
     * 读取/执行权限，改动目标文件的属主与 ACL 属于不必要的副作用。 */
    enable_all_privileges();
    HANDLE token = open_trusted_installer_token();
    const int rc = run_with_token_or_self(application, commandLine, token);
    if (token != nullptr)
        ::CloseHandle(token);

    return rc;
}
