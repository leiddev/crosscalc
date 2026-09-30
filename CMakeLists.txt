# ---------------------------------------------------------------------------
#  crosscalc
#
#  跨平台桌面计算器（Windows / Linux；macOS 理论可行但未验证），
#  在 webview-httplib-demo 脚手架上构建，支持三种模式：
#    标准计算器 / 科学计算器 / 程序员计算器
#
#  用到的库：
#    1. tinyexpr-plusplus https://github.com/Blake-Madden/tinyexpr-plusplus
#                        核心计算引擎（表达式求值）；本项目的唯一计算来源
#    2. cpp-httplib       https://github.com/yhirose/cpp-httplib
#                        本地 HTTP 服务器（提供 /api/* 接口）
#    3. webview           https://github.com/webview/webview
#                        原生窗口 + 系统 WebView 内核（Win: WebView2 / Linux: WebKitGTK）
#    4. cpp-embedlib      https://github.com/yhirose/cpp-embedlib
#                        把 www/ 目录里的前端资源编译进可执行文件（单文件分发）
#    5. nlohmann/json     https://github.com/nlohmann/json
#                        JSON 的序列化 / 解析
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)

project(crosscalc
    VERSION     0.1.1
    DESCRIPTION "跨平台计算器：标准 / 科学 / 程序员三种模式 (tinyexpr++ + webview)"
    LANGUAGES   CXX)

# --- 编译标准 / 编译选项 ----------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # /utf-8          : 源码与执行字符集都用 UTF-8（保证中文字符串字面量正确）
    # /MP             : 多进程编译
    # /Zc:__cplusplus : 让 __cplusplus 反映真实的 /std:c++20，而不是 MSVC 传统的 199711L。
    #                   【必须】tinyexpr-plusplus 用 `#if __cplusplus >= 202002L` 保护
    #                   BITLROTATE*/BITRROTATE* 等 8 个旋转函数，不开这个开关它们会被
    #                   静默编译掉，运行时表现为"未知函数"。
    add_compile_options(/utf-8 /MP /W3 /Zc:__cplusplus)
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    # GCC / Clang 默认就是 UTF-8 源码字符集，中文注释与字面量无需额外开关
    add_compile_options(-Wall -Wextra)
endif()

# --- 选项 -------------------------------------------------------------------
option(CROSSCALC_CONSOLE     "以控制台程序方式构建（可看到日志输出，便于调试）" OFF)
option(CROSSCALC_BUILD_TESTS "构建自动化测试（crosscalc_tests）" ON)

# ---------------------------------------------------------------------------
#  第三方依赖：全部在 configure 阶段自动下载
# ---------------------------------------------------------------------------
include(FetchContent)

# URL 方式下载的压缩包，让解压后的时间戳取"解压那一刻"而不是压缩包里记录的历史时间，
# 否则 Ninja/Make 容易被一堆陈旧的 mtime 搞出多余的重建。这个 policy 是 CMake 3.24 才有的，
# 而本项目最低支持 3.20，所以要先判一下是否存在。
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# cpp-httplib —— 单头文件 HTTP 服务器/客户端
FetchContent_Declare(cpp-httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.38.0
    GIT_SHALLOW    TRUE)

# webview —— 极简跨平台 WebView 封装
#   注意：0.11+ 的 C API 与 C++ API 是并存的（webview::webview 只是平台引擎的 type alias），
#   本项目用的是 C++ API，见 src/main.cpp。
#   Linux 上它会要求系统有 GTK + WebKitGTK 开发包（configure 阶段就会检查）。
FetchContent_Declare(webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG        0.12.0
    GIT_SHALLOW    TRUE)

# cpp-embedlib —— 把前端资源嵌入可执行文件
FetchContent_Declare(cpp-embedlib
    GIT_REPOSITORY https://github.com/yhirose/cpp-embedlib.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE)

# nlohmann/json —— JSON 序列化 / 解析（header-only）
#   用 git 而不是 release 附件里的 include.zip：本机实测 CMake 自带的
#   file(DOWNLOAD) 走 schannel 时会在 GitHub 的 release 重定向处握手失败
#   （"server close notification received"），而 git 协议下所有依赖都稳定。
#   为避免把上游自带的测试一起拉进来，显式关掉它自己的构建/安装选项。
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE)

# tinyexpr-plusplus —— 核心计算引擎
#   上游【没有】CMakeLists.txt，仓库里只有 tinyexpr.h + tinyexpr.cpp，
#   所以这里只做"取源码"，然后由下方 crosscalc_tinyexpr 目标自己编译。
#   GIT_TAG 锁定到具体 commit（而不是分支/标签），保证任何时候构建出的
#   都是同一份引擎源码，CI 与本地可复现。
set(CROSSCALC_TINYEXPR_COMMIT "404688cfdf13fdfc99ad564e4d294ef65880bc0a")
FetchContent_Declare(tinyexpr_plusplus
    GIT_REPOSITORY https://github.com/Blake-Madden/tinyexpr-plusplus.git
    GIT_TAG        ${CROSSCALC_TINYEXPR_COMMIT})

# 让 httplib 保持零外部依赖（不找 OpenSSL / zlib / brotli）
set(HTTPLIB_REQUIRE_OPENSSL          OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
# webview 只构建核心库，不构建它的示例/测试/文档
set(WEBVIEW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
# 我们只用 header-only 的 webview::core，不需要编译好的 dll/lib
set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(cpp-httplib webview cpp-embedlib nlohmann_json
                           tinyexpr_plusplus)

# nlohmann/json 的 CMakeLists 在作为子项目被引入时可正常构建成
# header-only 的 INTERFACE 目标，并自带 nlohmann_json::nlohmann_json 别名，
# 因此这里不需要再自己包一层。

# ---------------------------------------------------------------------------
#  tinyexpr-plusplus 编译成静态库（上游无 CMakeLists，需要自己包一层）
#  刻意【不】定义 TE_BITWISE_OPERATORS：
#    * 默认语义下 `^` 是乘方（科学模式符合直觉），`**` 是它的同义写法；
#    * 按位与/或/非由库提供的 BITAND()/BITOR()/BITXOR()/BITNOT() 函数完成，
#      程序员模式的按键直接插入这些函数形式，语义明确、不会歧义；
#    * `<< >> <<< >>> ~` 无论是否开这个宏都可用，直接原生使用。
#  文档见：docs/design-decisions.md
# ---------------------------------------------------------------------------
add_library(crosscalc_tinyexpr STATIC
    "${tinyexpr_plusplus_SOURCE_DIR}/tinyexpr.cpp")
target_include_directories(crosscalc_tinyexpr SYSTEM PUBLIC
    "${tinyexpr_plusplus_SOURCE_DIR}")
set_target_properties(crosscalc_tinyexpr PROPERTIES POSITION_INDEPENDENT_CODE ON)
# 第三方源码，不套用本项目的警告等级（-Wall -Wextra /W3 会刷屏）
if(MSVC)
    target_compile_options(crosscalc_tinyexpr PRIVATE /w /Zc:__cplusplus)
else()
    target_compile_options(crosscalc_tinyexpr PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
#  TE_LONG_DOUBLE —— 64 位精确整数（QWORD）的开关
#
#  tinyexpr++ 的内部数值类型 te_type 默认是 double，只能精确表示 2^53−1 以内的
#  整数（te_parser::supports_64bit() 的实现就是
#  numeric_limits<te_type>::digits >= 64）。定义为 TE_LONG_DOUBLE 后
#  te_type 变成 long double：x86-64 上是 80 位扩展精度（64 位尾数），
#  于是 8/16/32/64 位字长全都精确。
#
#  用"编译期试一下"来判断能不能放 64 位整数，而不是 sizeof(long double) 或平台名：
#    * MSVC 上 sizeof(long double) == 8（就是 double），定义了白搭；
#    * Apple Silicon 上 long double 也是 8 字节，同样没收益；
#    * 更关键的是，光看 sizeof 并不等于"尾数有 64 位"（例如 x86 的 32 位
#      long double 是 12 字节、尾数只有 64 位但另有对齐差异）。
#  tinyexpr++ 自己的判断条件就是 numeric_limits<te_type>::digits >= 64，
#  这里用同一个条件试编译，两边口径完全一致。
#  （注意 CMAKE_CXX_SIZEOF_LONG_DOUBLE 在部分 CMake 版本/平台上并不存在，
#   所以不能依赖它。）
# ---------------------------------------------------------------------------
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
#include <limits>
static_assert(std::numeric_limits<long double>::digits >= 64,
              \"long double cannot hold 64-bit integers exactly\");
int main() { return 0; }
" CROSSCALC_LONG_DOUBLE_HOLDS_64BIT)
#
#  必须是 PUBLIC：TE_LONG_DOUBLE 会改变 tinyexpr.h 里 te_type 的定义，
#  而 te_type 出现在 te_parser 的【公开接口】上（evaluate() 的返回值等）。
#  如果只有 tinyexpr.cpp 带着这个宏、而 calc_engine.cpp 不带，两边对同一个
#  函数的返回值类型理解不一致（long double vs double），就是 ODR 违反 +
#  调用约定错乱，运行时结果不可预测。让所有消费者的宏定义与它保持一致，
#  才能保证 ABI 统一。
#
#  影响：Windows 上 QWORD 会被明确拒绝（而不是给出被舍入的错误答案）；
#  Linux/macOS(x86-64) 上 QWORD 可用。前端通过 /api/platform 的
#  supports_64bit 拿到这个能力，据此禁用按钮。见 docs/design-decisions.md 第 3 条。
# ---------------------------------------------------------------------------
if(CROSSCALC_LONG_DOUBLE_HOLDS_64BIT)
    target_compile_definitions(crosscalc_tinyexpr PUBLIC TE_LONG_DOUBLE)
    set(CROSSCALC_64BIT_INTEGERS TRUE)
    set(CROSSCALC_WIDE_TYPE "long double（尾数 >= 64 位，可精确装下 64 位整数）")
else()
    set(CROSSCALC_64BIT_INTEGERS FALSE)
    set(CROSSCALC_WIDE_TYPE "double（long double 与它同宽，无法精确表示 64 位整数）")
endif()

# ---------------------------------------------------------------------------
#  crosscalc_core —— 计算核心（纯逻辑，无 GUI / 无网络依赖）
#  拆成独立静态库的意义：测试程序只链接它，不需要 GTK/WebKit/httplib，
#  CI 里跑单测既快又不会有显示相关的不稳定因素。
# ---------------------------------------------------------------------------
add_library(crosscalc_core STATIC
    src/core/calc_types.cpp
    src/core/calc_engine.cpp
    src/core/error_text.cpp
    src/core/number_format.cpp)
target_include_directories(crosscalc_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
# 用 PUBLIC 链接：crosscalc_tinyexpr 会把 TE_LONG_DOUBLE（如果平台支持）
# 和 tinyexpr.h 的 include 目录以 PUBLIC 的方式传出来，必须一路传到
# crosscalc_core 和 crosscalc_tests，否则 te_type 的 ABI 会不一致 —— 见上方
# TE_LONG_DOUBLE 一段的说明。
target_link_libraries(crosscalc_core PUBLIC crosscalc_tinyexpr)
# crosscalc_tinyexpr 是 PRIVATE 链接，但 tinyexpr.h 需要被 core 的公开头文件包含，
# 所以 include 目录要以 PUBLIC 传出去。
target_include_directories(crosscalc_core PUBLIC
    "${tinyexpr_plusplus_SOURCE_DIR}")
# 版本号注入给引擎（JSON API 会返回它，便于确认跑的是哪个构建）
target_compile_definitions(crosscalc_core PUBLIC
    CROSSCALC_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
#  把 www/ 里的前端资源嵌入成静态库，暴露 Web::FS（生成头文件 WebAssets.h）
# ---------------------------------------------------------------------------
cpp_embedlib_add(WebAssets
    FOLDER    ${CMAKE_CURRENT_SOURCE_DIR}/www
    NAMESPACE Web)

# ---------------------------------------------------------------------------
#  计算器主程序
# ---------------------------------------------------------------------------
add_executable(crosscalc src/main.cpp)

target_link_libraries(crosscalc PRIVATE
    crosscalc_core                  # 计算核心
    WebAssets                       # Web::FS —— 内嵌的前端资源
    cpp-embedlib-httplib            # httplib::mount(svr, Web::FS)
    httplib::httplib                # HTTP 服务器
    nlohmann_json::nlohmann_json    # JSON 序列化 / 解析
    webview::core)                  # webview（header-only 目标，C/C++ 两套 API 同一个头文件）

# main.cpp 用 std::thread 跑 httplib 的监听线程。Windows 上不需要额外链接，
# 但 Linux 上要 -pthread（依赖里可能已经带上，这里显式声明更稳妥）。
find_package(Threads REQUIRED)
target_link_libraries(crosscalc PRIVATE Threads::Threads)

if(NOT CROSSCALC_CONSOLE)
    # 默认构建成 GUI 程序。
    # 注意 WIN32_EXECUTABLE 只对 Windows 目标有意义，其它平台会被忽略 ——
    # 所以 Linux/macOS 构建本来就是个"控制台程序"，日志可以直接在终端看到。
    if(WIN32)
        # Windows 上运行时不弹控制台黑框。
        # MSVC 的 GUI 子系统默认找 WinMain，这里让 CRT 仍然调用标准的 main()。
        set_target_properties(crosscalc PROPERTIES WIN32_EXECUTABLE TRUE)
        if(MSVC)
            target_link_options(crosscalc PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
#  自动化测试
#  crosscalc_tests 既是 CTest 用例，也是一个带 CLI 的独立程序：
#      crosscalc_tests                 跑全部内建用例
#      crosscalc_tests --eval "2+3*4"  单条求值（供脚本 / 人工排查）
#      crosscalc_tests --vectors f.json --mode standard   跑外部黄金向量
# ---------------------------------------------------------------------------
if(CROSSCALC_BUILD_TESTS)
    enable_testing()

    add_executable(crosscalc_tests
        tests/test_calc.cpp
        tests/test_main.cpp
        tests/vector_runner.cpp)
    target_link_libraries(crosscalc_tests PRIVATE
        crosscalc_core
        nlohmann_json::nlohmann_json)   # 解析 tests/vectors/*.json
    target_include_directories(crosscalc_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests")

    add_test(NAME crosscalc.unit COMMAND crosscalc_tests)
    set_tests_properties(crosscalc.unit PROPERTIES
        PASS_REGULAR_EXPRESSION "ALL TESTS PASSED")

    # 黄金向量：tests/vectors/*.json，由 C++ 测试程序与 Python 端到端脚本共用
    add_test(NAME crosscalc.vectors
        COMMAND crosscalc_tests --vectors
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/standard.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/scientific.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/programmer.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/errors.json")
    set_tests_properties(crosscalc.vectors PROPERTIES
        PASS_REGULAR_EXPRESSION "ALL TESTS PASSED")
endif()

# ---------------------------------------------------------------------------
#  收尾提示
# ---------------------------------------------------------------------------
if(WIN32)
    set(_run_hint "build\\Release\\crosscalc.exe")
    set(_preset "x64")
else()
    set(_run_hint "build-linux/crosscalc")
    set(_preset "linux")
endif()
if(CROSSCALC_64BIT_INTEGERS)
    set(_int_hint "${CROSSCALC_WIDE_TYPE}，QWORD(64 位字长) 可用")
else()
    set(_int_hint "${CROSSCALC_WIDE_TYPE}，最大精确到 2^53-1，QWORD 不可用")
endif()
message(STATUS "----------------------------------------------------------")
message(STATUS " crosscalc ${PROJECT_VERSION} 配置完成")
message(STATUS "   平台     : ${CMAKE_SYSTEM_NAME}  (${CMAKE_GENERATOR})")
message(STATUS "   构建     : cmake --build --preset <release|linux-release>")
message(STATUS "   自测     : ctest --preset <windows-release|linux-release>")
message(STATUS "   运行     : ${_run_hint}")
message(STATUS "   整数精度 : ${_int_hint}")
message(STATUS "   预设名   : ${_preset}")
message(STATUS "----------------------------------------------------------")
