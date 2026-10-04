# 不使用 GUI 模块，只保留核心功能，减小依赖
QT -= gui

# 生成的 DLL 文件名为 TIHelper.dll
TARGET = TIHelper

# 编译为动态库（DLL）
TEMPLATE = lib

# 启用 C++11 标准
CONFIG += c++11

# 定义导出宏，编译 DLL 时生效，使 Q_DECL_EXPORT 生效
DEFINES += TIHELPER_EXPORTS

SOURCES += \
    tihelper.cpp

HEADERS += \
    tihelper.h

# Windows 平台需要链接的系统库：
# advapi32：权限调整、Token 操作、SID 转换、注册表等
# user32：部分窗口/权限相关 API
# shell32：ShellExecute 等 Shell 功能
# shlwapi：路径处理工具函数
win32:LIBS += -ladvapi32 -luser32 -lshell32 -lshlwapi

# Windows 平台嵌入 UAC manifest，使宿主 exe 启动时请求管理员权限
# 注意：该 manifest 只对 exe 宿主生效；python/c#/rundll32 加载 DLL 时不会自动提权
win32 {
    RC_FILE += TIHelper.rc
    DISTFILES += TIHelper.manifest
}

DISTFILES += \
    README.MD \
    TIHelper.manifest \
    TIHelper.rc
