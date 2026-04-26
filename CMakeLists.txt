# squirrel — ImGui + GLFW + cpp-httplib（可选 HTTPS/OpenSSL），MSVC 下默认静态链接运行库 (/MT)。
#
# 配置与编译（64 位 Release，需已安装 Git 与 MSVC，且能访问 GitHub）:
#   建议使用 vcpkg 安装 openssl，并在配置时传入工具链，例如:
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
#   cmake --build build --config Release
#
# 若启用动态链接 OpenSSL，请将 libssl / libcrypto 的 DLL 与 exe 同目录或一并打包进安装包。
#
cmake_minimum_required(VERSION 3.16)
project(squirrel VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 NEW) # FetchContent_Populate 已弃用，子项目用 MakeAvailable
endif()

include(FetchContent)
option(APITOOL_ENABLE_HTTPS "Enable HTTPS support via OpenSSL" ON)

set(GLFW_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
)
FetchContent_MakeAvailable(imgui)

# HTTPS 需要 OpenSSL（开发库）。未安装时自动降级为 HTTP-only，避免配置直接失败。
if(APITOOL_ENABLE_HTTPS)
    if(WIN32 AND NOT OPENSSL_ROOT_DIR)
        if(DEFINED ENV{OPENSSL_ROOT_DIR})
            file(TO_CMAKE_PATH "$ENV{OPENSSL_ROOT_DIR}" _ossl)
            set(OPENSSL_ROOT_DIR "${_ossl}" CACHE PATH "OpenSSL installation root")
        endif()
    endif()
    if(WIN32 AND NOT OPENSSL_ROOT_DIR)
        foreach(_try IN ITEMS
                "C:/Program Files/OpenSSL-Win64"
                "C:/OpenSSL-Win64"
                "$ENV{VCPKG_ROOT}/installed/x64-windows"
                "$ENV{VCPKG_ROOT}/installed/x64-windows-static"
                "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/x64-windows"
                "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/x64-windows-static")
            if(_try AND EXISTS "${_try}/include/openssl/ssl.h")
                set(OPENSSL_ROOT_DIR "${_try}" CACHE PATH "OpenSSL installation root")
                break()
            endif()
        endforeach()
    endif()

    find_package(OpenSSL QUIET)
    if(NOT OpenSSL_FOUND)
        message(WARNING
            "未能找到 OpenSSL 开发库，将自动降级为 HTTP-only 构建（HTTPS 不可用）。\n"
            "如需 HTTPS，请安装 OpenSSL 并重新配置。可选方式：\n"
            "  1) vcpkg：vcpkg install openssl:x64-windows（或 static 版），配置时增加\n"
            "     -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake\n"
            "  2) 安装 Win64 OpenSSL，并设置 OPENSSL_ROOT_DIR 指向安装目录（含 include/openssl、lib）\n"
            "当前 OPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}")
        set(APITOOL_ENABLE_HTTPS OFF)
    endif()
endif()

set(HTTPLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib")
if(NOT EXISTS "${HTTPLIB_INCLUDE_DIR}/httplib.h")
    message(FATAL_ERROR
        "未找到本地 cpp-httplib 头文件：${HTTPLIB_INCLUDE_DIR}/httplib.h\n"
        "请将 https://github.com/yhirose/cpp-httplib 的 httplib.h 放到 third_party/cpp-httplib/ 后重试。")
endif()

find_package(Threads REQUIRED)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
)
target_link_libraries(imgui PUBLIC glfw)
if(WIN32)
    target_link_libraries(imgui PUBLIC opengl32)
endif()

add_executable(squirrel
    src/main.cpp
    src/HttpWin.cpp
)
target_include_directories(squirrel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${HTTPLIB_INCLUDE_DIR}
)
target_link_libraries(squirrel PRIVATE imgui Threads::Threads)
if(APITOOL_ENABLE_HTTPS)
    target_link_libraries(squirrel PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(squirrel PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
else()
    target_compile_definitions(squirrel PRIVATE APITOOL_HTTP_ONLY)
endif()
if(WIN32)
    target_link_libraries(squirrel PRIVATE ws2_32 crypt32)
    set_target_properties(squirrel PROPERTIES WIN32_EXECUTABLE TRUE)
    target_compile_definitions(squirrel PRIVATE UNICODE _UNICODE)
endif()

if(MSVC)
    target_compile_options(squirrel PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(squirrel PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # MinGW/GCC 下显式指定源码与执行字符集为 UTF-8，避免中文字符串显示为乱码。
        target_compile_options(squirrel PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    endif()
endif()
