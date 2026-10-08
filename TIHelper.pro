# ============================================================================
# TIHelper —— TrustedInstaller 权限辅助 DLL（Qt 6.12 / C++17）
#
# 只使用 QtCore 的导出宏，不链接任何 Qt 运行库，
# 因此生成的 DLL 可以被 Python / C# / 原生 C++ 宿主直接加载。
# ============================================================================

QT -= gui
QT += core

TARGET   = TIHelper
TEMPLATE = lib

CONFIG += c++17
CONFIG -= debug_and_release debug_and_release_target

# 保持对外约定的固定文件名 TIHelper.dll（不带版本号后缀）
CONFIG += skip_target_version_ext

DEFINES += TIHELPER_EXPORTS

SOURCES += tihelper.cpp
HEADERS += tihelper.h

win32 {
    # advapi32：令牌 / ACL / 注册表
    # user32  ：窗口与权限相关 API
    # shell32 ：CommandLineToArgvW 等 Shell 功能
    # ole32   ：Shell API 所需的 COM 基础设施
    LIBS += -ladvapi32 -luser32 -lshell32 -lole32

    # UAC manifest：让宿主 exe 启动时请求管理员权限。
    # 注意：仅对 exe 宿主生效；python / c# / rundll32 加载 DLL 不会自动提权。
    RC_FILE  += TIHelper.rc
    DISTFILES += TIHelper.manifest

    # ------------------------------------------------------------------
    # ★ 构建完成后自动把 TIHelper.dll 拷贝到 czl_installer 工程目录。
    #
    # 这样 czl_installer 的 QMAKE_POST_LINK 就能从那里再拷到自己的
    # 输出目录，形成「TIHelper -> czl_installer -> 运行目录」的链条，
    # 不需要手工拷贝 DLL。
    #
    # 目录顺序可覆盖，便于 czl_installer 换位置：
    #   qmake "TIHELPER_CONSUMER_DIR=D:/other/czl_installer"
    # ------------------------------------------------------------------
    CONSUMER_DIR = $$TIHELPER_CONSUMER_DIR
    isEmpty(CONSUMER_DIR) {
        CONSUMER_DIR = $$shell_path($$PWD/../czl_installer)
    } else {
        CONSUMER_DIR = $$shell_path($$CONSUMER_DIR)
    }
    # 提示信息里也显示规范化后的绝对路径，避免出现 "..\"
    CONSUMER_DIR_INFO = $$clean_path($$CONSUMER_DIR)

    # 构建产物路径：MinGW 与 MSVC 都把 dll 放在 $$OUT_PWD
    # （MSVC 若自定义了 DESTDIR，这里会自动跟随）
    isEmpty(DESTDIR) {
        DLL_FILE = $$shell_path($$OUT_PWD/TIHelper.dll)
    } else {
        DLL_FILE = $$shell_path($$DESTDIR/TIHelper.dll)
    }
    DLL_TARGET = $$shell_path($$CONSUMER_DIR/TIHelper.dll)

    # 目标目录存在才拷贝。刻意用「if not exist ... goto :eof」这种单层写法，
    # 避免 && 与括号在 make 展开后被 cmd 误解析。
    QMAKE_POST_LINK += cmd /c if not exist \"$$CONSUMER_DIR\" goto :eof
    QMAKE_POST_LINK += $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += cmd /c copy /Y \"$$DLL_FILE\" \"$$DLL_TARGET\" >nul
    QMAKE_POST_LINK += $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += cmd /c echo [TIHelper] copied TIHelper.dll to $$CONSUMER_DIR_INFO
}

DISTFILES += README.MD
