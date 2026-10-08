#ifndef TIHELPER_H
#define TIHELPER_H

/*
 * TIHelper —— 以 TrustedInstaller 身份执行/删除文件的轻量 C 接口 DLL。
 *
 * 设计要点（Qt 6.12 / C++17）：
 *   - 只依赖 <QtCore/qglobal.h> 提供导出宏，不链接 Qt 运行库，
 *     因此 Python ctypes / C# DllImport / rundll32 均可直接加载；
 *   - 导出符号使用 extern "C"，不做 C++ 名字修饰；
 *   - 所有路径参数统一使用 UTF-8 编码。
 */

#include <QtCore/qglobal.h>

#if defined(_WIN32)
#  define TIHELPER_CALL __cdecl
#else
#  define TIHELPER_CALL
#endif

/* 编译 DLL 时由构建系统定义 TIHELPER_EXPORTS；外部包含时为导入。 */
#if defined(TIHELPER_STATIC)
#  define TIHELPER_API
#elif defined(TIHELPER_EXPORTS)
#  define TIHELPER_API Q_DECL_EXPORT
#else
#  define TIHELPER_API Q_DECL_IMPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 统一返回值约定（负数均为本库自身错误，正数为子进程退出码）：
 *   0   成功
 *   -1  参数非法（空指针、空字符串、UTF-8 解码失败）
 *   -2  路径不存在
 *   -3  权限不足（取所有权 / 提权均失败）
 *   -4  无法创建子进程
 *  >0   子进程真实退出码（仅 ti_execute_* 系列可能返回）
 */
#define TI_OK              0
#define TI_ERR_ARG        (-1)
#define TI_ERR_NOT_FOUND  (-2)
#define TI_ERR_ACCESS     (-3)
#define TI_ERR_SPAWN      (-4)

/* 导出接口 ABI 版本：调用方可用它判断 DLL 是否与头文件匹配。 */
#define TIHELPER_ABI_VERSION 2

/**
 * @brief 返回接口 ABI 版本（当前为 TIHELPER_ABI_VERSION）。
 */
TIHELPER_API int TIHELPER_CALL ti_helper_abi_version(void);

/**
 * @brief 以 TrustedInstaller 权限删除文件或目录（目录递归删除）。
 *
 * 内部流程：夺取所有权 -> 收紧 ACL -> 原生 API 删除 ->
 * 失败时退回 cmd.exe 的 rd / del，最后尝试重启后删除。
 *
 * @param path UTF-8 编码的文件或目录路径。
 * @return 见上方返回值约定。
 */
TIHELPER_API int TIHELPER_CALL ti_delete_path(const char *path);

/**
 * @brief 以 TrustedInstaller 权限执行指定文件（exe/msi/bat/cmd 等）。
 *
 * @param exe_path UTF-8 编码的可执行文件路径。
 * @return 0 表示子进程退出码为 0；>0 为子进程退出码；负数见约定。
 */
TIHELPER_API int TIHELPER_CALL ti_execute_file(const char *exe_path);

/**
 * @brief 以 TrustedInstaller 权限执行文件并附加命令行参数。
 *
 * 参数会按 Windows 命令行规则自动加引号（CommandLineToArgvW 可无损还原），
 * 因此含空格、中文、& | ^ 等字符的参数不会破坏命令解析。
 *
 * @param exe_path UTF-8 编码的可执行文件路径。
 * @param args     UTF-8 编码的参数串，可为 nullptr。
 * @return 同 ti_execute_file。
 */
TIHELPER_API int TIHELPER_CALL ti_execute_file_args(const char *exe_path,
                                                    const char *args);

#ifdef __cplusplus
}
#endif

#endif /* TIHELPER_H */
