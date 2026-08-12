cmake_minimum_required(VERSION 3.22)
project(notepad-safe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 强制使用静态 CRT（/MT），与 Qt 静态库编译选项匹配
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt 6.11.1 静态库路径
#   优先级：环境变量 NOTEPADSAFE_QT6_DIR > 缓存 NOTEPADSAFE_QT6_DIR > 原机默认值
if(DEFINED ENV{NOTEPADSAFE_QT6_DIR})
    set(NOTEPADSAFE_QT6_DIR "$ENV{NOTEPADSAFE_QT6_DIR}" CACHE PATH "Qt 6 static msvc path")
elseif(NOT NOTEPADSAFE_QT6_DIR)
    set(NOTEPADSAFE_QT6_DIR "E:/programs/Qt_6.11.1-static-Release_msvc2026_64/msvc2026_64" CACHE PATH "Qt 6 static msvc path")
endif()
set(CMAKE_PREFIX_PATH "${NOTEPADSAFE_QT6_DIR}")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets PrintSupport Svg SvgWidgets)

# QScintilla 静态库（相对于产品根，clone 后即可构建）
set(QSCINTILLA_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QScintilla_src-2.14.1/src")
set(QSCINTILLA_LIB "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QScintilla_src-2.14.1/src/release/qscintilla2_qt6.lib")

# 应用资源：EXE 图标 RC + 窗口/任务栏 QRC
set(APP_RC_FILE    "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon.rc")
set(APP_QRC_FILE   "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_resources.qrc")

add_executable(notepad-safe WIN32
    main.cpp
    MainWindow.cpp
    EditorTab.cpp
    TextEncoding.cpp
    FindReplaceDialog.cpp
    SearchResultsDock.cpp
    SessionManager.cpp
    ${APP_RC_FILE}
    ${APP_QRC_FILE}
)

target_include_directories(notepad-safe PRIVATE ${QSCINTILLA_INCLUDE_DIR})

target_link_libraries(notepad-safe PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
    Qt6::Svg
    Qt6::SvgWidgets
    ${QSCINTILLA_LIB}
)

# Windows 静态 Qt 需要显式链接系统库
if(WIN32)
    target_link_libraries(notepad-safe PRIVATE
        winmm
        imm32
        ws2_32
        ole32
        oleaut32
        advapi32
        shell32
        gdi32
        user32
        comdlg32
        uuid
        version
        netapi32
        userenv
        dwmapi
        uxtheme
        d2d1
        dwrite
        d3d11
        dxgi
        dxguid
        strmiids
        crypt32
    )
endif()

# 静态构建时显式导入平台插件
qt_import_plugins(notepad-safe
    INCLUDE Qt6::QWindowsIntegrationPlugin
    INCLUDE Qt6::QSvgPlugin
    INCLUDE Qt6::QSvgIconPlugin
)

# ============ 测试 ============

# Windows 静态 Qt 需要的系统库（复用于测试）
set(WIN_SYSTEM_LIBS
    winmm imm32 ws2_32 ole32 oleaut32 advapi32 shell32
    gdi32 user32 comdlg32 uuid version netapi32 userenv
    dwmapi uxtheme d2d1 dwrite d3d11 dxgi dxguid strmiids crypt32
)

# 测试 1: 编码检测（仅需 Qt6::Core）
add_executable(test_textencoding
    tests/test_textencoding.cpp
    TextEncoding.cpp
)
target_link_libraries(test_textencoding PRIVATE Qt6::Core)
if(WIN32)
    target_link_libraries(test_textencoding PRIVATE ${WIN_SYSTEM_LIBS})
endif()
qt_import_plugins(test_textencoding INCLUDE Qt6::QWindowsIntegrationPlugin)

# 测试 2: 正则搜索逻辑（仅需 Qt6::Core）
add_executable(test_search
    tests/test_search.cpp
)
target_link_libraries(test_search PRIVATE Qt6::Core)
if(WIN32)
    target_link_libraries(test_search PRIVATE ${WIN_SYSTEM_LIBS})
endif()
qt_import_plugins(test_search INCLUDE Qt6::QWindowsIntegrationPlugin)

# 测试 3: 会话序列化（需 Qt6::Core + QScintilla，因为 SessionManager.cpp 依赖 EditorTab）
add_executable(test_session
    tests/test_session.cpp
    SessionManager.cpp
    EditorTab.cpp
    TextEncoding.cpp
)
target_include_directories(test_session PRIVATE ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(test_session PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
    ${QSCINTILLA_LIB}
)
if(WIN32)
    target_link_libraries(test_session PRIVATE ${WIN_SYSTEM_LIBS})
endif()
qt_import_plugins(test_session INCLUDE Qt6::QWindowsIntegrationPlugin)

# 测试 4: 设置持久化 & 菜单 UI 勾选同步（纯逻辑，不创建 QWidget）
# 防回归："开启自动换行后关闭再打开，菜单勾选状态不同步"
add_executable(test_settings_ui
    tests/test_settings_ui.cpp
)
target_link_libraries(test_settings_ui PRIVATE
    Qt6::Core
)
if(WIN32)
    target_link_libraries(test_settings_ui PRIVATE ${WIN_SYSTEM_LIBS})
endif()

# 测试 5: 大文件分块加载回归测试
# 防回归："打开大文件时进度对话框正常显示，但加载完成后编辑框无内容"
# 使用 QApplication（非 QCoreApplication），与 notepad-safe.exe 主程序相同的初始化方式。
add_executable(test_largefile
    tests/test_largefile.cpp
)
target_include_directories(test_largefile PRIVATE ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(test_largefile PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    ${QSCINTILLA_LIB}
)
if(WIN32)
    target_link_libraries(test_largefile PRIVATE ${WIN_SYSTEM_LIBS})
endif()
qt_import_plugins(test_largefile
    INCLUDE Qt6::QWindowsIntegrationPlugin
)

# 测试目标
enable_testing()
add_test(NAME textencoding COMMAND test_textencoding)
add_test(NAME search       COMMAND test_search)
add_test(NAME session      COMMAND test_session)
add_test(NAME settings_ui  COMMAND test_settings_ui)
add_test(NAME largefile    COMMAND test_largefile)
