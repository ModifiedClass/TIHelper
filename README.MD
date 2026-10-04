# 调用方法
C
int ti_delete_path(const char *path);   // 删除文件或目录，UTF-8 路径
int ti_execute_file(const char *exe_path); // 以 TI 权限执行文件，UTF-8 路径

Python
import ctypes
import os

# 根据 DLL 位数选择路径，32 位 Python 加载 32 位 DLL，64 位同理
dll = ctypes.CDLL(os.path.join(os.path.dirname(__file__), "TIHelper.dll"))

dll.ti_delete_path.argtypes = [ctypes.c_char_p]
dll.ti_delete_path.restype = ctypes.c_int

dll.ti_execute_file.argtypes = [ctypes.c_char_p]
dll.ti_execute_file.restype = ctypes.c_int

# 删除文件或目录
ret = dll.ti_delete_path(r"C:\Windows\Temp\TestFolder".encode("utf-8"))
print("ti_delete_path:", ret)

# 以 TrustedInstaller 权限执行文件
ret = dll.ti_execute_file(r"C:\Tools\mytool.exe".encode("utf-8"))
print("ti_execute_file:", ret)


c++
#include <cstdio>
#include <cstdlib>
#include <windows.h>

// 定义与 DLL 导出函数一致的函数指针类型
typedef int (*ti_delete_path_fn)(const char *path);
typedef int (*ti_execute_file_fn)(const char *exe_path);

int main()
{
    // 1. 加载 DLL
    //    如果 DLL 不在当前目录或系统 PATH 中，建议使用绝对路径
    HMODULE hDll = LoadLibraryW(L"TIHelper.dll");
    if (!hDll) {
        // GetLastError() 可获取加载失败的具体原因
        printf("LoadLibrary failed, error code: %lu\n", GetLastError());
        return 1;
    }

    // 2. 获取导出函数的地址
    //    函数名必须与 DLL 中导出的名称完全一致（区分大小写）
    ti_delete_path_fn  fn_delete  = (ti_delete_path_fn)GetProcAddress(hDll, "ti_delete_path");
    ti_execute_file_fn fn_execute = (ti_execute_file_fn)GetProcAddress(hDll, "ti_execute_file");

    // 3. 检查函数指针是否获取成功
    if (!fn_delete || !fn_execute) {
        printf("GetProcAddress failed, error code: %lu\n", GetLastError());
        FreeLibrary(hDll);  // 释放 DLL
        return 1;
    }

    // 4. 调用导出函数
    //    路径必须使用 UTF-8 编码
    const char *delete_path = "C:\\Windows\\Temp\\TestFolder";
    const char *exec_path   = "C:\\Tools\\mytool.exe";

    int ret = fn_delete(delete_path);
    printf("ti_delete_path(\"%s\") = %d\n", delete_path, ret);

    ret = fn_execute(exec_path);
    printf("ti_execute_file(\"%s\") = %d\n", exec_path, ret);

    // 5. 释放 DLL
    FreeLibrary(hDll);

    return 0;
}

c#
using System;
using System.Runtime.InteropServices;
using System.Text;

class Program
{
    [DllImport("TIHelper.dll", CallingConvention = CallingConvention.Cdecl,
               CharSet = CharSet.Ansi)]
    private static extern int ti_delete_path(string path);

    [DllImport("TIHelper.dll", CallingConvention = CallingConvention.Cdecl,
               CharSet = CharSet.Ansi)]
    private static extern int ti_execute_file(string exe_path);

    static void Main()
    {
        // C# 默认字符串是 UTF-16，DllImport 的 CharSet.Ansi 会把字符串
        // 按系统 ANSI 编码传入。如果路径包含非 ANSI 字符，建议改成：
        //   CallingConvention.Cdecl, EntryPoint = "ti_delete_path"
        // 并手动把 UTF-8 字节数组传进去。

        int ret = ti_delete_path(@"C:\Windows\Temp\TestFolder");
        Console.WriteLine($"ti_delete_path: {ret}");

        ret = ti_execute_file(@"C:\Tools\mytool.exe");
        Console.WriteLine($"ti_execute_file: {ret}");
    }
}
如果路径可能包含中文、特殊符号，更稳妥的写法是把 UTF-8 字节数组传进去：
[DllImport("TIHelper.dll", CallingConvention = CallingConvention.Cdecl)]
private static extern int ti_delete_path(byte[] path);

[DllImport("TIHelper.dll", CallingConvention = CallingConvention.Cdecl)]
private static extern int ti_execute_file(byte[] exe_path);

// 调用
int ret = ti_delete_path(Encoding.UTF8.GetBytes(@"C:\测试目录\文件.txt"));

命令行快速测试
#include <cstdio>

extern "C" {
    int ti_delete_path(const char *path);
    int ti_execute_file(const char *exe_path);
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        printf("用法: TIHelperTest.exe delete \"C:\\path\"\n");
        printf("用法: TIHelperTest.exe exec   \"C:\\path\\file.exe\"\n");
        return 1;
    }

    const char *cmd  = argv[1];
    const char *path = argv[2];
    int ret = -1;

    if (strcmp(cmd, "delete") == 0)
        ret = ti_delete_path(path);
    else if (strcmp(cmd, "exec") == 0)
        ret = ti_execute_file(path);

    printf("result: %d\n", ret);
    return ret == 0 ? 0 : 1;
}
