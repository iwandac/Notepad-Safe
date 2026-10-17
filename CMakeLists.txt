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
option(NOTEPADSAFE_REQUIRE_TESTFILES
    "Fail configuration unless the frozen local testfiles matrix is available"
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

find_package(Qt6 6.11.1 EXACT REQUIRED COMPONENTS Core Gui Widgets Network PrintSupport Svg SvgWidgets Concurrent TaskTree)
include(cmake/PCRE2.cmake)

# QScintilla is always built from the vendored source tree by
# tools/bootstrap_qscintilla.ps1. No binary is allowed in third_party/src.
set(QSCINTILLA_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/src/qscintilla-2.14.1/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/src/qscintilla-2.14.1/scintilla/include"
)
set(QSCINTILLA_LIB
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/install/qscintilla-2.14.1-qt6.11.1-msvc/lib/qscintilla2_qt6.lib")
if(NOT EXISTS "${QSCINTILLA_LIB}")
    message(FATAL_ERROR
        "Source-built QScintilla library is missing: ${QSCINTILLA_LIB}. "
        "Run prepare_dependencies.bat from an x64 Visual Studio environment.")
endif()

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
    ${CMAKE_SOURCE_DIR}/src/app
    ${CMAKE_SOURCE_DIR}/src/domain
    ${CMAKE_SOURCE_DIR}/src/editor
    ${CMAKE_SOURCE_DIR}/src/search
    ${CMAKE_SOURCE_DIR}/src/async
    ${CMAKE_SOURCE_DIR}/src/session
    ${CMAKE_SOURCE_DIR}/src/window
    ${CMAKE_SOURCE_DIR}/src/vault
    ${CMAKE_SOURCE_DIR}/src/shell
)

add_library(notepadsafe_async_runtime STATIC
    src/async/AsyncTask.cpp
    src/async/AsyncThreadPool.cpp
    src/async/AsyncTaskManager.cpp
    src/async/AsyncSerialQueue.cpp
    src/async/DedicatedThread.cpp
)
target_include_directories(notepadsafe_async_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/src/async
)
target_link_libraries(notepadsafe_async_runtime PUBLIC
    Qt6::Core
    Qt6::Concurrent
)

add_library(notepadsafe_file_runtime STATIC
    src/file_runtime/OrdinaryReadSource.h
    src/file_runtime/OrdinaryReadSource.cpp
    src/file_runtime/ReadOnlyTextAnalysis.h
    src/file_runtime/ReadOnlyTextAnalysis.cpp
    src/editor/TextEncoding.cpp
    src/editor/DynamicThresholds.cpp
    src/editor/LineIndex.cpp
    src/editor/PatchList.cpp
    src/editor/FileContentProbe.cpp
)
target_include_directories(notepadsafe_file_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/src/file_runtime
    ${CMAKE_SOURCE_DIR}/src/editor
    ${CMAKE_SOURCE_DIR}/src/domain
)
target_link_libraries(notepadsafe_file_runtime PUBLIC
    notepadsafe_domain
    notepadsafe_async_runtime
    Qt6::Core
    Qt6::Concurrent
)

add_library(notepadsafe_search_runtime STATIC
    src/search/SearchSession.h
    src/search/EditableTextSearch.cpp
    src/search/EditableTextFindAllTask.h
    src/search/EditableTextFindAllTask.cpp
    src/search/EditableTextNavigationScan.h
    src/search/EditableTextNavigationScan.cpp
    src/search/ReadOnlySearchSource.h
    src/search/ReadOnlySearchSource.cpp
    src/search/EncodedRegexCompiler.cpp
    src/search/Pcre2Regex.cpp
    src/search/LargeFileSearch.cpp
    src/search/LargeFileSearchTasks.cpp
)
target_include_directories(notepadsafe_search_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/src/search
    ${CMAKE_SOURCE_DIR}/src/editor
    ${CMAKE_SOURCE_DIR}/src/async
)
target_link_libraries(notepadsafe_search_runtime PUBLIC
    notepadsafe_file_runtime
    notepadsafe_async_runtime
    Qt6::Core
    Qt6::Gui
    Qt6::Concurrent
    notepadsafe_pcre2
)

# Stable product-domain policies. Keep file-tier and draft-storage rules free of
# QWidget/QScintilla dependencies so they can be tested at exact byte boundaries.
add_library(notepadsafe_domain STATIC
    src/domain/AppDataLocation.cpp
    src/domain/DocumentSession.h
    src/domain/SessionDocumentPort.h
    src/domain/FileTierPolicy.cpp
    src/domain/DocumentToolRegistry.cpp
    src/domain/FeatureRegistry.cpp
    src/domain/InferenceProviderRegistry.cpp
    src/domain/PersistenceAdapterRegistry.cpp
)
target_include_directories(notepadsafe_domain PUBLIC
    ${CMAKE_SOURCE_DIR}/src/domain
    ${CMAKE_SOURCE_DIR}/src/session
)
target_link_libraries(notepadsafe_domain PUBLIC Qt6::Core)

# OS-backed recovery persistence is infrastructure, not product-domain policy.
# Editor/session/security runtimes share this one implementation without
# forcing Windows cryptography into the domain target.
add_library(notepadsafe_recovery_runtime STATIC
    src/session/DraftStore.cpp
)
target_include_directories(notepadsafe_recovery_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/src/session
)
target_link_libraries(notepadsafe_recovery_runtime PUBLIC Qt6::Core)
if(WIN32)
    target_link_libraries(notepadsafe_recovery_runtime PRIVATE crypt32)
endif()

add_library(notepadsafe_vault STATIC
    src/vault/SecureDocumentContract.h
    src/vault/SecureBuffer.cpp
    src/vault/SecuritySessionPort.h
    src/vault/VaultService.cpp
    src/vault/KdbxFormatProbe.cpp
    src/vault/VaultBackendFactory.cpp
    src/vault/SecurityServiceFactory.cpp
)
target_include_directories(notepadsafe_vault PUBLIC
    ${CMAKE_SOURCE_DIR}/src/vault
    ${CMAKE_SOURCE_DIR}/src/domain
)
target_link_libraries(notepadsafe_vault
    PUBLIC Qt6::Core
    PRIVATE notepadsafe_domain notepadsafe_recovery_runtime
)

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
    target_compile_definitions(notepadsafe_vault PRIVATE NOTEPADSAFE_HAS_KDBX_BACKEND=1)
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
    add_custom_target(verify_async_threading
        COMMAND ${NOTEPADSAFE_POWERSHELL_EXECUTABLE}
            -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_SOURCE_DIR}/tools/check_async_threading.ps1
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
    add_custom_target(verify_architecture
        COMMAND ${NOTEPADSAFE_POWERSHELL_EXECUTABLE}
            -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_SOURCE_DIR}/tools/check_architecture.ps1
            -SourceRoot ${CMAKE_SOURCE_DIR}
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

# ============ 编辑器产品运行时 ============

# The application and GUI tests must execute the same product objects.  Keep
# the test harness out of this target; everything below is production code.
set(NOTEPADSAFE_EDITOR_RUNTIME_SOURCES
    src/editor/DocumentSearchSurface.h
    src/editor/EditorTab.cpp
    src/editor/EditorTabPersistence.cpp
    src/editor/EditorTabCommands.cpp
    src/editor/DocumentClipboardCommands.cpp
    src/editor/EditorTabAccessibility.cpp
    src/editor/EditorSurfaceAccessibility.cpp
    src/editor/EditorTabLargeDocument.cpp
    src/editor/EditorTabHex.cpp
    src/editor/HexMouseSelectionController.cpp
    src/editor/HexMouseSelectionController.h
    src/editor/WindowedHexMouseSelectionController.cpp
    src/editor/WindowedHexMouseSelectionController.h
    src/editor/HexProjection.cpp
    src/editor/EditorTabLargeDocumentPersistence.cpp
    src/editor/EditorTabSearch.cpp
    src/editor/EditorSearchSource.h
    src/editor/EditorSearchSource.cpp
    src/editor/VirtualFileView.cpp
    src/editor/WindowedReadOnlyProvider.cpp
    src/editor/WindowedReadOnlyProviderLineIndex.cpp
    src/editor/ReadOnlyTailController.h
    src/editor/ReadOnlyTailController.cpp
    src/search/SearchResultsModel.cpp
    src/search/SearchNavigationRequest.h
    src/search/SearchNavigationRequest.cpp
)

add_library(notepadsafe_editor_runtime STATIC
    ${NOTEPADSAFE_EDITOR_RUNTIME_SOURCES}
)
target_include_directories(notepadsafe_editor_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/src/domain
    ${CMAKE_SOURCE_DIR}/src/editor
    ${CMAKE_SOURCE_DIR}/src/search
    ${CMAKE_SOURCE_DIR}/src/async
    ${CMAKE_SOURCE_DIR}/src/session
    ${QSCINTILLA_INCLUDE_DIR}
)
target_link_libraries(notepadsafe_editor_runtime PUBLIC
    notepadsafe_domain
    notepadsafe_recovery_runtime
    notepadsafe_file_runtime
    notepadsafe_async_runtime
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

add_library(notepadsafe_session_runtime STATIC
    src/session/SessionManager.cpp
)
target_include_directories(notepadsafe_session_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/src/session
    ${CMAKE_SOURCE_DIR}/src/domain
)
target_link_libraries(notepadsafe_session_runtime PUBLIC
    notepadsafe_domain
    notepadsafe_recovery_runtime
    Qt6::Core
)

# Executable document-open adapters sit above the editor surface.  Keeping
# them out of notepadsafe_domain prevents UI types and policy values from
# collapsing back into one dependency cycle.
add_library(notepadsafe_document_runtime STATIC
    src/app/DocumentProviderRegistry.cpp
    src/app/PlainFilePersistence.cpp
)
target_include_directories(notepadsafe_document_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/src/app
    ${CMAKE_SOURCE_DIR}/src/domain
    ${CMAKE_SOURCE_DIR}/src/editor
)
target_link_libraries(notepadsafe_document_runtime PUBLIC
    notepadsafe_domain
    notepadsafe_file_runtime
    Qt6::Core
)

# ============ 应用 Shell ============

add_library(notepadsafe_single_instance STATIC
    src/app/SingleInstanceCoordinator.cpp
)
target_include_directories(notepadsafe_single_instance PUBLIC
    ${CMAKE_SOURCE_DIR}/src/app
    ${CMAKE_SOURCE_DIR}/src/domain
)
target_link_libraries(notepadsafe_single_instance PUBLIC
    notepadsafe_domain
    notepadsafe_async_runtime
    Qt6::Core
    Qt6::Network
)

# MainWindow and its platform-facing widgets are production code. Keep them in
# one target so the formal executable and Shell GUI journeys cannot silently
# compile different implementations.
add_library(notepadsafe_app STATIC
    src/app/AppComposition.cpp
    src/app/SecureDocumentPersistence.cpp
    src/app/SecureDocumentPersistencePort.h
    src/app/StartupCoordinator.cpp
    src/app/StartupTrace.cpp
     src/app/CommandRegistry.cpp
    src/app/DocumentOpenWorkflow.cpp
    src/app/DocumentSessionView.cpp
    src/app/SaveAsDialog.cpp
    src/app/SensitiveText.cpp
     src/window/FileCredentialDialog.cpp
    src/window/MainWindow.cpp
    src/window/MainWindowCommands.cpp
    src/window/DocumentStatusPresentation.cpp
    src/window/DocumentTailActions.h
    src/window/DocumentTailActions.cpp
    src/window/MainWindowDocuments.cpp
    src/window/MainWindowExternalOpen.cpp
    src/window/MainWindowOpenRouting.cpp
    src/window/MainWindowSession.cpp
    src/window/RecoveryIssuePage.cpp
    src/window/WorkspaceRecoveryCoordinator.cpp
    src/search/FindReplaceDialog.cpp
    src/search/SingleFindAllRequest.cpp
    src/search/SingleFindAllRequest.h
    src/search/SearchResultsController.cpp
    src/search/SearchResultsDelegate.cpp
     src/search/SearchResultsViewPolicy.cpp
     src/search/SearchResultsDock.cpp
     src/search/SearchResultsFeedback.cpp
    src/async/StatusBarTaskWidget.cpp
    src/async/AsyncTaskPanelDialog.cpp
    src/vault/VaultDock.cpp
    src/vault/VaultDockEntries.cpp
    src/vault/VaultDockSupport.cpp
    src/shell/ThemeTokens.cpp
    src/shell/ThemeStyle.cpp
    src/shell/WindowChrome.cpp
    src/shell/TitleBar.cpp
)
target_include_directories(notepadsafe_app PUBLIC
    ${APP_INCLUDE_DIRS}
    ${QSCINTILLA_INCLUDE_DIR}
)
target_compile_definitions(notepadsafe_app PUBLIC
    NOTEPADSAFE_VERSION="${PROJECT_VERSION}"
)
if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    # VaultDock is the only application UI adapter that compiles conditional
    # KDBX controls. Do not leak the backend macro through every vault consumer.
    target_compile_definitions(notepadsafe_app PRIVATE
        NOTEPADSAFE_HAS_KDBX_BACKEND=1)
endif()
target_link_libraries(notepadsafe_app PUBLIC
    notepadsafe_document_runtime
    notepadsafe_editor_runtime
    notepadsafe_session_runtime
    notepadsafe_async_runtime
    notepadsafe_domain
    notepadsafe_vault
    notepadsafe_search_runtime
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    Qt6::PrintSupport
    Qt6::Svg
    Qt6::SvgWidgets
    Qt6::TaskTree
    ${QSCINTILLA_LIB}
)

# ============ 主程序 ============

add_executable(notepad-safe WIN32
    src/main.cpp
    ${APP_RC_FILE}
    ${APP_QRC_FILE}
)

if(WIN32)
    add_dependencies(notepad-safe generate_sbom verify_async_threading verify_architecture)
endif()

target_include_directories(notepad-safe PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_compile_definitions(notepad-safe PRIVATE
    NOTEPADSAFE_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(notepad-safe PRIVATE
    notepadsafe_single_instance
    notepadsafe_app
    notepadsafe_editor_runtime
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
    third_party/src/qscintilla-2.14.1/LICENSE
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
    "${CMAKE_SOURCE_DIR}/third_party/src/qscintilla-2.14.1/LICENSE")
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
)
target_include_directories(test_textencoding PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_textencoding PRIVATE notepadsafe_search_runtime Qt6::Core)
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

add_executable(test_largefile_encoded_search
    tests/test_largefile_encoded_search.cpp
)
target_include_directories(test_largefile_encoded_search PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_largefile_encoded_search PRIVATE
    notepadsafe_editor_runtime Qt6::Core Qt6::Gui Qt6::Concurrent)
if(WIN32)
    target_link_libraries(test_largefile_encoded_search PRIVATE ${WIN_SYSTEM_LIBS})
endif()

# 测试 3: 会话序列化（需 Qt6::Core + QScintilla，因为 SessionManager.cpp 依赖 EditorTab）
add_executable(test_session
    tests/test_session.cpp
)
target_include_directories(test_session PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(test_session PRIVATE
    notepadsafe_app
    notepadsafe_session_runtime
    notepadsafe_editor_runtime
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

add_executable(test_session_preservation tests/test_session_preservation.cpp)
target_include_directories(test_session_preservation PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(test_session_preservation PRIVATE notepadsafe_app Qt6::Test)
qt_import_plugins(test_session_preservation INCLUDE Qt6::QWindowsIntegrationPlugin)

# Core-only fault injection: no QApplication, widgets, clipboard or desktop.
add_executable(test_closeout_persistence tests/test_closeout_persistence.cpp)
target_include_directories(test_closeout_persistence PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_closeout_persistence PRIVATE notepadsafe_app Qt6::Test)

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

add_executable(test_architecture_contracts tests/test_architecture_contracts.cpp)
target_include_directories(test_architecture_contracts PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_architecture_contracts PRIVATE
    notepadsafe_domain notepadsafe_document_runtime Qt6::Core)
if(WIN32)
    target_link_libraries(test_architecture_contracts PRIVATE ${WIN_SYSTEM_LIBS})
endif()

add_executable(test_document_open_routing tests/test_document_open_routing.cpp)
target_include_directories(test_document_open_routing PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_document_open_routing PRIVATE notepadsafe_app Qt6::Test)
add_executable(test_theme_tokens tests/test_theme_tokens.cpp)
target_include_directories(test_theme_tokens PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_theme_tokens PRIVATE notepadsafe_app Qt6::Test)
add_executable(test_shell_logic tests/test_shell_logic.cpp)
target_include_directories(test_shell_logic PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_shell_logic PRIVATE notepadsafe_app Qt6::Test)

add_executable(test_tail_command_contracts tests/test_tail_command_contracts.cpp)
target_include_directories(test_tail_command_contracts PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_tail_command_contracts PRIVATE notepadsafe_app Qt6::Test)

add_executable(test_startup_composition tests/test_startup_composition.cpp)
target_include_directories(test_startup_composition PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(test_startup_composition PRIVATE
    notepadsafe_app Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
if(WIN32)
    target_link_libraries(test_startup_composition PRIVATE ${WIN_SYSTEM_LIBS})
endif()
qt_import_plugins(test_startup_composition INCLUDE Qt6::QWindowsIntegrationPlugin)

add_executable(test_startup_trace tests/test_startup_trace.cpp)
target_include_directories(test_startup_trace PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_startup_trace PRIVATE notepadsafe_app Qt6::Core Qt6::Test)

# Frozen real-file matrix used by local adjacent/release validation. The files
# are intentionally not source-controlled because they total about 310 MiB.
set(NOTEPADSAFE_TESTFILE_NAMES
    tinytextfile.txt
    tinytextfile-utf-16be.txt
    tinytextfile-utf-16be-utf-8-bom.txt
    tinytestfile
    smalltextfile.txt
    smalltextfile-utf-8-utf-16le.txt
    smalltextfile-utf-8.txt
    smalltestfile
)
set(NOTEPADSAFE_TESTFILES_AVAILABLE TRUE)
foreach(testfile_name IN LISTS NOTEPADSAFE_TESTFILE_NAMES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/testfiles/${testfile_name}")
        set(NOTEPADSAFE_TESTFILES_AVAILABLE FALSE)
    endif()
endforeach()
if(NOTEPADSAFE_TESTFILES_AVAILABLE)
    add_executable(test_testfile_matrix tests/test_testfile_matrix.cpp)
    target_include_directories(test_testfile_matrix PRIVATE ${APP_INCLUDE_DIRS})
    target_compile_definitions(test_testfile_matrix PRIVATE
        NOTEPADSAFE_TESTFILES_DIR="${CMAKE_SOURCE_DIR}/testfiles"
    )
    target_link_libraries(test_testfile_matrix PRIVATE
        notepadsafe_editor_runtime
        Qt6::Core
    )
    if(WIN32)
        target_link_libraries(test_testfile_matrix PRIVATE ${WIN_SYSTEM_LIBS})
    endif()
elseif(NOTEPADSAFE_REQUIRE_TESTFILES)
    message(FATAL_ERROR
        "NOTEPADSAFE_REQUIRE_TESTFILES=ON but the eight-file testfiles matrix is incomplete")
else()
    message(STATUS
        "Local testfiles matrix unavailable; test_testfile_matrix is not registered")
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

add_executable(test_single_instance
    tests/test_single_instance.cpp
)
target_include_directories(test_single_instance PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_single_instance PRIVATE
    notepadsafe_single_instance
    Qt6::Core
    Qt6::Network
    Qt6::Test
)
if(WIN32)
    target_link_libraries(test_single_instance PRIVATE ${WIN_SYSTEM_LIBS})
endif()

add_executable(single_instance_unresponsive_owner
    tests/single_instance_unresponsive_owner.cpp
)
target_include_directories(single_instance_unresponsive_owner PRIVATE
    ${APP_INCLUDE_DIRS})
target_link_libraries(single_instance_unresponsive_owner PRIVATE
    notepadsafe_domain Qt6::Core Qt6::Network)
if(WIN32)
    target_link_libraries(single_instance_unresponsive_owner PRIVATE
        ${WIN_SYSTEM_LIBS})
endif()

if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    add_executable(test_kdbx_backend tests/test_kdbx_backend.cpp)
    target_include_directories(test_kdbx_backend PRIVATE
        ${APP_INCLUDE_DIRS}
        "${NOTEPADSAFE_KEEPASSXC_SOURCE_DIR}/src"
    )
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

add_executable(test_ordinary_read_source tests/test_ordinary_read_source.cpp)
target_include_directories(test_ordinary_read_source PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(test_ordinary_read_source PRIVATE
    notepadsafe_app notepadsafe_file_runtime Qt6::Core Qt6::Test)
if(WIN32)
    target_link_libraries(test_ordinary_read_source PRIVATE ${WIN_SYSTEM_LIBS})
endif()

if(WIN32)
    # Manual native-rename diagnostic: successful integrity checks do not imply
    # every rename primitive supports replacing an open target on this host.
    add_executable(test_windows_read_rename EXCLUDE_FROM_ALL tests/test_windows_read_rename.cpp)
    target_include_directories(test_windows_read_rename PRIVATE ${APP_INCLUDE_DIRS})
    target_link_libraries(test_windows_read_rename PRIVATE
        notepadsafe_file_runtime Qt6::Core Qt6::Test ${WIN_SYSTEM_LIBS})
endif()

set(NOTEPADSAFE_NON_GUI_TEST_TARGETS
    test_ordinary_read_source
    test_textencoding
    test_search
    test_pcre2_regex
    test_largefile_encoded_search
    test_session
    test_session_preservation
    test_closeout_persistence
    test_settings_ui
    test_filetierpolicy
    test_architecture_contracts
    test_tail_command_contracts
    test_document_open_routing
    test_theme_tokens
    test_shell_logic
    test_startup_composition
    test_startup_trace
    test_vault_contract
    test_single_instance
)
if(TARGET test_testfile_matrix)
    list(APPEND NOTEPADSAFE_NON_GUI_TEST_TARGETS test_testfile_matrix)
endif()
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

# HX-R01: formal executable/process-boundary recovery. The harness links no
# product runtime and drives only the shipping executable through Win32 input.
if(WIN32)
    add_executable(e2e_hex_process_recovery
        tests/e2e_hex_process_recovery.cpp
    )
    target_link_libraries(e2e_hex_process_recovery PRIVATE
        Qt6::Core
        ${WIN_SYSTEM_LIBS}
    )
    add_dependencies(e2e_hex_process_recovery notepad-safe)
endif()

# 测试目标
enable_testing()
add_test(NAME ordinary_read_source COMMAND test_ordinary_read_source)
add_test(NAME textencoding COMMAND test_textencoding)
add_test(NAME search       COMMAND test_search)
add_test(NAME pcre2_regex  COMMAND test_pcre2_regex)
add_test(NAME largefile_encoded_search COMMAND test_largefile_encoded_search)
add_test(NAME session      COMMAND test_session)
add_test(NAME session_preservation COMMAND test_session_preservation)
add_test(NAME closeout_persistence COMMAND test_closeout_persistence)
set_tests_properties(closeout_persistence PROPERTIES
    LABELS "quick;recovery;core-only" RUN_SERIAL TRUE TIMEOUT 60)
set_tests_properties(session_preservation PROPERTIES
    LABELS "quick;recovery;NSP-RECOVERY" RUN_SERIAL TRUE TIMEOUT 30)
add_test(NAME settings_ui  COMMAND test_settings_ui)
add_test(NAME filetierpolicy COMMAND test_filetierpolicy)
add_test(NAME architecture_contracts COMMAND test_architecture_contracts)
set_tests_properties(architecture_contracts PROPERTIES
    LABELS "quick;architecture;tools;providers" TIMEOUT 30)
add_test(NAME tail_command_contracts COMMAND test_tail_command_contracts)
add_test(NAME document_open_routing COMMAND test_document_open_routing)
add_test(NAME theme_tokens COMMAND test_theme_tokens)
add_test(NAME shell_logic COMMAND test_shell_logic)
set_tests_properties(tail_command_contracts PROPERTIES
    LABELS "quick;architecture;NSP-LARGEFILE" TIMEOUT 30)
add_test(NAME startup_composition COMMAND test_startup_composition)
set_tests_properties(startup_composition PROPERTIES
    LABELS "quick;startup;architecture" TIMEOUT 30)
add_test(NAME startup_trace COMMAND test_startup_trace)
set_tests_properties(startup_trace PROPERTIES
    LABELS "quick;startup;diagnostics" TIMEOUT 30)
if(TARGET test_testfile_matrix)
    add_test(NAME testfile_matrix COMMAND test_testfile_matrix)
    set_tests_properties(testfile_matrix PROPERTIES
        LABELS "adjacent;release;testfiles;file-routing"
        TIMEOUT 60
    )
endif()
add_test(NAME vault_contract COMMAND test_vault_contract)
add_test(NAME single_instance COMMAND test_single_instance)
set_tests_properties(single_instance PROPERTIES
    LABELS "quick;architecture;ipc;single-instance" TIMEOUT 30)
if(WIN32)
    find_program(NOTEPADSAFE_POWERSHELL_EXECUTABLE
        NAMES pwsh.exe powershell.exe)
    if(NOTEPADSAFE_POWERSHELL_EXECUTABLE)
        add_test(NAME single_instance_process
            COMMAND ${NOTEPADSAFE_POWERSHELL_EXECUTABLE}
                -NoLogo -NoProfile -NonInteractive
                -ExecutionPolicy Bypass
                -File ${CMAKE_SOURCE_DIR}/tests/e2e_single_instance.ps1
                -Exe $<TARGET_FILE:notepad-safe>
                -OwnerHelper $<TARGET_FILE:single_instance_unresponsive_owner>)
        set_tests_properties(single_instance_process PROPERTIES
            LABELS "release;process;ipc;single-instance" TIMEOUT 150)
    endif()
endif()
if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    add_test(NAME kdbx_backend COMMAND test_kdbx_backend)
    set_tests_properties(kdbx_backend PROPERTIES LABELS "vault;integration" TIMEOUT 120)
    add_test(NAME security_identity COMMAND test_security_identity)
    set_tests_properties(security_identity PROPERTIES LABELS "vault;security" TIMEOUT 120)
    add_test(NAME security_identity_rotation
        COMMAND test_security_identity --master-password-rotation)
    add_test(NAME security_recovery_verification
        COMMAND test_security_identity --recovery-verification)
    add_test(NAME security_backup_retention
        COMMAND test_security_identity --single-backup-lifecycle)
    set_tests_properties(
        security_identity_rotation
        security_recovery_verification
        security_backup_retention
        PROPERTIES LABELS "vault;security" TIMEOUT 120
    )
endif()
if(WIN32)
    add_test(NAME supply_chain_lock
        COMMAND ${NOTEPADSAFE_POWERSHELL_EXECUTABLE}
            -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_SOURCE_DIR}/tools/verify_supply_chain.ps1
    )
    set_tests_properties(supply_chain_lock PROPERTIES LABELS "supply-chain")
    add_test(NAME architecture_boundary
        COMMAND ${NOTEPADSAFE_POWERSHELL_EXECUTABLE}
            -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_SOURCE_DIR}/tools/check_architecture.ps1
            -SourceRoot ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(architecture_boundary PROPERTIES
        LABELS "quick;architecture"
        TIMEOUT 30
    )
    add_test(NAME release_gate_contract
        COMMAND ${NOTEPADSAFE_POWERSHELL_EXECUTABLE}
            -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_SOURCE_DIR}/tools/test_release_gate_contract.ps1
    )
    set_tests_properties(release_gate_contract PROPERTIES
        LABELS "quick;architecture;release;tools;non-gui"
        TIMEOUT 60
    )
endif()

# ============ 诊断工具 ============

if(NOTEPADSAFE_BUILD_DIAGNOSTICS)

add_executable(diag_lineindex
    tests/diag_lineindex.cpp
)
target_include_directories(diag_lineindex PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(diag_lineindex PRIVATE
    notepadsafe_editor_runtime
    Qt6::Core
    Qt6::Concurrent
)

add_executable(diag_scroll_lines
    tests/diag_scroll_lines.cpp
)
target_include_directories(diag_scroll_lines PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(diag_scroll_lines PRIVATE
    notepadsafe_editor_runtime
    Qt6::Core
    Qt6::Concurrent
)

endif()

# ============================================================================
#  GUI 冒烟测试组件（tests/gui_smoke/）
#  - 所有 target 走 Qt6 Test（QTEST_MAIN），真实 GUI 事件模拟（QTest::mouseClick 等）
#  - Editor/Search/Session/Async 链接正式 notepadsafe_editor_runtime；测试不再
#    重编一套 GUI_SMOKE_SHARED_SOURCES。共享夹具链接 notepadsafe_app 以使用
#    实际注册的只读准入策略；不实例化主窗口，测试入口仍由 QTEST_MAIN 提供。
#  - 构建与执行由 run_all_gui_smoke.bat 统一管理
# ============================================================================
add_library(notepadsafe_gui_test_support STATIC
    tests/gui_smoke/SmokeTestBase.cpp
)
target_include_directories(notepadsafe_gui_test_support PUBLIC
    ${APP_INCLUDE_DIRS}
    ${QSCINTILLA_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/tests/gui_smoke
)
target_link_libraries(notepadsafe_gui_test_support PUBLIC
    notepadsafe_app
    notepadsafe_document_runtime
    notepadsafe_editor_runtime
    Qt6::Test
)

# —— smoke_coord_editops：坐标错乱 / 输入偏移 / 自动换行 / 光标 / 拖选 ——
add_executable(gui_smoke_coord
    tests/gui_smoke/smoke_coord_editops.cpp
    ${APP_QRC_FILE}
)
target_include_directories(gui_smoke_coord PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_coord PRIVATE
    notepadsafe_gui_test_support
    notepadsafe_editor_runtime
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
    ${APP_QRC_FILE}
)

add_executable(gui_smoke_hex_readonly_search
    tests/gui_smoke/smoke_hex_readonly_search.cpp
    ${APP_QRC_FILE}
)

# HX-R01/HX-P03: editable B1 Hex recovery primitives and external-save
# conflict protection. Kept separate from the broad shell smoke executable so
# recovery regressions have stable case IDs and bounded per-case timeouts.
add_executable(gui_smoke_hex_recovery
    tests/gui_smoke/smoke_hex_recovery.cpp
    ${APP_QRC_FILE}
)

# HX-R01: damaged/stale encrypted recovery points remain user-resolvable and
# can only be discarded through an explicit, tested product action.
add_executable(gui_smoke_recovery_issue
    tests/gui_smoke/smoke_recovery_issue.cpp
    ${APP_QRC_FILE}
)

# R3 baseline: pending and freshly restored encrypted drafts remain discoverable.
add_executable(gui_smoke_recovery_preservation
    tests/gui_smoke/smoke_recovery_preservation.cpp
    ${APP_QRC_FILE}
)

add_executable(gui_smoke_recovery_intent
    tests/gui_smoke/smoke_recovery_intent.cpp
    ${APP_QRC_FILE}
)

add_executable(gui_smoke_startup_shell
    tests/gui_smoke/smoke_startup_shell.cpp
    ${APP_QRC_FILE}
)
if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    target_compile_definitions(gui_smoke_startup_shell PRIVATE NOTEPADSAFE_HAS_KDBX_BACKEND=1)
endif()

# Product task presentation uses the same task manager and delegate as the EXE.
add_executable(gui_smoke_task_product_language
    tests/gui_smoke/smoke_task_product_language.cpp
    ${APP_QRC_FILE}
)

add_executable(gui_smoke_document_product_language
    tests/gui_smoke/smoke_document_product_language.cpp
    ${APP_QRC_FILE}
)

add_executable(gui_smoke_task_real_errors
    tests/gui_smoke/smoke_task_real_errors.cpp
    ${APP_QRC_FILE}
)

add_executable(gui_smoke_search_lifecycle
    tests/gui_smoke/smoke_search_lifecycle.cpp
    ${APP_QRC_FILE}
)
add_executable(gui_smoke_l2_queued_cancel_process
    tests/gui_smoke/smoke_l2_queued_cancel_process.cpp
    ${APP_QRC_FILE}
)
add_executable(gui_smoke_search_real_failure
    tests/gui_smoke/smoke_search_real_failure.cpp
    ${APP_QRC_FILE}
)
add_executable(gui_smoke_search_capacity
    tests/gui_smoke/smoke_search_capacity.cpp
    ${APP_QRC_FILE}
)
add_executable(gui_smoke_search_external_changes
    tests/gui_smoke/smoke_search_external_changes.cpp
    ${APP_QRC_FILE}
)
add_executable(gui_smoke_readonly_search_source
    tests/gui_smoke/smoke_readonly_search_source.cpp
    ${APP_QRC_FILE}
)
add_executable(gui_smoke_read_admission_open
    tests/gui_smoke/smoke_read_admission_open.cpp
    ${APP_QRC_FILE}
)
add_executable(gui_smoke_tail_follow
    tests/gui_smoke/smoke_tail_follow.cpp
    ${APP_QRC_FILE}
)
add_executable(gui_smoke_tail_presentation
    tests/gui_smoke/smoke_tail_presentation.cpp
    ${APP_QRC_FILE}
)
foreach(search_lifecycle_target IN ITEMS
        gui_smoke_search_lifecycle gui_smoke_l2_queued_cancel_process
        gui_smoke_search_real_failure gui_smoke_search_capacity
        gui_smoke_search_external_changes
        gui_smoke_readonly_search_source gui_smoke_read_admission_open
        gui_smoke_tail_follow gui_smoke_tail_presentation)
    target_include_directories(${search_lifecycle_target} PRIVATE
        ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
    target_link_libraries(${search_lifecycle_target} PRIVATE
        notepadsafe_app notepadsafe_gui_test_support Qt6::Test)
endforeach()

# HX-L01/L02/HX-A01: B1 Hex DPI/theme/accessibility, context-menu routing,
# and atomic clipboard-budget behaviour.
add_executable(gui_smoke_hex_accessibility
    tests/gui_smoke/smoke_hex_accessibility.cpp
    ${APP_QRC_FILE}
)

# HX-N04/N05/HX-S01/S02/S03: real B2/B3 Hex/ASCII mouse gestures cross
# bounded read-only projection windows while mutation entrances remain closed.
add_executable(gui_smoke_windowed_hex_mouse
    tests/gui_smoke/smoke_windowed_hex_mouse.cpp
    ${APP_QRC_FILE}
)

# Large-file acceptance: S2/S3 mmap browse/search/copy stay read-only and no
# background task constructs a full Scintilla document.
add_executable(gui_smoke_large_readonly
    tests/gui_smoke/smoke_large_readonly.cpp
    ${APP_QRC_FILE}
)
target_compile_definitions(gui_smoke_large_readonly PRIVATE
    NOTEPADSAFE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_include_directories(gui_smoke_large_readonly PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_large_readonly PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    notepadsafe_editor_runtime
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
    notepadsafe_app
    notepadsafe_gui_test_support
    notepadsafe_editor_runtime
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

target_include_directories(gui_smoke_hex_readonly_search PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_hex_readonly_search PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    notepadsafe_editor_runtime
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

target_include_directories(gui_smoke_hex_recovery PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_hex_recovery PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    notepadsafe_editor_runtime
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
if(WIN32)
    target_link_libraries(gui_smoke_hex_recovery PRIVATE ${WIN_SYSTEM_LIBS})
endif()

target_include_directories(gui_smoke_recovery_issue PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_recovery_issue PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    notepadsafe_editor_runtime
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

target_include_directories(gui_smoke_recovery_preservation PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_recovery_preservation PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    Qt6::Test
)

target_include_directories(gui_smoke_recovery_intent PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_recovery_intent PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    Qt6::Test
)

target_include_directories(gui_smoke_startup_shell PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_startup_shell PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    Qt6::Test
)

target_include_directories(gui_smoke_task_product_language PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_task_product_language PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    Qt6::Test
)

target_include_directories(gui_smoke_document_product_language PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_document_product_language PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    Qt6::Test
)

target_include_directories(gui_smoke_task_real_errors PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_task_real_errors PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    Qt6::Test
)

target_include_directories(gui_smoke_hex_accessibility PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_hex_accessibility PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    notepadsafe_editor_runtime
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
if(WIN32)
    target_link_libraries(gui_smoke_hex_accessibility PRIVATE ${WIN_SYSTEM_LIBS})
endif()

target_include_directories(gui_smoke_windowed_hex_mouse PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_windowed_hex_mouse PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    notepadsafe_editor_runtime
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

# Manual HX-1D B1 visual evidence capture. This is deliberately excluded from
# the default test/build graph: it drives the same product shell/runtime as the
# formal executable, but only a human review may promote its output to RG-07.
add_executable(hex_visual_evidence EXCLUDE_FROM_ALL
    tests/visual/hex_visual_evidence.cpp
    ${APP_QRC_FILE}
)
target_include_directories(hex_visual_evidence PRIVATE
    ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(hex_visual_evidence PRIVATE
    notepadsafe_app
    notepadsafe_editor_runtime
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
    ${QSCINTILLA_LIB}
)
if(WIN32)
    target_link_libraries(hex_visual_evidence PRIVATE ${WIN_SYSTEM_LIBS})
endif()
qt_import_plugins(hex_visual_evidence
    INCLUDE Qt6::QWindowsIntegrationPlugin
    INCLUDE Qt6::QSvgPlugin
    INCLUDE Qt6::QSvgIconPlugin
)

# —— smoke_smallfixes_menu_ui：§16 组 1 · 小修复组（替换快捷键 Ctrl+R 一致性 + L2 菜单禁用 7 项 + lifecycle）——
# MainWindow 引用的 StatusBarTaskWidget / AsyncTaskPanelDialog 仍在 Shell
# 测试目标中单独列出；共享编辑器实现来自正式产品 runtime。
add_executable(gui_smoke_smallfixes
    tests/gui_smoke/smoke_smallfixes_menu_ui.cpp
    ${APP_QRC_FILE}
)
target_compile_definitions(gui_smoke_smallfixes PRIVATE
    NOTEPADSAFE_VERSION="${PROJECT_VERSION}"
)
target_include_directories(gui_smoke_smallfixes PRIVATE ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(gui_smoke_smallfixes PRIVATE
    notepadsafe_app
    notepadsafe_gui_test_support
    notepadsafe_editor_runtime
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
)
target_compile_definitions(gui_smoke_findreplace PRIVATE
    NOTEPADSAFE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_include_directories(gui_smoke_vault PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(gui_smoke_vault PRIVATE
    notepadsafe_app
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
        ${APP_QRC_FILE}
    )
    target_include_directories(gui_smoke_secure_documents PRIVATE
        ${APP_INCLUDE_DIRS} ${QSCINTILLA_INCLUDE_DIR}
    )
    target_link_libraries(gui_smoke_secure_documents PRIVATE
        notepadsafe_app
        notepadsafe_gui_test_support
        notepadsafe_editor_runtime
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

# The build gate and per-function CTest registration share one target list.
# A hung GUI case therefore costs only its own bounded timeout instead of a
# 3600-second monolithic executable timeout.
set(NOTEPADSAFE_GUI_SMOKE_TARGETS
    gui_smoke_coord
    gui_smoke_findreplace
    gui_smoke_hex_readonly_search
    gui_smoke_hex_recovery
    gui_smoke_recovery_issue
    gui_smoke_recovery_preservation
    gui_smoke_recovery_intent
    gui_smoke_startup_shell
    gui_smoke_task_product_language
    gui_smoke_document_product_language
    gui_smoke_task_real_errors
    gui_smoke_search_lifecycle
    gui_smoke_l2_queued_cancel_process
    gui_smoke_search_real_failure
    gui_smoke_search_capacity
    gui_smoke_search_external_changes
    gui_smoke_readonly_search_source
    gui_smoke_read_admission_open
    gui_smoke_tail_follow
    gui_smoke_tail_presentation
    gui_smoke_hex_accessibility
    gui_smoke_windowed_hex_mouse
    gui_smoke_large_readonly
    gui_smoke_smallfixes
    gui_smoke_vault
)
if(NOTEPADSAFE_ENABLE_KDBX_BACKEND)
    target_compile_definitions(gui_smoke_vault PRIVATE
        NOTEPADSAFE_HAS_KDBX_BACKEND=1)
endif()
if(TARGET gui_smoke_secure_documents)
    list(APPEND NOTEPADSAFE_GUI_SMOKE_TARGETS gui_smoke_secure_documents)
endif()
add_custom_target(notepadsafe_gui_smoke_tests
    DEPENDS ${NOTEPADSAFE_GUI_SMOKE_TARGETS}
)

function(notepadsafe_register_qt_cases target source tier timeout feature)
    cmake_parse_arguments(CASE "" "" "KDBX_CASES;EXCLUDE_CASES" ${ARGN})
    # New/renamed Qt slots must regenerate CTest even in an incremental build.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/${source}")
    file(STRINGS "${CMAKE_SOURCE_DIR}/${source}" qt_case_lines
        REGEX "^[ \t]*void[ \t]+[A-Za-z0-9_]+[ \t]*\\(\\)")
    foreach(qt_case_line IN LISTS qt_case_lines)
        string(REGEX MATCH "void[ \t]+([A-Za-z0-9_]+)" _ "${qt_case_line}")
        set(qt_case "${CMAKE_MATCH_1}")
        if(qt_case IN_LIST CASE_EXCLUDE_CASES)
            continue()
        endif()
        if(qt_case STREQUAL "initTestCase"
           OR qt_case MATCHES "_data$"
           OR qt_case STREQUAL "cleanupTestCase"
           OR qt_case STREQUAL "init"
           OR qt_case STREQUAL "cleanup"
           OR qt_case STREQUAL "acceptNextQuestion"
           OR qt_case STREQUAL "lock")
            continue()
        endif()
        if(NOT NOTEPADSAFE_ENABLE_KDBX_BACKEND
           AND qt_case IN_LIST CASE_KDBX_CASES)
            continue()
        endif()
        set(test_name "${target}.${qt_case}")
        add_test(NAME "${test_name}" COMMAND ${target} ${qt_case})
        set_tests_properties("${test_name}" PROPERTIES
            LABELS "gui;smoke;${tier};${feature}"
            RUN_SERIAL TRUE
            TIMEOUT ${timeout}
        )
    endforeach()
endfunction()

notepadsafe_register_qt_cases(gui_smoke_coord
    tests/gui_smoke/smoke_coord_editops.cpp adjacent 120 NSP-EDIT)
notepadsafe_register_qt_cases(gui_smoke_findreplace
    tests/gui_smoke/smoke_find_replace.cpp adjacent 180 NSP-SEARCH)
notepadsafe_register_qt_cases(gui_smoke_hex_readonly_search
    tests/gui_smoke/smoke_hex_readonly_search.cpp adjacent 180 NSP-SEARCH)
notepadsafe_register_qt_cases(gui_smoke_hex_recovery
    tests/gui_smoke/smoke_hex_recovery.cpp adjacent 180 NSP-RECOVERY
    EXCLUDE_CASES close) # NoDeleteSharingFileLock helper, not a Qt slot.
notepadsafe_register_qt_cases(gui_smoke_recovery_issue
    tests/gui_smoke/smoke_recovery_issue.cpp adjacent 180 NSP-RECOVERY)
notepadsafe_register_qt_cases(gui_smoke_recovery_preservation
    tests/gui_smoke/smoke_recovery_preservation.cpp adjacent 30 NSP-RECOVERY)
notepadsafe_register_qt_cases(gui_smoke_recovery_intent
    tests/gui_smoke/smoke_recovery_intent.cpp adjacent 30 NSP-RECOVERY)
notepadsafe_register_qt_cases(gui_smoke_startup_shell
    tests/gui_smoke/smoke_startup_shell.cpp adjacent 30 "NSP-SHELL;NSP-FILE;NSP-SECURE")
notepadsafe_register_qt_cases(gui_smoke_task_product_language
    tests/gui_smoke/smoke_task_product_language.cpp adjacent 60 NSP-TASKS)
notepadsafe_register_qt_cases(gui_smoke_document_product_language
    tests/gui_smoke/smoke_document_product_language.cpp adjacent 90
    "NSP-VIEW;NSP-RECOVERY;NSP-SEARCH;NSP-SHELL")
notepadsafe_register_qt_cases(gui_smoke_task_real_errors
    tests/gui_smoke/smoke_task_real_errors.cpp adjacent 90 NSP-TASKS)
# These files also contain no-argument workload helpers, not Qt test slots.
# Register only their explicit business cases; child scenarios remain owned
# and bounded by the parent test process.
foreach(search_lifecycle_case IN ITEMS l1QueuedCancelSettlesAndAllowsImmediateRerun
        allOpenQueuedTargetCloseSettlesAndAllowsNewQuery
        l1LateCompletionPreservesNewSingleQuery
        l1QueuedNavigationCancelSettlesAndAllowsImmediateRerun)
    add_test(NAME gui_smoke_search_lifecycle.${search_lifecycle_case}
        COMMAND gui_smoke_search_lifecycle ${search_lifecycle_case})
    set_tests_properties(gui_smoke_search_lifecycle.${search_lifecycle_case}
        PROPERTIES LABELS "gui;smoke;adjacent;NSP-SEARCH;NSP-TASKS"
        RUN_SERIAL TRUE TIMEOUT 60)
endforeach()
foreach(search_process_case IN ITEMS queuedFindAllCancellation queuedFindOneCancellation
        dualLaneSourceOwnership)
    set(search_process_rows cancelAndWait sourceClose taskAddedCancel taskAddedClose
        stopCancelClosesSource sourceCloseReenters)
    if(search_process_case STREQUAL "queuedFindAllCancellation")
        list(APPEND search_process_rows terminalRerun querySwitchBack querySwitchBackInvalidRegex
            hideReopen sourceChangeTerminalReentry)
    elseif(search_process_case STREQUAL "queuedFindOneCancellation")
        list(APPEND search_process_rows mappingPendingClose mappingPendingCancel
            mappingPendingFailure canceledNavigationKeepsNewQueryBusy)
    else()
        set(search_process_rows navigationFirst findAllFirst)
    endif()
    # One owned child per CTest deadline. Do not let a cumulative outer timeout
    # kill the parent before it can clean up its own child on a product failure.
    foreach(search_process_row IN LISTS search_process_rows)
        set(search_process_test
            "gui_smoke_l2_queued_cancel_process.${search_process_case}.${search_process_row}")
        add_test(NAME ${search_process_test}
            COMMAND gui_smoke_l2_queued_cancel_process "${search_process_case}:${search_process_row}")
        set_tests_properties(${search_process_test}
            PROPERTIES LABELS "gui;smoke;adjacent;NSP-SEARCH;NSP-TASKS;process"
            RUN_SERIAL TRUE TIMEOUT 60)
    endforeach()
endforeach()
foreach(readonly_search_case IN ITEMS
        completedSpoolSurvivesCachedNavigationAndDifferentQuery
        partialBatchCancellationSurvivesDockReopenAndFreshQuery)
    add_test(NAME gui_smoke_readonly_search_source.${readonly_search_case}
        COMMAND gui_smoke_readonly_search_source ${readonly_search_case})
    set_tests_properties(gui_smoke_readonly_search_source.${readonly_search_case}
        PROPERTIES LABELS "gui;smoke;adjacent;NSP-SEARCH;NSP-FILE"
        RUN_SERIAL TRUE TIMEOUT 60)
endforeach()
foreach(read_admission_row IN ITEMS ordinarySmallReplacement actualSecureSmallReplacement)
    add_test(NAME gui_smoke_read_admission_open.initialReadOnlyRouteSurvivesAtomicReplacement.${read_admission_row}
        COMMAND gui_smoke_read_admission_open initialReadOnlyRouteSurvivesAtomicReplacement:${read_admission_row})
    set_tests_properties(gui_smoke_read_admission_open.initialReadOnlyRouteSurvivesAtomicReplacement.${read_admission_row}
        PROPERTIES LABELS "gui;smoke;adjacent;NSP-FILE;NSP-SECURITY" RUN_SERIAL TRUE TIMEOUT 60)
endforeach()
foreach(tail_case IN ITEMS defaultOffAndUnsupportedTargets
        enableAboveEofPausesWithoutMoving menuFollowPreservesUtf8CrlfBoundaries
        pausedAppendPreservesNativeReadingAndResume sourceFaultStopsAndNeverAutoResumes
        secureReplacementStopsWithoutUnlock navigationPausesTail
        disablingBeforeLatePaintDoesNotReclaimReadingPosition splitAppendCompletesProjection
        disablingDuringTextChangedDoesNotRearmEofPaint pageUpDuringWindowReadyPausesBeforeLatePaint)
    add_test(NAME gui_smoke_tail_follow.${tail_case}
        COMMAND gui_smoke_tail_follow ${tail_case})
    set_tests_properties(gui_smoke_tail_follow.${tail_case}
        PROPERTIES LABELS "gui;smoke;adjacent;NSP-LARGEFILE;NSP-FILE"
        RUN_SERIAL TRUE TIMEOUT 60)
endforeach()
foreach(tail_presentation_case IN ITEMS darkBeforeOpenAndThemeChangesKeepReadonlyTextReadable
        tailAndOrdinaryReadonlyPositionUseBrowsingOffset)
    add_test(NAME gui_smoke_tail_presentation.${tail_presentation_case}
        COMMAND gui_smoke_tail_presentation ${tail_presentation_case})
    set_tests_properties(gui_smoke_tail_presentation.${tail_presentation_case}
        PROPERTIES LABELS "gui;smoke;adjacent;NSP-LARGEFILE;NSP-VIEW;NSP-ACCESSIBILITY"
        RUN_SERIAL TRUE TIMEOUT 90)
endforeach()
add_test(NAME gui_smoke_search_real_failure.spoolCreationFailureSettlesDomainAndTask
    COMMAND gui_smoke_search_real_failure spoolCreationFailureSettlesDomainAndTask)
set_tests_properties(gui_smoke_search_real_failure.spoolCreationFailureSettlesDomainAndTask
    PROPERTIES LABELS "gui;smoke;adjacent;NSP-SEARCH;NSP-TASKS"
    RUN_SERIAL TRUE TIMEOUT 60)
foreach(search_capacity_case IN ITEMS uiFindAllCapacityAndNavigation publicSynchronousFindAllCapacity)
    foreach(search_capacity_row IN ITEMS literal-exact-200000 literal-overflow-200001
            regex-exact-200000 regex-overflow-200001)
        set(search_capacity_test "gui_smoke_search_capacity.${search_capacity_case}.${search_capacity_row}")
        add_test(NAME ${search_capacity_test}
            COMMAND gui_smoke_search_capacity "${search_capacity_case}:${search_capacity_row}")
        set_tests_properties(${search_capacity_test}
            PROPERTIES LABELS "gui;smoke;adjacent;NSP-SEARCH" RUN_SERIAL TRUE TIMEOUT 60)
    endforeach()
endforeach()
foreach(search_capacity_replace_row IN ITEMS literal-overflow-refusal regex-overflow-refusal)
    set(search_capacity_replace_test
        "gui_smoke_search_capacity.partialReplaceAllRefusesWithoutChanges.${search_capacity_replace_row}")
    add_test(NAME ${search_capacity_replace_test}
        COMMAND gui_smoke_search_capacity
            "partialReplaceAllRefusesWithoutChanges:${search_capacity_replace_row}")
    set_tests_properties(${search_capacity_replace_test}
        PROPERTIES LABELS "gui;smoke;adjacent;NSP-SEARCH" RUN_SERIAL TRUE TIMEOUT 60)
endforeach()
get_property(recovery_registered_tests DIRECTORY PROPERTY TESTS)
foreach(search_external_row IN ITEMS append sameIdentityTruncate sameSizeAtomicReplace
        spooledSameIdentityTruncate spooledSameSizeAtomicReplace)
    set(search_external_test
        "gui_smoke_search_external_changes.completedSearchInvalidatesAndReruns.${search_external_row}")
    add_test(NAME ${search_external_test}
        COMMAND gui_smoke_search_external_changes
            "completedSearchInvalidatesAndReruns:${search_external_row}")
    set_tests_properties(${search_external_test}
        PROPERTIES LABELS "gui;smoke;adjacent;NSP-SEARCH;NSP-FILE" RUN_SERIAL TRUE TIMEOUT 60)
endforeach()
foreach(search_secure_replacement_row IN ITEMS prePollReads automaticRefresh)
    set(search_secure_replacement_test
        "gui_smoke_search_external_changes.ordinaryReadOnlyRejectsSecureReplacement.${search_secure_replacement_row}")
    add_test(NAME ${search_secure_replacement_test}
        COMMAND gui_smoke_search_external_changes
            "ordinaryReadOnlyRejectsSecureReplacement:${search_secure_replacement_row}")
    set_tests_properties(${search_secure_replacement_test}
        PROPERTIES LABELS "gui;smoke;adjacent;NSP-SEARCH;NSP-FILE;NSP-SECURITY"
        RUN_SERIAL TRUE TIMEOUT 60)
endforeach()
foreach(recovery_test IN LISTS recovery_registered_tests)
    if(recovery_test MATCHES "^gui_smoke_recovery_issue\\.tc_2b1_")
        set_property(TEST "${recovery_test}" APPEND PROPERTY LABELS
            NSP-FILE NSP-SHELL NSP-ENCODING NSP-LANGUAGE NSP-VIEW)
    endif()
endforeach()
notepadsafe_register_qt_cases(gui_smoke_hex_accessibility
    tests/gui_smoke/smoke_hex_accessibility.cpp adjacent 180 NSP-ACCESSIBILITY)
set_tests_properties(
    gui_smoke_hex_accessibility.tc_hx_1d_layout_theme_accessibility
    PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=windows:dpiawareness=0;QT_SCALE_FACTOR=1.0;NOTEPADSAFE_EXPECTED_DPR=1.0")
set(hex_dpi_cases 125 200)
set(hex_dpi_values 1.25 2.0)
foreach(scale_case scale_value IN ZIP_LISTS hex_dpi_cases hex_dpi_values)
    set(scale_test "gui_smoke_hex_accessibility.tc_hx_1d_layout_theme_accessibility_${scale_case}")
    add_test(NAME "${scale_test}" COMMAND gui_smoke_hex_accessibility
        tc_hx_1d_layout_theme_accessibility)
    set_tests_properties("${scale_test}" PROPERTIES
        LABELS "gui;smoke;adjacent;NSP-ACCESSIBILITY;dpi-${scale_case}"
        RUN_SERIAL TRUE TIMEOUT 180
        ENVIRONMENT
            "QT_QPA_PLATFORM=windows:dpiawareness=0;QT_SCALE_FACTOR=${scale_value};NOTEPADSAFE_EXPECTED_DPR=${scale_value}")
endforeach()
notepadsafe_register_qt_cases(gui_smoke_windowed_hex_mouse
    tests/gui_smoke/smoke_windowed_hex_mouse.cpp adjacent 300 NSP-LARGEFILE)
notepadsafe_register_qt_cases(gui_smoke_large_readonly
    tests/gui_smoke/smoke_large_readonly.cpp adjacent 300 NSP-LARGEFILE)
notepadsafe_register_qt_cases(gui_smoke_smallfixes
    tests/gui_smoke/smoke_smallfixes_menu_ui.cpp adjacent 180 NSP-SHELL)
notepadsafe_register_qt_cases(gui_smoke_vault
    tests/gui_smoke/smoke_vault.cpp adjacent 180 NSP-ACCOUNTS
    KDBX_CASES
        productionExtendedAccountWorkflow
        productionAccountHistoryCanRestorePassword
        productionKdbxFixtureUnlocksIntoEditor)
if(TARGET gui_smoke_secure_documents)
    notepadsafe_register_qt_cases(gui_smoke_secure_documents
        tests/gui_smoke/smoke_secure_documents.cpp adjacent 300 NSP-SECURE)
endif()

# Permanent historical regressions are also registered in the quick gate.
add_test(NAME quick.search_dock_compact
    COMMAND gui_smoke_findreplace tc_fr_041_search_dock_rows_remain_compact_across_modes)
set_tests_properties(quick.search_dock_compact PROPERTIES
    LABELS "gui;quick;NSP-SEARCH;regression"
    RUN_SERIAL TRUE TIMEOUT 60)
add_test(NAME quick.readonly_repeated_copy
    COMMAND gui_smoke_large_readonly readonlyTextNativeSelectionSupersedesSearchSelectionForEveryCopy)
set_tests_properties(quick.readonly_repeated_copy PROPERTIES
    LABELS "gui;quick;NSP-LARGEFILE;regression"
    RUN_SERIAL TRUE TIMEOUT 60)

# 真实进程 E2E：启动完整 notepad-safe.exe，并通过 Win32 获取窗口/像素。
# 串行执行，避免两个测试同时抢占前台窗口、DWM 和同机磁盘吞吐导致误判。
if(WIN32)
    add_test(NAME e2e_startup_verify     COMMAND e2e_startup_verify)
    add_test(NAME e2e_l2_content_visible COMMAND e2e_l2_content_visible)
    add_test(NAME e2e_hex_process_recovery
        COMMAND e2e_hex_process_recovery $<TARGET_FILE:notepad-safe>)
    set_tests_properties(
        e2e_startup_verify
        e2e_l2_content_visible
        PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 60
        LABELS "gui;e2e"
    )
    set_tests_properties(
        e2e_hex_process_recovery
        PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 120
        LABELS "gui;e2e;recovery"
    )
endif()

endif() # NOTEPADSAFE_BUILD_TESTS
