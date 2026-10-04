#ifndef TIHELPER_H
#define TIHELPER_H

#include <QtGlobal>

// 编译 DLL 时定义 TIHELPER_EXPORTS，导出符号；
// 外部程序包含此头文件时未定义该宏，则为导入符号。
#ifdef TIHELPER_EXPORTS
#  define TIHELPER_API Q_DECL_EXPORT
#else
#  define TIHELPER_API Q_DECL_IMPORT
#endif

// 使用 extern "C" 避免 C++ 名称修饰（name mangling），
// 使 Python ctypes、C# DllImport 等能直接按函数名调用。
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 以 TrustedInstaller 权限删除文件或目录（递归删除）。
 * @param path UTF-8 编码的文件或目录路径。
 * @return 0 表示成功，-1 表示失败。
 */
TIHELPER_API int ti_delete_path(const char *path);

/**
 * @brief 以 TrustedInstaller 权限执行指定文件（exe/msi/bat/cmd 等）。
 * @param exe_path UTF-8 编码的可执行文件路径。
 * @return 0 表示成功，-1 表示失败。
 */
TIHELPER_API int ti_execute_file(const char *exe_path);

#ifdef __cplusplus
}
#endif

#endif // TIHELPER_H
