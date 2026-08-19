cmake_minimum_required(VERSION 3.22)
project(notepad-safe VERSION 0.3.1 LANGUAGES C CXX)

option(NOTEPADSAFE_ENABLE_KDBX_BACKEND
    "Build the pinned KeePassXC/Botan/Argon2/zlib production vault backend"
    ON
)
option(NOTEPADSAFE_ENABLE_ASAN
    "Instrument Notepad Safe targets with MSVC AddressSanitizer"
    OFF
)
option(NOTEPADSAFE_BUILD_TESTS
    "Build unit, integration, E2E, and GUI smoke test targets"
    OFF
)
option(NOTEPADSAFE_BUILD_DIAGNOSTICS
    "Build developer-only diagnostics (requires NOTEPADSAFE_BUILD_TESTS)"
    OFF
)
if(NOTEPADSAFE_BUILD_DIAGNOSTICS AND NOT NOTEPADSAFE_BUILD_TESTS)
    message(FATAL_ERROR
        "NOTEPADSAFE_BUILD_DIAGNOSTICS requires NOTEPADSAFE_BUILD_TESTS=ON")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 强制使用静态 CRT（/MT），与 Qt 静态库编译选项匹配
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC AND NOTEPADSAFE_ENABLE_ASAN)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/fsanitize=address /INCREMENTAL:NO /DEBUG)
endif()

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

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network PrintSupport Svg SvgWidgets Concurrent TaskTree)
include(cmake/PCRE2.cmake)

# QScintilla 静态库（相对于产品根，clone 后即可构建）
set(QSCINTILLA_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QScintilla_src-2.14.1/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QScintilla_src-2.14.1/scintilla/include"
)
set(QSCINTILLA_LIB "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QScintilla_src-2.14.1/src/release/qscintilla2_qt6.lib")

# 应用资源：EXE 图标/版本 RC + 窗口/任务栏 QRC。RC 与 About、ZIP 共用
# project(VERSION)，避免发布时出现三个版本号各自更新。
set(APP_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/app_icon.rc")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon.rc"
    "${APP_RC_FILE}"
    @ONLY
)
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

add_library(notepadsafe_search_runtime STATIC
    src/search/Pcre2Regex.cpp
)
target_include_directories(notepadsafe_search_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/src/search
)
target_link_libraries(notepadsafe_search_runtime PUBLIC Qt6::Core notepadsafe_pcre2)

# Stable product-domain policies. Keep file-tier and draft-storage rules free of
# QWidget/QScintilla dependencies so they can be tested at exact byte boundaries.
add_library(notepadsafe_domain STATIC
    src/domain/FileTierPolicy.cpp
    src/session/DraftStore.cpp
)
target_include_directories(notepadsafe_domain PUBLIC
    ${CMAKE_SOURCE_DIR}/src/domain
    ${CMAKE_SOURCE_DIR}/src/session
)
target_link_libraries(notepadsafe_domain PUBLIC Qt6::Core)
if(WIN32)
    target_link_libraries(notepadsafe_domain PRIVATE crypt32)
endif()

add_library(notepadsafe_vault STATIC
    src/vault/SecureBuffer.cpp
    src/vault/VaultService.cpp
    src/vault/KdbxFormatProbe.cpp
    src/vault/VaultBackendFactory.cpp
)
target_include_directories(notepadsafe_vault PUBLIC
    ${CMAKE_SOURCE_DIR}/src/vault
    ${CMAKE_SOURCE_DIR}/src/domain
)
target_link_libraries(notepadsafe_vault PUBLIC Qt6::Core)

if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    include(cmake/KeePassXCCore.cmake)
    target_sources(notepadsafe_vault PRIVATE
        src/vault/KeePassXCVaultBackend.cpp
        src/vault/SecurityIdentity.cpp
        src/vault/SecuritySessionService.cpp
        src/vault/SecureDocumentService.cpp
    )
    set_property(TARGET notepadsafe_vault PROPERTY CXX_STANDARD 20)
    target_link_libraries(notepadsafe_vault PRIVATE notepadsafe_keepassxc_core)
    target_compile_definitions(notepadsafe_vault PUBLIC NOTEPADSAFE_HAS_KDBX_BACKEND=1)
    if(WIN32)
        target_link_libraries(notepadsafe_vault PRIVATE crypt32)
    endif()
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
    set(NOTEPADSAFE_SBOM "${CMAKE_CURRENT_BINARY_DIR}/notepad-safe.spdx.json")
    add_custom_command(
        OUTPUT "${NOTEPADSAFE_SBOM}"
        COMMAND ${NOTEPADSAFE_POWERSHELL_EXECUTABLE}
            -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_SOURCE_DIR}/tools/generate_sbom.ps1
            -LockFile ${CMAKE_SOURCE_DIR}/third_party/dependencies.lock.json
            -OutputFile "${NOTEPADSAFE_SBOM}"
            -ProductVersion ${PROJECT_VERSION}
        DEPENDS
            ${CMAKE_SOURCE_DIR}/tools/generate_sbom.ps1
            ${CMAKE_SOURCE_DIR}/third_party/dependencies.lock.json
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
    add_custom_target(generate_sbom DEPENDS "${NOTEPADSAFE_SBOM}")
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
    src/editor/WindowedReadOnlyProvider.cpp
    src/async/AsyncTask.cpp
    src/async/AsyncThreadPool.cpp
    src/async/AsyncTaskManager.cpp
    src/async/StatusBarTaskWidget.cpp
    src/async/AsyncTaskPanelDialog.cpp
    src/vault/VaultDock.cpp
    ${APP_RC_FILE}
    ${APP_QRC_FILE}
)

if(WIN32)
    add_dependencies(notepad-safe generate_sbom)
endif()

target_include_directories(notepad-safe PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_compile_definitions(notepad-safe PRIVATE
    NOTEPADSAFE_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(notepad-safe PRIVATE
    notepadsafe_domain
    notepadsafe_vault
    notepadsafe_search_runtime
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
        wtsapi32
    )
endif()

# 静态构建时显式导入平台插件
qt_import_plugins(notepad-safe
    INCLUDE Qt6::QWindowsIntegrationPlugin
    INCLUDE Qt6::QSvgPlugin
    INCLUDE Qt6::QSvgIconPlugin
)

# Portable Windows release. Qt and the C runtime are static in the supported
# release build, so the package deliberately contains one executable plus the
# license, dependency lock, SPDX SBOM and user-facing documentation.
install(TARGETS notepad-safe RUNTIME DESTINATION .)
install(FILES
    README.md
    THIRD_PARTY_NOTICES.md
    third_party/QScintilla_src-2.14.1/LICENSE
    DESTINATION .
)
install(FILES
    third_party/dependencies.lock.json
    "${NOTEPADSAFE_SBOM}"
    DESTINATION metadata
)
install(FILES
    docs/TARGET_EXPERIENCE.md
    docs/SUPPLY_CHAIN.md
    DESTINATION docs
)

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "NotepadSafe")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "NotepadSafe-${PROJECT_VERSION}-windows-x64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_RESOURCE_FILE_LICENSE
    "${CMAKE_SOURCE_DIR}/third_party/QScintilla_src-2.14.1/LICENSE")
set(CMAKE_SKIP_INSTALL_ALL_DEPENDENCY TRUE)
include(CPack)
if(TARGET package)
    add_dependencies(package notepad-safe generate_sbom)
endif()
add_custom_target(portable_package
    COMMAND "${CMAKE_CPACK_COMMAND}"
        -G ZIP --config "${CMAKE_CURRENT_BINARY_DIR}/CPackConfig.cmake"
    COMMAND ${NOTEPADSAFE_POWERSHELL_EXECUTABLE}
        -NoProfile -ExecutionPolicy Bypass
        -File ${CMAKE_SOURCE_DIR}/tools/verify_release_package.ps1
        -Package "${CMAKE_CURRENT_BINARY_DIR}/NotepadSafe-${PROJECT_VERSION}-windows-x64.zip"
        -ProductExecutable "$<TARGET_FILE:notepad-safe>"
        -LockFile ${CMAKE_SOURCE_DIR}/third_party/dependencies.lock.json
        -ProductVersion ${PROJECT_VERSION}
    DEPENDS notepad-safe generate_sbom
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Create the portable Notepad Safe release ZIP"
    VERBATIM
)

# ============ 测试 ============

# Product builds intentionally stop above this line. Tests are explicit opt-in
# targets so build.bat cannot accidentally compile stale experiments or GUI
# harnesses into the default developer/release build graph.
if(NOTEPADSAFE_BUILD_TESTS)
find_package(Qt6 REQUIRED COMPONENTS Test)

# Windows 静态 Qt 需要的系统库（复用于测试）
set(WIN_SYSTEM_LIBS
    winmm imm32 ws2_32 ole32 oleaut32 advapi32 shell32
    gdi32 user32 comdlg32 uuid version netapi32 userenv
    dwmapi uxtheme d2d1 dwrite d3d11 dxgi dxguid strmiids crypt32 wtsapi32
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

add_executable(test_pcre2_regex
    tests/test_pcre2_regex.cpp
)
target_include_directories(test_pcre2_regex PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_pcre2_regex PRIVATE notepadsafe_search_runtime Qt6::Core)
if(WIN32)
    target_link_libraries(test_pcre2_regex PRIVATE ${WIN_SYSTEM_LIBS})
endif()

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
    src/editor/WindowedReadOnlyProvider.cpp
    src/async/AsyncTask.cpp
    src/async/AsyncThreadPool.cpp
    src/async/AsyncTaskManager.cpp
)
target_include_directories(test_session PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(test_session PRIVATE
    notepadsafe_domain
    notepadsafe_search_runtime
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

# Test 5: exact S1/S2/S3 boundaries and permanent large-file read-only policy.
add_executable(test_filetierpolicy
    tests/test_filetierpolicy.cpp
)
target_include_directories(test_filetierpolicy PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_filetierpolicy PRIVATE notepadsafe_domain Qt6::Core)
if(WIN32)
    target_link_libraries(test_filetierpolicy PRIVATE ${WIN_SYSTEM_LIBS})
endif()

# Test 6: secret-memory/service-state contract plus pinned KeePassXC fixtures.
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

    add_executable(test_security_identity tests/test_security_identity.cpp)
    target_include_directories(test_security_identity PRIVATE ${APP_INCLUDE_DIRS})
    target_link_libraries(test_security_identity PRIVATE notepadsafe_vault Qt6::Core)
    if(WIN32)
        target_link_libraries(test_security_identity PRIVATE ${WIN_SYSTEM_LIBS})
    endif()
endif()

set(NOTEPADSAFE_NON_GUI_TEST_TARGETS
    test_textencoding
    test_search
    test_pcre2_regex
    test_session
    test_settings_ui
    test_filetierpolicy
    test_vault_contract
)
if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    list(APPEND NOTEPADSAFE_NON_GUI_TEST_TARGETS
        test_kdbx_backend
        test_security_identity
    )
endif()
add_custom_target(notepadsafe_non_gui_tests
    DEPENDS ${NOTEPADSAFE_NON_GUI_TEST_TARGETS}
)

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
add_test(NAME pcre2_regex  COMMAND test_pcre2_regex)
add_test(NAME session      COMMAND test_session)
add_test(NAME settings_ui  COMMAND test_settings_ui)
add_test(NAME filetierpolicy COMMAND test_filetierpolicy)
add_test(NAME vault_contract COMMAND test_vault_contract)
if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    add_test(NAME kdbx_backend COMMAND test_kdbx_backend)
    set_tests_properties(kdbx_backend PROPERTIES LABELS "vault;integration" TIMEOUT 120)
    add_test(NAME security_identity COMMAND test_security_identity)
    set_tests_properties(security_identity PROPERTIES LABELS "vault;security" TIMEOUT 120)
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

if(NOTEPADSAFE_BUILD_DIAGNOSTICS)

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

endif()

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
    src/editor/WindowedReadOnlyProvider.cpp
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
    notepadsafe_search_runtime
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

# Large-file acceptance: S2/S3 mmap browse/search/copy stay read-only and no
# background task constructs a full Scintilla document.
add_executable(gui_smoke_large_readonly
    tests/gui_smoke/smoke_large_readonly.cpp
    ${GUI_SMOKE_SHARED_SOURCES}
)
target_compile_definitions(gui_smoke_large_readonly PRIVATE
    NOTEPADSAFE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_include_directories(gui_smoke_large_readonly PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_large_readonly PRIVATE
    notepadsafe_domain
    notepadsafe_search_runtime
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

target_include_directories(gui_smoke_findreplace PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_findreplace PRIVATE
    notepadsafe_domain
    notepadsafe_search_runtime
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
target_compile_definitions(gui_smoke_smallfixes PRIVATE
    NOTEPADSAFE_VERSION="${PROJECT_VERSION}"
)
target_include_directories(gui_smoke_smallfixes PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_smallfixes PRIVATE
    notepadsafe_domain
    notepadsafe_vault
    notepadsafe_search_runtime
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
        d3d11 dxgi dxguid strmiids crypt32 wtsapi32
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

if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    add_executable(gui_smoke_secure_documents
        tests/gui_smoke/smoke_secure_documents.cpp
        ${GUI_SMOKE_SHARED_SOURCES}
        src/window/MainWindow.cpp
        src/async/StatusBarTaskWidget.cpp
        src/async/AsyncTaskPanelDialog.cpp
        src/vault/VaultDock.cpp
    )
    target_include_directories(gui_smoke_secure_documents PRIVATE
        ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR}
    )
    target_link_libraries(gui_smoke_secure_documents PRIVATE
        notepadsafe_domain
        notepadsafe_vault
        notepadsafe_search_runtime
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
    if(WIN32)
        target_link_libraries(gui_smoke_secure_documents PRIVATE ${WIN_SYSTEM_LIBS})
    endif()
    qt_import_plugins(gui_smoke_secure_documents
        INCLUDE Qt6::QWindowsIntegrationPlugin
        INCLUDE Qt6::QSvgPlugin
        INCLUDE Qt6::QSvgIconPlugin
    )
endif()

# CTest 注册（可选；日常由 run_all_gui_smoke.bat 直跑更直观）
add_test(NAME gui_smoke_coord        COMMAND gui_smoke_coord)
add_test(NAME gui_smoke_findreplace  COMMAND gui_smoke_findreplace)
add_test(NAME gui_smoke_large_readonly COMMAND gui_smoke_large_readonly)
add_test(NAME gui_smoke_smallfixes   COMMAND gui_smoke_smallfixes)
add_test(NAME gui_smoke_vault        COMMAND gui_smoke_vault)
if(TARGET gui_smoke_secure_documents)
    add_test(NAME gui_smoke_secure_documents COMMAND gui_smoke_secure_documents)
    set_tests_properties(gui_smoke_secure_documents PROPERTIES LABELS "gui;security;vault")
endif()
set_tests_properties(
    gui_smoke_coord gui_smoke_findreplace gui_smoke_large_readonly
    gui_smoke_smallfixes gui_smoke_vault
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 60
)
if(TARGET gui_smoke_secure_documents)
    set_tests_properties(gui_smoke_secure_documents PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
endif()

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

endif() # NOTEPADSAFE_BUILD_TESTS
