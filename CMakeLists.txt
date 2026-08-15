cmake_minimum_required(VERSION 3.22)
project(notepad-safe LANGUAGES CXX)

option(NOTEPADSAFE_ENABLE_KDBX_BACKEND
    "Build the pinned KeePassXC/Botan/Argon2/zlib production vault backend"
    OFF
)

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

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network PrintSupport Svg SvgWidgets Concurrent TaskTree Test)

# QScintilla 静态库（相对于产品根，clone 后即可构建）
set(QSCINTILLA_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QScintilla_src-2.14.1/src")
set(QSCINTILLA_LIB "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QScintilla_src-2.14.1/src/release/qscintilla2_qt6.lib")

# 应用资源：EXE 图标 RC + 窗口/任务栏 QRC
set(APP_RC_FILE    "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon.rc")
set(APP_QRC_FILE   "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_resources.qrc")

# ============================================================================
#  源码目录结构（按功能模块聚类）
#    src/main.cpp           — 程序入口
#    src/editor/            — 编辑器引擎：编码/行索引/补丁/虚拟视图/编辑标签页
#    src/search/            — 搜索与替换：大文件搜索/结果模型/Dock/查找替换对话框
#    src/async/             — 异步任务框架：线程池/管理器/加密/状态栏/面板对话框
#    src/session/           — 会话管理：序列化/恢复
#    src/window/            — 主窗口
# ============================================================================
set(APP_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/domain
    ${CMAKE_SOURCE_DIR}/src/editor
    ${CMAKE_SOURCE_DIR}/src/search
    ${CMAKE_SOURCE_DIR}/src/async
    ${CMAKE_SOURCE_DIR}/src/session
    ${CMAKE_SOURCE_DIR}/src/window
    ${CMAKE_SOURCE_DIR}/src/vault
)

# Stable product-domain policies. Keep file tiers and resource admission free of
# QWidget/QScintilla dependencies so they can be tested at exact byte boundaries.
add_library(notepadsafe_domain STATIC
    src/domain/FileTierPolicy.cpp
)
target_include_directories(notepadsafe_domain PUBLIC
    ${CMAKE_SOURCE_DIR}/src/domain
)
target_link_libraries(notepadsafe_domain PUBLIC Qt6::Core)

add_library(notepadsafe_vault STATIC
    src/vault/SecureBuffer.cpp
    src/vault/VaultService.cpp
    src/vault/KdbxFormatProbe.cpp
    src/vault/VaultBackendFactory.cpp
)
target_include_directories(notepadsafe_vault PUBLIC
    ${CMAKE_SOURCE_DIR}/src/vault
)
target_link_libraries(notepadsafe_vault PUBLIC Qt6::Core)

if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    include(cmake/KeePassXCCore.cmake)
    target_sources(notepadsafe_vault PRIVATE src/vault/KeePassXCVaultBackend.cpp)
    set_property(TARGET notepadsafe_vault PROPERTY CXX_STANDARD 20)
    target_link_libraries(notepadsafe_vault PRIVATE notepadsafe_keepassxc_core)
    target_compile_definitions(notepadsafe_vault PUBLIC NOTEPADSAFE_HAS_KDBX_BACKEND=1)
endif()

if(WIN32)
    find_program(NOTEPADSAFE_POWERSHELL_EXECUTABLE
        NAMES powershell.exe powershell
        REQUIRED
    )
    add_custom_target(verify_supply_chain
        COMMAND ${NOTEPADSAFE_POWERSHELL_EXECUTABLE}
            -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_SOURCE_DIR}/tools/verify_supply_chain.ps1
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
endif()

# ============ 主程序 ============

add_executable(notepad-safe WIN32
    src/main.cpp
    src/window/MainWindow.cpp
    src/editor/EditorTab.cpp
    src/editor/TextEncoding.cpp
    src/search/FindReplaceDialog.cpp
    src/search/SearchResultsDock.cpp
    src/session/SessionManager.cpp
    src/editor/DynamicThresholds.cpp
    src/editor/LineIndex.cpp
    src/editor/PatchList.cpp
    src/search/LargeFileSearch.cpp
    src/search/SearchResultsModel.cpp
    src/editor/VirtualFileView.cpp
    src/async/AsyncTask.cpp
    src/async/AsyncThreadPool.cpp
    src/async/AsyncTaskManager.cpp
    src/async/StatusBarTaskWidget.cpp
    src/async/AsyncTaskPanelDialog.cpp
    src/vault/VaultDock.cpp
    ${APP_RC_FILE}
    ${APP_QRC_FILE}
)

target_include_directories(notepad-safe PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})

target_link_libraries(notepad-safe PRIVATE
    notepadsafe_domain
    notepadsafe_vault
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::PrintSupport
    Qt6::Svg
    Qt6::SvgWidgets
    Qt6::TaskTree
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
    src/editor/TextEncoding.cpp
)
target_include_directories(test_textencoding PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_textencoding PRIVATE Qt6::Core)
if(WIN32)
    target_link_libraries(test_textencoding PRIVATE ${WIN_SYSTEM_LIBS})
endif()
qt_import_plugins(test_textencoding INCLUDE Qt6::QWindowsIntegrationPlugin)

# 测试 2: 正则搜索逻辑（仅需 Qt6::Core）
add_executable(test_search
    tests/test_search.cpp
)
target_include_directories(test_search PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_search PRIVATE Qt6::Core)
if(WIN32)
    target_link_libraries(test_search PRIVATE ${WIN_SYSTEM_LIBS})
endif()
qt_import_plugins(test_search INCLUDE Qt6::QWindowsIntegrationPlugin)

# 测试 3: 会话序列化（需 Qt6::Core + QScintilla，因为 SessionManager.cpp 依赖 EditorTab）
add_executable(test_session
    tests/test_session.cpp
    src/session/SessionManager.cpp
    src/editor/EditorTab.cpp
    src/editor/TextEncoding.cpp
    src/editor/DynamicThresholds.cpp
    src/editor/LineIndex.cpp
    src/editor/PatchList.cpp
    src/search/LargeFileSearch.cpp
    src/search/SearchResultsModel.cpp
    src/editor/VirtualFileView.cpp
    src/async/AsyncTask.cpp
    src/async/AsyncThreadPool.cpp
    src/async/AsyncTaskManager.cpp
)
target_include_directories(test_session PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(test_session PRIVATE
    notepadsafe_domain
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::PrintSupport
    Qt6::TaskTree
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
target_include_directories(test_settings_ui PRIVATE ${APP_INCLUDE_DIRS})
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
    src/editor/DynamicThresholds.cpp
    src/editor/LineIndex.cpp
    src/editor/PatchList.cpp
    src/search/LargeFileSearch.cpp
    src/search/SearchResultsModel.cpp
    src/async/AsyncTask.cpp
    src/async/AsyncThreadPool.cpp
    src/async/AsyncTaskManager.cpp
    src/editor/VirtualFileView.cpp
)
target_include_directories(test_largefile PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(test_largefile PRIVATE
    notepadsafe_domain
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::TaskTree
    ${QSCINTILLA_LIB}
)
if(WIN32)
    target_link_libraries(test_largefile PRIVATE ${WIN_SYSTEM_LIBS})
endif()
qt_import_plugins(test_largefile
    INCLUDE Qt6::QWindowsIntegrationPlugin
)

# Test 6: exact S1/S2/S3 boundaries and deterministic promotion admission.
add_executable(test_filetierpolicy
    tests/test_filetierpolicy.cpp
)
target_include_directories(test_filetierpolicy PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_filetierpolicy PRIVATE notepadsafe_domain Qt6::Core)
if(WIN32)
    target_link_libraries(test_filetierpolicy PRIVATE ${WIN_SYSTEM_LIBS})
endif()

# Test 7: secret-memory/service-state contract plus pinned KeePassXC fixtures.
add_executable(test_vault_contract
    tests/test_vault_contract.cpp
)
target_include_directories(test_vault_contract PRIVATE ${APP_INCLUDE_DIRS})
target_compile_definitions(test_vault_contract PRIVATE
    NOTEPADSAFE_KDBX_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/kdbx"
)
target_link_libraries(test_vault_contract PRIVATE notepadsafe_vault Qt6::Core)
if(WIN32)
    target_link_libraries(test_vault_contract PRIVATE ${WIN_SYSTEM_LIBS})
endif()

if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    add_executable(test_kdbx_backend tests/test_kdbx_backend.cpp)
    target_include_directories(test_kdbx_backend PRIVATE ${APP_INCLUDE_DIRS})
    target_compile_definitions(test_kdbx_backend PRIVATE
        NOTEPADSAFE_KDBX_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/kdbx"
    )
    target_link_libraries(test_kdbx_backend PRIVATE notepadsafe_vault Qt6::Core)
    if(WIN32)
        target_link_libraries(test_kdbx_backend PRIVATE ${WIN_SYSTEM_LIBS})
    endif()
endif()

# 测试 6: 端到端启动秒开验证（100MB 会话恢复 → 主窗口 < 1500ms 可见）
#  方式：QProcess 启动 notepad-safe.exe + Win32 EnumWindows 检测窗口可见时间
#  注意：此测试使用 NOTEPADSAFE_DATADIR 指向临时目录，不读写用户真实 session.json。
add_executable(e2e_startup_verify
    tests/e2e_startup_verify.cpp
)
target_include_directories(e2e_startup_verify PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(e2e_startup_verify PRIVATE
    Qt6::Core
)
if(WIN32)
    target_link_libraries(e2e_startup_verify PRIVATE ${WIN_SYSTEM_LIBS})
endif()

# 测试 7: E2E L2 内容可见性验证（打开 40MB 文件 → PrintWindow 抓客户区 → 像素分析）
# 目的：regression 历史上"打开大文件没内容显示"的三种根因不再复现：
#       · vp size=0 导致 Scintilla overlay 0x0
#       · scrollContentsBy 默认实现 viewport()->scroll 移走 Scintilla
#       · activeWindowChanged 未 emit 导致 setText 从未被调用
add_executable(e2e_l2_content_visible
    tests/e2e_l2_content_visible.cpp
)
target_include_directories(e2e_l2_content_visible PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(e2e_l2_content_visible PRIVATE
    Qt6::Core
)
if(WIN32)
    target_link_libraries(e2e_l2_content_visible PRIVATE ${WIN_SYSTEM_LIBS})
endif()

# 测试目标
enable_testing()
add_test(NAME textencoding COMMAND test_textencoding)
add_test(NAME search       COMMAND test_search)
add_test(NAME session      COMMAND test_session)
add_test(NAME settings_ui  COMMAND test_settings_ui)
add_test(NAME largefile    COMMAND test_largefile)
add_test(NAME filetierpolicy COMMAND test_filetierpolicy)
add_test(NAME vault_contract COMMAND test_vault_contract)
if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    add_test(NAME kdbx_backend COMMAND test_kdbx_backend)
    set_tests_properties(kdbx_backend PROPERTIES LABELS "vault;integration" TIMEOUT 120)
endif()
if(WIN32)
    add_test(NAME supply_chain_lock
        COMMAND ${NOTEPADSAFE_POWERSHELL_EXECUTABLE}
            -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_SOURCE_DIR}/tools/verify_supply_chain.ps1
    )
    set_tests_properties(supply_chain_lock PROPERTIES LABELS "supply-chain")
endif()

# ============ 诊断工具 ============

add_executable(diag_lineindex
    tests/diag_lineindex.cpp
    src/editor/DynamicThresholds.cpp
    src/editor/LineIndex.cpp
)
target_include_directories(diag_lineindex PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(diag_lineindex PRIVATE
    Qt6::Core
    Qt6::Concurrent
)

add_executable(diag_editops
    tests/diag_editops.cpp
    src/editor/DynamicThresholds.cpp
    src/editor/LineIndex.cpp
    src/editor/PatchList.cpp
)
target_include_directories(diag_editops PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(diag_editops PRIVATE
    Qt6::Core
    Qt6::Concurrent
)

add_executable(diag_scroll_lines
    tests/diag_scroll_lines.cpp
    src/editor/DynamicThresholds.cpp
    src/editor/LineIndex.cpp
)
target_include_directories(diag_scroll_lines PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(diag_scroll_lines PRIVATE
    Qt6::Core
    Qt6::Concurrent
)

add_executable(diag_gui_l2_editops
    tests/diag_gui_l2_editops.cpp
    src/editor/VirtualFileView.cpp
    src/editor/PatchList.cpp
    src/editor/LineIndex.cpp
    src/search/LargeFileSearch.cpp
    src/async/AsyncTaskManager.cpp
    src/async/AsyncTask.cpp
    src/async/AsyncThreadPool.cpp
    src/editor/DynamicThresholds.cpp
)
target_include_directories(diag_gui_l2_editops PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(diag_gui_l2_editops PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::PrintSupport
    Qt6::Svg
)

add_executable(diag_save_l2_chain
    tests/diag_save_l2_chain.cpp
    src/editor/PatchList.cpp
    src/editor/LineIndex.cpp
    src/editor/DynamicThresholds.cpp
)
target_include_directories(diag_save_l2_chain PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(diag_save_l2_chain PRIVATE
    Qt6::Core
    Qt6::Concurrent
)

# ============================================================================
#  GUI 冒烟测试组件（tests/gui_smoke/）
#  - 所有 target 走 Qt6 Test（QTEST_MAIN），真实 GUI 事件模拟（QTest::mouseClick 等）
#  - 不直接 link MainWindow.cpp（避免 WinMain 与 QTEST_MAIN 的 main 冲突）
#    所有场景直接实例化 EditorTab + FindReplaceDialog 驱动真实 L2 GUI 路径。
#  - 构建与执行由 run_all_gui_smoke.bat 统一管理
# ============================================================================
set(GUI_SMOKE_SHARED_SOURCES
    tests/gui_smoke/SmokeTestBase.cpp
    src/editor/VirtualFileView.cpp
    src/editor/PatchList.cpp
    src/editor/LineIndex.cpp
    src/search/LargeFileSearch.cpp
    src/search/SearchResultsModel.cpp
    src/search/SearchResultsDock.cpp
    src/editor/EditorTab.cpp
    src/search/FindReplaceDialog.cpp
    src/editor/TextEncoding.cpp
    src/session/SessionManager.cpp
    src/editor/DynamicThresholds.cpp
    src/async/AsyncTaskManager.cpp
    src/async/AsyncTask.cpp
    src/async/AsyncThreadPool.cpp
    ${APP_QRC_FILE}
)

# —— smoke_coord_editops：坐标错乱 / 输入偏移 / 自动换行 / 光标 / 拖选 ——
add_executable(gui_smoke_coord
    tests/gui_smoke/smoke_coord_editops.cpp
    ${GUI_SMOKE_SHARED_SOURCES}
)
target_include_directories(gui_smoke_coord PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_coord PRIVATE
    notepadsafe_domain
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::PrintSupport
    Qt6::Svg
    Qt6::SvgWidgets
    Qt6::TaskTree
    Qt6::Test
    ${QSCINTILLA_LIB}
)

# —— smoke_find_replace：Ctrl+H 快捷键 / 替换内容正确性 / 跳转 / 替换全部 ——
add_executable(gui_smoke_findreplace
    tests/gui_smoke/smoke_find_replace.cpp
    ${GUI_SMOKE_SHARED_SOURCES}
)
target_include_directories(gui_smoke_findreplace PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_findreplace PRIVATE
    notepadsafe_domain
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::PrintSupport
    Qt6::Svg
    Qt6::SvgWidgets
    Qt6::TaskTree
    Qt6::Test
    ${QSCINTILLA_LIB}
)

# —— smoke_smallfixes_menu_ui：§16 组 1 · 小修复组（替换快捷键 Ctrl+R 一致性 + L2 菜单禁用 7 项 + lifecycle）——
# 本目标是**唯一** link MainWindow.cpp 的 GUI smoke（其他 smoke 不 link 以避免 main 冲突）；
# MainWindow 引用的 StatusBarTaskWidget / AsyncTaskPanelDialog 也在此单独列出来（不在 GUI_SMOKE_SHARED_SOURCES）。
add_executable(gui_smoke_smallfixes
    tests/gui_smoke/smoke_smallfixes_menu_ui.cpp
    ${GUI_SMOKE_SHARED_SOURCES}
    src/window/MainWindow.cpp
    src/async/StatusBarTaskWidget.cpp
    src/async/AsyncTaskPanelDialog.cpp
    src/vault/VaultDock.cpp
)
target_include_directories(gui_smoke_smallfixes PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_smallfixes PRIVATE
    notepadsafe_domain
    notepadsafe_vault
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::PrintSupport
    Qt6::Svg
    Qt6::SvgWidgets
    Qt6::TaskTree
    Qt6::Test
    ${QSCINTILLA_LIB}
)
# Windows 静态 Qt：显式链接系统库（与 notepad-safe 一致）
if(WIN32)
    target_link_libraries(gui_smoke_smallfixes PRIVATE
        winmm imm32 ws2_32 ole32 oleaut32 advapi32
        shell32 gdi32 user32 comdlg32 uuid version
        netapi32 userenv dwmapi uxtheme d2d1 dwrite
        d3d11 dxgi dxguid strmiids crypt32
    )
endif()

# 保险库真实控件 smoke：注入内存后端，驱动解锁、账号/笔记编辑、保存、筛选与锁定。
add_executable(gui_smoke_vault
    tests/gui_smoke/smoke_vault.cpp
    src/vault/VaultDock.cpp
)
target_include_directories(gui_smoke_vault PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(gui_smoke_vault PRIVATE
    notepadsafe_vault
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Test
)
if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    target_compile_definitions(gui_smoke_vault PRIVATE
        NOTEPADSAFE_KDBX_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/kdbx"
    )
endif()
if(WIN32)
    target_link_libraries(gui_smoke_vault PRIVATE ${WIN_SYSTEM_LIBS})
endif()
qt_import_plugins(gui_smoke_vault INCLUDE Qt6::QWindowsIntegrationPlugin)

# CTest 注册（可选；日常由 run_all_gui_smoke.bat 直跑更直观）
add_test(NAME gui_smoke_coord        COMMAND gui_smoke_coord)
add_test(NAME gui_smoke_findreplace  COMMAND gui_smoke_findreplace)
add_test(NAME gui_smoke_smallfixes   COMMAND gui_smoke_smallfixes)
add_test(NAME gui_smoke_vault        COMMAND gui_smoke_vault)

# 真实进程 E2E：启动完整 notepad-safe.exe，并通过 Win32 获取窗口/像素。
# 串行执行，避免两个测试同时抢占前台窗口、DWM 和同机磁盘吞吐导致误判。
if(WIN32)
    add_test(NAME e2e_startup_verify     COMMAND e2e_startup_verify)
    add_test(NAME e2e_l2_content_visible COMMAND e2e_l2_content_visible)
    set_tests_properties(e2e_startup_verify e2e_l2_content_visible PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 60
        LABELS "gui;e2e"
    )
endif()
