cmake_minimum_required(VERSION 3.16)

project(seimi-render
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# macOS 架构策略：以 Qt 库 Mach-O 架构为准（lipo -archs 读 fat header），
# 不用 CMAKE_HOST_SYSTEM_PROCESSOR——Rosetta 进程下不准（返回翻译模式而非物理架构）。

# MSVC 源码按 UTF-8 处理（third_party 的 httplib.h/cpp-mcp 是 UTF-8，默认代码页会误读）。
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------- 构建版本信息（编译期注入 main.cpp）----------
# configure 时刻取值（非每次编译，避免增量构建反复重编 main.cpp）。
# 时间强制东八区：CI 可能跑在 UTC，但分发版要显示北京时间。
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short --always HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _seimi_git_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _seimi_git_rc
    )
    if(NOT _seimi_git_rc EQUAL 0 OR _seimi_git_commit STREQUAL "")
        set(_seimi_git_commit "unknown")
    endif()
    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _seimi_git_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_seimi_git_status STREQUAL "")
        set(_seimi_git_dirty "clean")
    else()
        set(_seimi_git_dirty "dirty")
    endif()
else()
    set(_seimi_git_commit "unknown")
    set(_seimi_git_dirty "nogit")
endif()

# 强制东八区：Windows 用 PowerShell 时区转换，macOS/Linux 用 TZ=Asia/Shanghai。
set(_seimi_build_time "")
if(WIN32)
    execute_process(
        COMMAND powershell -NoProfile -Command
            "[System.TimeZoneInfo]::ConvertTimeBySystemTimeZoneId([DateTime]::UtcNow,'China Standard Time').ToString('yyyy-MM-dd HH:mm:ss')"
        OUTPUT_VARIABLE _seimi_ps_time
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _seimi_ps_rc)
    if(_seimi_ps_rc EQUAL 0 AND _seimi_ps_time)
        set(_seimi_build_time "${_seimi_ps_time} UTC+8")
    endif()
else()
    execute_process(
        COMMAND env TZ=Asia/Shanghai date "+%Y-%m-%d %H:%M:%S"
        OUTPUT_VARIABLE _seimi_date_time
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _seimi_date_rc)
    if(_seimi_date_rc EQUAL 0 AND _seimi_date_time)
        set(_seimi_build_time "${_seimi_date_time} UTC+8")
    endif()
endif()
# 回退：本地时间（标注 Local 不撒谎）。
if(_seimi_build_time STREQUAL "")
    string(TIMESTAMP _seimi_build_time "%Y-%m-%d %H:%M:%S Local")
endif()

message(STATUS "seimi-render build info:")
message(STATUS "  version    : ${PROJECT_VERSION}  (edit project(VERSION) in CMakeLists.txt to bump)")
message(STATUS "  git commit : ${_seimi_git_commit} (${_seimi_git_dirty})")
message(STATUS "  build time : ${_seimi_build_time}")

# macOS CLT（无完整 Xcode）clang 找不到 <string>：用 try_compile 实测，失败则
# 注入 -nostdinc++ + -isystem <SDK>/c++/v1。不能用路径判断（Xcode 同样没头文件但能自洽）。
if(APPLE AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(_cxx_probe_dir "${CMAKE_BINARY_DIR}/cxx_probe")
    file(WRITE "${_cxx_probe_dir}/probe.cpp" "#include <string>\nint main(){return 0;}\n")
    try_compile(_cxx_headers_ok
        "${_cxx_probe_dir}/build"
        SOURCES "${_cxx_probe_dir}/probe.cpp"
        CXX_STANDARD 17
        OUTPUT_VARIABLE _cxx_probe_output
    )
    if(_cxx_headers_ok)
        message(STATUS "seimi-render: C++ 标准库头文件可用（toolchain 自洽），无需 workaround")
    else()
        message(STATUS "seimi-render: toolchain 缺 C++ 标准库头文件（CLT-only 环境），启用 SDK workaround")

        set(_sdk_root "")
        if(CMAKE_OSX_SYSROOT AND EXISTS "${CMAKE_OSX_SYSROOT}")
            set(_sdk_root "${CMAKE_OSX_SYSROOT}")
        else()
            execute_process(
                COMMAND xcrun --show-sdk-path
                OUTPUT_VARIABLE _xcrun_sdk
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(EXISTS "${_xcrun_sdk}")
                set(_sdk_root "${_xcrun_sdk}")
            endif()
        endif()

        set(_sdk_cxx_v1 "${_sdk_root}/usr/include/c++/v1")
        if(NOT EXISTS "${_sdk_cxx_v1}/string")
            message(FATAL_ERROR
                "toolchain 缺 C++ 标准库头文件，且 SDK 内也找不到 ${_sdk_cxx_v1}/string\n"
                "SDK root = ${_sdk_root}\n"
                "解决：重装 CommandLineTools（sudo softwareupdate --install "
                "\"Command Line Tools for Xcode-16.4\"），或改装完整 Xcode。"
            )
        endif()

        # -nostdinc++ 必须配 -isystem：单用 -nostdinc++ 会关掉所有 C++ 查找路径。
        message(STATUS "  C++ headers workaround: -nostdinc++ -isystem ${_sdk_cxx_v1}")
        add_compile_options(-nostdinc++ -isystem "${_sdk_cxx_v1}")
    endif()
endif()

# ---------- Qt ----------
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    WebSockets
    WebEngineCore
    WebEngineWidgets
)

qt_standard_project_setup()

# lipo 读 Qt6Core Mach-O fat header 取架构（不受 Rosetta 欺骗），与目标架构校验一致性。
if(APPLE)
    get_target_property(_qt_core_lib Qt6::Core LOCATION)
    if(NOT _qt_core_lib)
        # 兜底：Qt cmake config 未设 LOCATION 时，从 Qt6Core_DIR 往上两级推。
        if(DEFINED Qt6Core_DIR)
            get_filename_component(_qt_lib_dir "${Qt6Core_DIR}/../.." ABSOLUTE)
            set(_qt_core_lib "${_qt_lib_dir}/QtCore.framework/QtCore")
        endif()
    endif()

    set(_qt_arch "")
    if(_qt_core_lib AND EXISTS "${_qt_core_lib}")
        execute_process(
            COMMAND lipo -archs "${_qt_core_lib}"
            OUTPUT_VARIABLE _qt_arch_out
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _qt_arch_rc)
        if(_qt_arch_rc EQUAL 0)
            separate_arguments(_qt_arch UNIX_COMMAND "${_qt_arch_out}")
            list(LENGTH _qt_arch _qt_arch_count)
            if(_qt_arch_count EQUAL 1)
                set(_qt_arch "${_qt_arch_out}")
            else()
                set(_qt_arch "${_qt_arch_out}")
            endif()
        endif()
    endif()

    if(NOT CMAKE_OSX_ARCHITECTURES)
        if(_qt_arch)
            set(CMAKE_OSX_ARCHITECTURES "${_qt_arch}" CACHE STRING "" FORCE)
            message(STATUS "seimi-render: CMAKE_OSX_ARCHITECTURES auto-set to '${_qt_arch}' (matches Qt)")
        elseif(CMAKE_HOST_SYSTEM_PROCESSOR)
            # lipo 失败兜底（罕见）：可能被 Rosetta 误导，但好过留空。
            set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}" CACHE STRING "" FORCE)
            message(STATUS "seimi-render: CMAKE_OSX_ARCHITECTURES set to '${CMAKE_HOST_SYSTEM_PROCESSOR}' (Qt arch probe failed, host fallback)")
        endif()
    endif()

    # 一致性守卫：configure 阶段报错，而非拖到链接阶段吐符号缺失。
    if(_qt_arch AND CMAKE_OSX_ARCHITECTURES)
        separate_arguments(_target_arch UNIX_COMMAND "${CMAKE_OSX_ARCHITECTURES}")
        set(_unsupported "")
        foreach(_a ${_target_arch})
            if(NOT "${_a}" IN_LIST _qt_arch)
                list(APPEND _unsupported ${_a})
            endif()
        endforeach()
        if(_unsupported)
            message(FATAL_ERROR
                "seimi-render: 目标架构 '${CMAKE_OSX_ARCHITECTURES}' 与 Qt 库架构 '${_qt_arch}' 不兼容\n"
                "  不支持的架构: ${_unsupported}\n"
                "  Qt 库路径   : ${_qt_core_lib}\n"
                "  可能原因    : 用了 x86_64 的 cmake（/usr/local/bin/cmake，跑在 Rosetta 下）或\n"
                "                终端勾选了「使用 Rosetta 打开」，导致 cmake 误把目标架构定为 x86_64\n"
                "                而 Homebrew 的 Qt 是 arm64-only。\n"
                "  解决        : 任选其一——\n"
                "                1) 改用 arm64 工具链重跑打包：PATH=/opt/homebrew/bin:$PATH bash scripts/package.sh Release clean\n"
                "                2) 卸载 Intel Homebrew 残留（/usr/local/Homebrew）后重开终端\n"
                "                3) 临时绕过：cmake -DCMAKE_OSX_ARCHITECTURES=${_qt_arch} ...")
        endif()
    endif()

    unset(_qt_core_lib)
    unset(_qt_arch)
    unset(_qt_arch_out)
    unset(_qt_arch_rc)
    unset(_qt_arch_count)
    unset(_target_arch)
    unset(_unsupported)
    unset(_a)
endif()

# ---------- cpp-httplib (单头文件，嵌入，无 OpenSSL 依赖) ----------
set(HTTPLIB_DIR ${CMAKE_SOURCE_DIR}/third_party)

find_package(Threads REQUIRED)

# ---------- MCP SDK (hkr04/cpp-mcp) ----------
# cpp-mcp 自带 httplib.h 与项目 third_party/httplib.h 同名，必须隔离 include path：
# seimi-mcp 只暴露 cpp-mcp 的 include，不泄露给主目标的 httplib。
set(MCP_DIR ${CMAKE_SOURCE_DIR}/third_party/cpp-mcp)
add_library(seimi-mcp STATIC
    src/McpServer.h
    src/McpServer.cpp
    ${MCP_DIR}/src/mcp_message.cpp
    ${MCP_DIR}/src/mcp_resource.cpp
    ${MCP_DIR}/src/mcp_server.cpp
    ${MCP_DIR}/src/mcp_sse_client.cpp
    ${MCP_DIR}/src/mcp_stdio_client.cpp
    ${MCP_DIR}/src/mcp_tool.cpp
)
# seimi-mcp 刻意不依赖 Qt，关 AUTOMOC 避免 QObject 处理。
set_target_properties(seimi-mcp PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
target_include_directories(seimi-mcp PUBLIC
    ${MCP_DIR}/include
)
target_include_directories(seimi-mcp PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${HTTPLIB_DIR}        # 统一用项目 httplib(0.47)，避免与 cpp-mcp 自带 0.19 ODR 冲突
    ${MCP_DIR}/common     # 仅 json.hpp / base64.hpp（httplib.h 已删）
)
target_compile_definitions(seimi-mcp PRIVATE
    MCP_MAX_SESSIONS=10
    MCP_SESSION_TIMEOUT=30
)
# 绕过上游 bug：cpp-mcp 的 mcp_logger.h 用 `#if _CRT_SECURE_NO_WARNINGS`（应为 #ifdef），
# 宏定义为空时 MSVC 报 C1017。定义成 1 让 #if 1 成立。
if(MSVC)
    target_compile_definitions(seimi-mcp PRIVATE _CRT_SECURE_NO_WARNINGS=1)
endif()
target_link_libraries(seimi-mcp PUBLIC Threads::Threads)

# ---------- 可执行目标 ----------
add_executable(seimi-render
    src/main.cpp
    src/RenderTask.h
    src/RenderQueue.h
    src/RenderQueue.cpp
    src/Metrics.h
    src/Metrics.cpp
    src/Environment.h
    src/Environment.cpp
    src/CookieStore.h
    src/CookieStore.cpp
    src/ProxyConfig.h
    src/ProxyConfig.cpp
    src/RenderPool.h
    src/RenderPool.cpp
    src/HttpServer.h
    src/HttpServer.cpp
    src/WsServer.h
    src/WsServer.cpp
    third_party/html2md.cpp
    third_party/table.cpp
    third_party/qaes/qaesencryption.cpp
)

target_include_directories(seimi-render PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/qaes
    ${HTTPLIB_DIR}
)

target_link_libraries(seimi-render PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
    Qt6::WebEngineCore
    Qt6::WebEngineWidgets
    Threads::Threads
    seimi-mcp
)

# macOS：产出 .app bundle（QtWebEngine 需 bundle 结构，macdeployqt 打包依赖）。
if(APPLE)
    set_target_properties(seimi-render PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.seimi.render"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_NAME "seimi-render"
        # LSBackgroundOnly=2：.app 不显示 Dock 但仍是 GUI 应用（Chromium 无窗口渲染需要 NSApp）。
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/MacOSXBundleInfo.plist.in"
    )
endif()

# 注入源码目录（开发模式 fallback）+ 构建版本信息（顶部已取值）。
target_compile_definitions(seimi-render PRIVATE
    SEIMI_ADMIN_UI_SRC_DIR="${CMAKE_SOURCE_DIR}/admin-ui"
    SEIMI_VERSION="${PROJECT_VERSION}"
    SEIMI_BUILD_TIME="${_seimi_build_time}"
    SEIMI_GIT_COMMIT="${_seimi_git_commit}"
    SEIMI_GIT_DIRTY="${_seimi_git_dirty}"
)

# 安装目标（macdeployqt 脚本定位用）。
install(TARGETS seimi-render BUNDLE DESTINATION .)
install(DIRECTORY admin-ui/ DESTINATION admin-ui)
install(FILES third_party/readability/extract.js
              third_party/readability/simplify.js
        DESTINATION third_party/readability)
install(FILES third_party/stealth/stealth.js
        DESTINATION third_party/stealth)
install(FILES third_party/serp/baidu_serp.js
              third_party/serp/bing_serp.js
              third_party/serp/google_serp.js
        DESTINATION third_party/serp)
