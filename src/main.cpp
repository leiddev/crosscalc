// ---------------------------------------------------------------------------
//  main.cpp —— cpp-httplib + webview + cpp-embedlib 桌面应用示例
//  （JSON 的序列化 / 解析交给 nlohmann/json，不再手写转义与拼接）
//
//  运行结构：
//
//     [ webview 原生窗口 ]                        [ 后台线程 ]
//       │  WebView2 载入 http://127.0.0.1:<port>     │
//       └──────────────► GET /            ────────►  cpp-embedlib 内嵌资源
//       │                GET /api/*       ────────►  cpp-httplib 处理
//       └── webview::bind ─────────────────────────► 直接调用 C++ 函数（不经 HTTP）
//
// ---------------------------------------------------------------------------
#include "WebAssets.h"  // cpp-embedlib 生成的：Web::FS

#include <cpp-embedlib-httplib.h>  // httplib::mount(svr, Web::FS)
#include <httplib.h>               // cpp-httplib

#include <webview/webview.h>  // webview 0.12 的 C++ API（header-only）

#include <nlohmann/json.hpp>  // JSON 序列化 / 解析（header-only）

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>  // getpid()
#endif

namespace {

// ------------------------------------------------------------------ JSON ----
// 用 ordered_json 而不是 nlohmann::json：后者的底层是 std::map，键会按字母序输出，
// ordered_json 保留我们插入的顺序（响应体读起来和以前一模一样）。
using json = nlohmann::ordered_json;

// 统一的序列化出口。注意 resolve() / set_content() 要的是"JSON 文本"，
// 所以最后永远得 dump() 一次 —— JSON 库帮的是"造"，不是"交"。
//
// error_handler_t::replace：源字符串里若混进非法 UTF-8（比如 ?q=%FF，一个 URL 就能塞进来），
// 默认的 strict 会抛 type_error.316 被 cpp-httplib 兜成 500；
// replace 则把坏字节换成 U+FFFD，照样输出合法 JSON。
std::string json_text(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// ------------------------------------------------------- 平台差异的小常量 ----
#if defined(_WIN32)
constexpr const char* k_os_name = "windows";
constexpr const char* k_native_handle_name = "HWND";
constexpr const char* k_webview_hint = "请确认已安装 WebView2 运行时";
#elif defined(__APPLE__)
constexpr const char* k_os_name = "macos";
constexpr const char* k_native_handle_name = "NSView *";
constexpr const char* k_webview_hint = "请确认系统提供了 WebKit（macOS 自带）";
#else
constexpr const char* k_os_name = "linux";
constexpr const char* k_native_handle_name = "GtkWidget *";
constexpr const char* k_webview_hint =
    "请确认已安装 WebKitGTK 运行库（如 libwebkit2gtk-4.1-0），且当前有 X11/Wayland 显示";
#endif

// --------------------------------------------------------------- 全局状态 --
const auto g_started_at = std::chrono::steady_clock::now();
webview::webview* g_webview = nullptr;  // webview::bind 回调里要用它回传结果

// ---------------------------------------------------------------- 小工具 ----
#ifdef _WIN32
// Win32 的 *W 接口要的是 UTF-16，而项目里传给它们的都是 UTF-8 字节串
// （MSVC 那边我们加了 /utf-8，GCC 本来就是），所以必须真正转一次码。
//
// 千万不要写成 std::wstring(msg.begin(), msg.end())：那是把每个"字节"当成一个
// wchar_t 塞进去，一个汉字（UTF-8 3 字节）会变成 3 个乱码字符；又因为 MSVC 的
// char 是带符号的，字节 0xE7 还会符号扩展成 0xFFE7，屏幕上就是一串半角片假名。
// 这个坑编译器在 /W3 下不会警告，只有把对话框里的字读出来才发现。
//
// flags 传 0（不传 MB_ERR_INVALID_CHARS）是有意的：非法 UTF-8 会被换成 U+FFFD
// 而不是失败，和 json_text() 用 error_handler_t::replace 是同一个态度 ——
// 报错信息本身绝不能因为混进一个坏字节就报不出来。
std::wstring to_wstring_utf8(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int len = static_cast<int>(s.size());
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), len, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(size), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.data(), len, w.data(), size);
    }
    return w;
}
#endif

void log_line(const std::string& msg) {
    const std::string line = msg + "\n";
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
#ifdef _WIN32
    // GUI 子系统没有控制台，日志在 VS 输出窗口 / DebugView 里可见。
    // 这里同样得用 *W 版本：*A 会把 UTF-8 当成 ANSI 码页解释，中文一样是乱码。
    OutputDebugStringW(to_wstring_utf8(line).c_str());
#endif
}

[[noreturn]] void fatal(const std::string& msg) {
    log_line("[fatal] " + msg);
#ifdef _WIN32
    MessageBoxW(nullptr, to_wstring_utf8(msg).c_str(), L"webview-httplib-demo",
                MB_ICONERROR | MB_OK);
#endif
    std::exit(1);
}

std::string local_time_string() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

long long uptime_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 g_started_at)
        .count();
}

// 当前进程号：Windows 用 Win32 API，其它平台用 POSIX getpid()。
// （以前非 Windows 分支直接返回 0，Linux 上 /api/info 的 pid 就恒为 0 了。）
long current_process_id() {
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}

void reply_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(json_text(body), "application/json; charset=utf-8");
}

// 取 webview::bind 收到的 JSON 数组参数里的第一个字符串
// （req 形如 ["hello"]，是 "params" 那一段的原文）
std::string first_json_string(const std::string& args) {
    const json parsed = json::parse(args, nullptr, /* allow_exceptions = */ false);
    if (parsed.is_array() && !parsed.empty() && parsed.front().is_string()) {
        return parsed.front().get<std::string>();
    }
    return {};
}

double form_number(const httplib::Request& req, const std::string& key, bool& ok) {
    const std::string value = req.get_param_value(key);
    if (value.empty()) {
        ok = false;
        return 0.0;
    }
    try {
        std::size_t used = 0;
        const double d = std::stod(value, &used);
        ok = (used == value.size());
        return d;
    } catch (...) {
        ok = false;
        return 0.0;
    }
}

// -------------------------------------------------- webview::bind 回调（C++）--
// 这些函数由 JavaScript 直接调用，完全不走 HTTP。
// 回调签名就是 webview::engine_base::binding_t：
//     std::function<void(std::string id, std::string args, void *arg)>
// 处理完用 resolve(id, status, result) 把结果回传给 JS 的 Promise
// （对应 C API 的 webview_return；result 需是合法 JSON 文本，
//   所以这里统一走 json_text()，别直接传裸字符串）。

// JS: cppNativeEcho("...") —— 把文本交给 C++ 处理后再返回
void on_native_echo(std::string id, std::string req, void*) {
    const std::string text = first_json_string(req);
    std::string upper = text;
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    g_webview->resolve(id, 0, json_text(json{{"source", "webview::bind → C++"},
                                             {"echo", text},
                                             {"chars", text.size()},
                                             {"upper", upper},
                                             {"time", local_time_string()}}));
}

// JS: cppNativeHandle() —— 拿到原生窗口句柄，演示 JS 与原生窗口互操作
//   注意各平台拿到的类型不同：Windows 是 HWND，Linux 是 GtkWidget *，
//   这里只是把它当个不透明指针打印出来（名字由 k_native_handle_name 决定）。
void on_native_handle(std::string id, std::string, void*) {
    // C API 是 webview_get_window()；C++ 这边返回 result<void*>，要判一下
    std::uintptr_t handle_value = 0;
    if (const auto handle = g_webview->window(); handle.ok()) {
        handle_value = reinterpret_cast<std::uintptr_t>(handle.value());
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s = 0x%llX（由 webview::window() 取得）",
                  k_native_handle_name, static_cast<unsigned long long>(handle_value));
    g_webview->resolve(id, 0, json_text(json(std::string(buf))));
}

// JS: cppCloseWindow() —— 关闭窗口（结束 webview::run() 的消息循环）
void on_close_window(std::string id, std::string, void*) {
    g_webview->resolve(id, 0, json_text(json("closing...")));
    g_webview->terminate();  // C API: webview_terminate()
}

// ------------------------------------------------------------------ 路由 ----
void register_api(httplib::Server& svr) {
    // ---- GET /api/hello ---------------------------------------------------
    svr.Get("/api/hello", [](const httplib::Request&, httplib::Response& res) {
        reply_json(res, json{{"message", "你好，这是 C++ 后端（cpp-httplib）的问候"},
                             {"from", "GET /api/hello"},
                             {"time", local_time_string()}});
    });

    // ---- GET /api/time ----------------------------------------------------
    svr.Get("/api/time", [](const httplib::Request&, httplib::Response& res) {
        reply_json(res, json{{"local", local_time_string()}, {"uptime_ms", uptime_ms()}});
    });

    // ---- GET /api/echo?q=... ---------------------------------------------
    svr.Get("/api/echo", [](const httplib::Request& req, httplib::Response& res) {
        const std::string q = req.get_param_value("q");
        reply_json(res, json{{"you_said", q}, {"length", q.size()}});
    });

    // ---- POST /api/add （application/x-www-form-urlencoded: a=..&b=..）----
    svr.Post("/api/add", [](const httplib::Request& req, httplib::Response& res) {
        bool ok_a = false, ok_b = false;
        const double a = form_number(req, "a", ok_a);
        const double b = form_number(req, "b", ok_b);
        if (!ok_a || !ok_b) {
            reply_json(res, json{{"error", "需要表单参数 a 与 b，例如 a=12&b=30"}}, 400);
            return;
        }
        // 注意 a / b / sum 都是 double，所以输出是 12.0 而不是 12 ——
        // 想输出整数得自己判一下再塞 long long（这里故意不判，保持代码简单）。
        reply_json(res, json{{"a", a}, {"b", b}, {"sum", a + b}});
    });

    // ---- GET /api/assets —— 列出被 cpp-embedlib 内嵌的资源 ----------------
    svr.Get("/api/assets", [](const httplib::Request&, httplib::Response& res) {
        json files = json::array();
        for (auto it = Web::FS.begin(); it != Web::FS.end(); ++it) {
            const auto entry = *it;
            if (entry.is_dir()) { continue; }
            const auto bytes = entry.bytes();
            files.push_back(json{{"path", entry.path()},
                                 {"mime", entry.mime_type()},
                                 {"bytes", bytes ? bytes->size() : 0}});
        }
        reply_json(res, json{{"count", files.size()}, {"files", files}});
    });

    // ---- GET /api/info —— 各库的版本信息 ---------------------------------
    svr.Get("/api/info", [](const httplib::Request&, httplib::Response& res) {
        // C 的 webview_version() 只是返回 webview::detail::library_version_info
        // （见 webview.h:4552）；C++ 这边没有等价的公开函数，用同一个宏最稳。
        reply_json(res, json{{"cpp-httplib", CPPHTTPLIB_VERSION},
                             {"webview", WEBVIEW_VERSION_NUMBER},
                             {"cpp-embedlib", "main"},
                             {"nlohmann/json",
                              std::to_string(NLOHMANN_JSON_VERSION_MAJOR) + "." +
                                  std::to_string(NLOHMANN_JSON_VERSION_MINOR) + "." +
                                  std::to_string(NLOHMANN_JSON_VERSION_PATCH)},
                             {"os", k_os_name},
                             {"pid", current_process_id()},
                             {"uptime_ms", uptime_ms()}});
    });
}

// ------------------------------------------------------------------ 参数 ----
int parse_port_argument(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--port") { return std::atoi(argv[i + 1]); }
    }
    return 0;  // 0 = 自动挑选空闲端口
}

}  // namespace

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    // ---- 1. HTTP 服务器：cpp-httplib ------------------------------------
    httplib::Server svr;
    svr.set_read_timeout(5, 0);
    svr.set_payload_max_length(1024 * 1024);

    // 访问日志：控制台构建直接可见；GUI 构建可在 VS 输出窗口 / DebugView 中看到
    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        log_line("[http] " + req.method + " " + req.path + " -> " + std::to_string(res.status));
    });

    register_api(svr);

    // ---- 2. 前端资源：cpp-embedlib（挂到 "/"）----------------------------
    // 注意：API 路由要先注册，mount 作为兜底放在后面。
    httplib::mount(svr, Web::FS);

    const int requested_port = parse_port_argument(argc, argv);
    int port = 0;
    if (requested_port > 0) {
        if (!svr.bind_to_port("127.0.0.1", requested_port)) {
            fatal("端口 " + std::to_string(requested_port) + " 已被占用");
        }
        port = requested_port;
    } else {
        port = svr.bind_to_any_port("127.0.0.1");
    }
    if (port <= 0) { fatal("无法绑定本地端口"); }

    std::thread server_thread([&svr] { svr.listen_after_bind(); });

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    log_line("HTTP 服务已启动: " + url);

    // ---- 3. 原生窗口：webview（C++ API）---------------------------------
    // 对象放栈上，析构即销毁窗口 / 释放 WebView 内核（相当于 C API 的 webview_destroy）。
    // 构造与运行都可能抛 webview::exception，所以整段包在 try 里。
    try {
        webview::webview w(/* debug = */ true, /* parent window = */ nullptr);
        g_webview = &w;  // 回调里要通过它 resolve / terminate

        w.set_title("cpp-httplib + webview + cpp-embedlib + nlohmann/json Demo");
        w.set_size(1080, 780, WEBVIEW_HINT_NONE);

        // JS → C++ 的三个绑定（回调统一是 binding_t 签名）
        w.bind("cppNativeEcho", &on_native_echo, nullptr);
        w.bind("cppNativeHandle", &on_native_handle, nullptr);
        w.bind("cppCloseWindow", &on_close_window, nullptr);

        // 让窗口载入本机 HTTP 服务（同源，前端可直接 fetch("/api/...")）
        w.navigate(url);

        w.run();  // 阻塞：直到窗口关闭

        g_webview = nullptr;
    } catch (const webview::exception& e) {
        g_webview = nullptr;
        fatal(std::string("webview 初始化/运行失败: ") + e.what() + "（" + k_webview_hint + "）");
    }

    // ---- 4. 收尾 ---------------------------------------------------------
    svr.stop();
    if (server_thread.joinable()) { server_thread.join(); }

    log_line("已退出。");
    return 0;
}
