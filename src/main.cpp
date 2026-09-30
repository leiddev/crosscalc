// ---------------------------------------------------------------------------
//  crosscalc —— 跨平台计算器主程序
//
//  运行结构：
//
//     [ webview 原生窗口 ]                          [ 后台线程 ]
//       │  WebView2 / WebKitGTK 载入                  │
//       │  http://127.0.0.1:<port>                    │
//       ├──────────────► GET /             ────────►  cpp-embedlib 内嵌的 www/ 资源
//       │                GET/POST /api/*   ────────►  cpp-httplib 处理
//       └── webview::bind("cppEvaluate") ──────────► 直接调用 C++（不经 HTTP）
//
//  计算全部由 crosscalc_core 完成，而 crosscalc_core 只驱动
//  tinyexpr-plusplus —— 本程序自身不实现任何数学运算。
//
//  三种模式（标准 / 科学 / 程序员）共用同一个 /api/eval 入口，
//  靠 mode / base / wordsize 三个参数区分，具体语义见 src/core/calc_engine.h。
//
//  命令行：
//     crosscalc                正常启动（带窗口）
//     crosscalc --port 18080   指定端口（0 或缺省 = 自动挑选空闲端口）
//     crosscalc --headless     不开窗口，只跑 HTTP 服务（CI / 端到端测试用）
//     crosscalc --debug        打开 webview 的开发者工具（右键检查元素）
// ---------------------------------------------------------------------------
#include "WebAssets.h"  // cpp-embedlib 生成的：Web::FS

#include <cpp-embedlib-httplib.h>  // httplib::mount(svr, Web::FS)
#include <httplib.h>               // cpp-httplib

#include <webview/webview.h>  // webview 0.12 的 C++ API（header-only）

#include <nlohmann/json.hpp>  // JSON 序列化 / 解析（header-only）

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "core/calc_engine.h"
#include "core/calc_types.h"
#include "core/number_format.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>  // getpid()
#endif

namespace {

// ------------------------------------------------------------------ JSON ----
// 用 ordered_json 而不是 nlohmann::json：后者的底层是 std::map，键会按字母序输出，
// ordered_json 保留我们插入的顺序，响应体读起来更符合直觉。
using json = nlohmann::ordered_json;

// 统一的序列化出口。
// error_handler_t::replace：源字符串里若混进非法 UTF-8（比如 ?q=%FF），
// 默认的 strict 会抛 type_error.316 被 cpp-httplib 兜成 500；
// replace 则把坏字节换成 U+FFFD，照样输出合法 JSON。
std::string json_text(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// ------------------------------------------------------- 平台差异的小常量 ----
#if defined(_WIN32)
constexpr const char* k_os_name = "windows";
constexpr const char* k_webview_hint = "请确认已安装 WebView2 运行时";
#elif defined(__APPLE__)
constexpr const char* k_os_name = "macos";
constexpr const char* k_webview_hint = "请确认系统提供了 WebKit（macOS 自带）";
#else
constexpr const char* k_os_name = "linux";
constexpr const char* k_webview_hint =
    "请确认已安装 WebKitGTK 运行库（如 libwebkit2gtk-4.1-0），且当前有 X11/Wayland 显示";
#endif

// --------------------------------------------------------------- 全局状态 --
const auto g_started_at = std::chrono::steady_clock::now();
webview::webview* g_webview = nullptr;  // webview::bind 回调里要用它回传结果
const crosscalc::Engine g_engine;       // 引擎无状态，全局一份即可

// ---------------------------------------------------------------- 小工具 ----
#ifdef _WIN32
// Win32 的 *W 接口要的是 UTF-16，而项目里传给它们的都是 UTF-8 字节串
// （MSVC 那边我们加了 /utf-8，GCC 本来就是），所以必须真正转一次码。
//
// 千万不要写成 std::wstring(msg.begin(), msg.end())：那是把每个"字节"当成一个
// wchar_t 塞进去，一个汉字（UTF-8 3 字节）会变成 3 个乱码字符。
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
    OutputDebugStringW(to_wstring_utf8(line).c_str());
#endif
}

[[noreturn]] void fatal(const std::string& msg) {
    log_line("[fatal] " + msg);
#ifdef _WIN32
    // 只有带窗口的模式才弹对话框；--headless 下 CI 没人点确定，弹窗会卡住
    if (GetConsoleWindow() == nullptr && GetStdHandle(STD_ERROR_HANDLE) == nullptr) {
        MessageBoxW(nullptr, to_wstring_utf8(msg).c_str(), L"crosscalc",
                    MB_ICONERROR | MB_OK);
    }
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
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_started_at)
        .count();
}

long current_process_id() {
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}

void reply_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    // 所有 API 回复都是"此刻的事实"（/api/info 里的运行时长就是活的），
    // 一律不许缓存：否则前端"重新取一次"可能拿到浏览器缓存里的旧值。
    res.set_header("Cache-Control", "no-store");
    res.set_content(json_text(body), "application/json; charset=utf-8");
}

// webview::bind 会把 JS 调用时的所有实参打包成一个 JSON 数组字符串传进来。
// 前端既可以传对象（cppEvaluate({...})）也可以传 JSON 字符串，两种都接受。
json extract_request_object(const std::string& args) {
    json parsed = json::parse(args, nullptr, /* allow_exceptions = */ false);
    if (parsed.is_array()) {
        if (parsed.empty()) {
            return json::object();
        }
        if (parsed.front().is_object()) {
            return parsed.front();
        }
        if (parsed.front().is_string()) {
            return json::parse(parsed.front().get<std::string>(), nullptr,
                               /* allow_exceptions = */ false);
        }
        return json::object();
    }
    return parsed;  // 理论上到不了这里，留个兜底
}

// ------------------------------------------------- 请求参数 -> EvalRequest ----
// 三种模式共用。任何无法识别的取值都会返回 false 并给出中文说明，
// 而不是悄悄退回默认值 —— 否则前端写错参数时很难排查。
bool build_request(const json& in, crosscalc::EvalRequest& req,
                   std::string& error) {
    if (!in.contains("expression") || !in["expression"].is_string()) {
        error = "缺少 expression 字段（应为字符串）";
        return false;
    }
    req.expression = in["expression"].get<std::string>();

    if (in.contains("mode") && in["mode"].is_string()) {
        if (!crosscalc::parse_mode(in["mode"].get<std::string>(), req.mode)) {
            error = "无法识别的 mode：应为 standard / scientific / programmer";
            return false;
        }
    }
    if (in.contains("base") && in["base"].is_string()) {
        if (!crosscalc::parse_base(in["base"].get<std::string>(), req.base)) {
            error = "无法识别的 base：应为 dec / hex / oct / bin";
            return false;
        }
    }
    if (in.contains("wordsize")) {
        std::string ws;
        if (in["wordsize"].is_string()) {
            ws = in["wordsize"].get<std::string>();
        } else if (in["wordsize"].is_number_integer()) {
            ws = std::to_string(in["wordsize"].get<long long>());
        } else {
            error = "无法识别的 wordsize：应为 8 / 16 / 32 / 64";
            return false;
        }
        if (!crosscalc::parse_word_size(ws, req.word_size)) {
            error = "无法识别的 wordsize：应为 8 / 16 / 32 / 64";
            return false;
        }
    }
    return true;
}

// 程序员模式：把同一个结果显示成 4 种进制。
// 每一种都再走一次引擎 —— 进制渲染完全由 crosscalc_core 负责，
// 前端不会自己算位模式（那样就等于把展示逻辑复制到 JS 里，容易两边不一致）。
json all_bases_json(const crosscalc::EvalRequest& req) {
    static const crosscalc::NumBase kBases[] = {
        crosscalc::NumBase::Hex, crosscalc::NumBase::Dec,
        crosscalc::NumBase::Oct, crosscalc::NumBase::Bin};

    json out = json::object();
    for (const crosscalc::NumBase b : kBases) {
        crosscalc::EvalRequest r = req;
        r.base = b;
        const crosscalc::EvalResult res = g_engine.evaluate(r);
        out[crosscalc::base_name(b)] = res.ok ? json(res.display) : json(nullptr);
    }
    return out;
}

// EvalResult -> JSON（HTTP 与 webview::bind 两条路径共用同一个形状）
json result_to_json(const crosscalc::EvalRequest& req,
                    const crosscalc::EvalResult& r) {
    json out;
    out["ok"] = r.ok;
    out["mode"] = crosscalc::mode_name(req.mode);
    out["base"] = crosscalc::base_name(req.base);
    out["wordsize"] = static_cast<int>(req.word_size);
    out["expression"] = req.expression;
    if (r.ok) {
        out["display"] = r.display;
        // JSON 的数字类型装不下 64 位整数，这里显式窄化成 double。
        // 需要精确值时看 display（十六进制/二进制位模式那一串），
        // 这也是前端在程序员模式下显示结果的字段。
        out["value"] = static_cast<double>(r.value);
        out["integral"] = r.integral;
        out["error"] = nullptr;
        out["error_pos"] = -1;
        out["error_len"] = 0;
        out["incomplete"] = false;
        // 程序员模式额外给出四种进制的显示，供前端的进制面板使用
        if (req.mode == crosscalc::Mode::Programmer) {
            out["all_bases"] = all_bases_json(req);
        }
    } else {
        out["display"] = nullptr;
        out["value"] = nullptr;
        out["integral"] = false;
        out["error"] = r.error;
        out["error_pos"] = r.error_pos;
        out["error_len"] = r.error_len;
        // true 表示这只是"表达式还没输完"，前端实时预览时先不要弹错误
        out["incomplete"] = r.incomplete;
    }
    return out;
}

// -------------------------------------------------- 前端页面用的元信息 JSON --
json platform_json() {
    const auto info = crosscalc::Engine::platform_info();
    return json{
        {"supports_64bit", info.supports_64bit},
        {"max_integer_bitness", info.max_integer_bitness},
        // 同 result_to_json：JSON 数字是 double 精度，这里只是给前端做展示，
        // 是否启用 QWORD 一律以 supports_64bit 为准。
        {"max_integer", static_cast<double>(info.max_integer)},
        {"max_bitops_value", static_cast<double>(info.max_bitops_value)},
        {"os", k_os_name},
    };
}

json info_json() {
    return json{
        {"name", "crosscalc"},
        {"version", CROSSCALC_VERSION},
        {"os", k_os_name},
        {"pid", current_process_id()},
        {"uptime_ms", uptime_ms()},
        {"time", local_time_string()},
        {"cpp-httplib", CPPHTTPLIB_VERSION},
        {"webview", WEBVIEW_VERSION_NUMBER},
        {"cpp-embedlib", "main"},
        {"nlohmann/json",
         std::to_string(NLOHMANN_JSON_VERSION_MAJOR) + "." +
             std::to_string(NLOHMANN_JSON_VERSION_MINOR) + "." +
             std::to_string(NLOHMANN_JSON_VERSION_PATCH)},
        {"tinyexpr-plusplus", "404688cf"},
        {"platform", platform_json()},
    };
}

// -------------------------------------------------- webview::bind 回调（C++）--
// JS: cppEvaluate(jsonString) —— 与 POST /api/eval 等价，但不走 HTTP。
// 走 bind 的好处是不受 HTTP 超时/端口影响，对"每次按键都求值"的实时预览更稳。
void on_evaluate(std::string id, std::string req, void*) {
    crosscalc::EvalRequest eval_req;
    std::string error;
    json out;

    const json parsed = extract_request_object(req);
    if (!parsed.is_object() || parsed.empty()) {
        out = json{{"ok", false},
                   {"error", "cppEvaluate 需要一个 JSON 对象参数"},
                   {"error_pos", -1},
                   {"error_len", 0}};
    } else if (!build_request(parsed, eval_req, error)) {
        out = json{{"ok", false},
                   {"error", error},
                   {"error_pos", -1},
                   {"error_len", 0}};
    } else {
        out = result_to_json(eval_req, g_engine.evaluate(eval_req));
    }

    if (g_webview != nullptr) {
        g_webview->resolve(id, 0, json_text(out));
    }
}

// JS: cppCloseWindow() —— 关闭窗口（结束 webview::run() 的消息循环）
void on_close_window(std::string id, std::string, void*) {
    g_webview->resolve(id, 0, json_text(json("closing...")));
    g_webview->terminate();
}

// ------------------------------------------------------------------ 路由 ----
void register_api(httplib::Server& svr) {
    // ---- POST /api/eval ---------------------------------------------------
    // 请求体: {"expression":"2+3*4","mode":"standard","base":"dec","wordsize":"32"}
    svr.Post("/api/eval", [](const httplib::Request& req, httplib::Response& res) {
        const json parsed =
            json::parse(req.body, nullptr, /* allow_exceptions = */ false);
        if (!parsed.is_object()) {
            reply_json(res, json{{"ok", false},
                                 {"error", "请求体必须是 JSON 对象"},
                                 {"error_pos", -1},
                                 {"error_len", 0}},
                       400);
            return;
        }

        crosscalc::EvalRequest eval_req;
        std::string error;
        if (!build_request(parsed, eval_req, error)) {
            reply_json(res, json{{"ok", false},
                                 {"error", error},
                                 {"error_pos", -1},
                                 {"error_len", 0}},
                       400);
            return;
        }
        // 注意：表达式本身算错【不】是 HTTP 错误，仍然是 200 + ok:false，
        // 这样前端只需要看 ok 字段，不用区分网络错误与计算错误。
        reply_json(res, result_to_json(eval_req, g_engine.evaluate(eval_req)));
    });

    // ---- GET /api/eval?expr=...&mode=... ---------------------------------
    // 方便 curl / 脚本 / 浏览器直接验证
    svr.Get("/api/eval", [](const httplib::Request& req, httplib::Response& res) {
        crosscalc::EvalRequest eval_req;
        eval_req.expression = req.get_param_value("expr");

        std::string error;
        if (req.has_param("mode") &&
            !crosscalc::parse_mode(req.get_param_value("mode"), eval_req.mode)) {
            error = "无法识别的 mode：应为 standard / scientific / programmer";
        }
        if (error.empty() && req.has_param("base") &&
            !crosscalc::parse_base(req.get_param_value("base"), eval_req.base)) {
            error = "无法识别的 base：应为 dec / hex / oct / bin";
        }
        if (error.empty() && req.has_param("wordsize") &&
            !crosscalc::parse_word_size(req.get_param_value("wordsize"),
                                        eval_req.word_size)) {
            error = "无法识别的 wordsize：应为 8 / 16 / 32 / 64";
        }
        if (!error.empty()) {
            reply_json(res, json{{"ok", false},
                                 {"error", error},
                                 {"error_pos", -1},
                                 {"error_len", 0}},
                       400);
            return;
        }
        reply_json(res, result_to_json(eval_req, g_engine.evaluate(eval_req)));
    });

    // ---- GET /api/info ----------------------------------------------------
    svr.Get("/api/info", [](const httplib::Request&, httplib::Response& res) {
        reply_json(res, info_json());
    });

    // ---- GET /api/platform —— 前端据此禁用 QWORD 等按钮并显示原因 ---------
    svr.Get("/api/platform", [](const httplib::Request&, httplib::Response& res) {
        reply_json(res, platform_json());
    });

    // ---- GET /api/functions —— 引擎真实提供的全部函数名 ------------------
    // 内容直接来自 tinyexpr++ 的 list_available_functions_and_variables()，
    // 前端"函数参考"面板用它，免得手写一份会和引擎不同步的清单。
    svr.Get("/api/functions", [](const httplib::Request&, httplib::Response& res) {
        reply_json(res, json{{"text", crosscalc::Engine::available_functions_text()}});
    });

    // ---- GET /api/assets —— 列出被 cpp-embedlib 内嵌的资源 ----------------
    svr.Get("/api/assets", [](const httplib::Request&, httplib::Response& res) {
        json files = json::array();
        for (auto it = Web::FS.begin(); it != Web::FS.end(); ++it) {
            const auto entry = *it;
            if (entry.is_dir()) {
                continue;
            }
            const auto bytes = entry.bytes();
            files.push_back(json{{"path", entry.path()},
                                 {"mime", entry.mime_type()},
                                 {"bytes", bytes ? bytes->size() : 0}});
        }
        reply_json(res, json{{"count", files.size()}, {"files", files}});
    });

    // ---- GET /api/health —— 供 CI / 脚本做就绪探测 ------------------------
    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        reply_json(res, json{{"status", "ok"}, {"version", CROSSCALC_VERSION}});
    });
}

// ------------------------------------------------------------------ 参数 ----
struct Options {
    int port = 0;          // 0 = 自动挑选空闲端口
    bool headless = false; // 不开窗口，只跑 HTTP
    bool debug = false;    // 打开开发者工具
};

Options parse_arguments(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            opt.port = std::atoi(argv[++i]);
        } else if (arg == "--headless" || arg == "--no-gui") {
            opt.headless = true;
        } else if (arg == "--debug") {
            opt.debug = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "crosscalc —— 跨平台计算器（标准 / 科学 / 程序员）\n"
                "\n"
                "用法: crosscalc [选项]\n"
                "  --port <n>   指定 HTTP 服务端口（默认自动挑选空闲端口）\n"
                "  --headless   只启动 HTTP 服务，不打开窗口（CI / 脚本用）\n"
                "  --debug      打开 webview 开发者工具\n"
                "  --help       显示本帮助\n");
            std::exit(0);
        }
    }
    return opt;
}

}  // namespace

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    const Options opt = parse_arguments(argc, argv);

    // ---- 1. HTTP 服务器：cpp-httplib ------------------------------------
    httplib::Server svr;
    svr.set_read_timeout(5, 0);
    svr.set_payload_max_length(1024 * 1024);

    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        log_line("[http] " + req.method + " " + req.path + " -> " +
                 std::to_string(res.status));
    });

    register_api(svr);

    // ---- 2. 前端资源：cpp-embedlib（挂到 "/"）----------------------------
    // 注意：API 路由要先注册，mount 作为兜底放在后面。
    httplib::mount(svr, Web::FS);

    int port = 0;
    if (opt.port > 0) {
        if (!svr.bind_to_port("127.0.0.1", opt.port)) {
            fatal("端口 " + std::to_string(opt.port) + " 已被占用");
        }
        port = opt.port;
    } else {
        port = svr.bind_to_any_port("127.0.0.1");
    }
    if (port <= 0) {
        fatal("无法绑定本地端口");
    }

    std::thread server_thread([&svr] { svr.listen_after_bind(); });

    const std::string url =
        "http://127.0.0.1:" + std::to_string(port) + "/";
    log_line("crosscalc " + std::string(CROSSCALC_VERSION) +
             " 已启动，HTTP 服务: " + url);
    // 这一行是给脚本/CI 解析用的固定格式，改动请同步 tests/e2e_api.py
    log_line("LISTENING " + url);

    const auto platform = crosscalc::Engine::platform_info();
    // 这里只报"能不能精确表示 64 位整数"，不报最大值本身：
    // 最大精确整数在支持 64 位的平台上是 2^64−1，而把它打印出来必然要经过
    // 十进制格式化，17 位有效数字下会显示成 2^64（18446744073709551616），
    // 看起来像是差一，容易误导。位数（bitness）才是这个能力的关键指标。
    log_line(std::string("平台整数精度: ") +
             (platform.supports_64bit ? "支持 64 位精确整数（QWORD 可用）"
                                      : "仅支持 53 位精确整数（QWORD 不可用）") +
             "，精确整数位数 " + std::to_string(platform.max_integer_bitness) + " bit");

    // ---- 3. headless：不开窗口，等待 Ctrl+C / 被 kill --------------------
    if (opt.headless) {
        log_line("以 --headless 模式运行，不打开窗口。按 Ctrl+C 退出。");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::hours(24));
        }
    }

    // ---- 4. 原生窗口：webview（C++ API）---------------------------------
    try {
        webview::webview w(/* debug = */ opt.debug, /* parent window = */ nullptr);
        g_webview = &w;

        w.set_title("crosscalc —— 计算器（标准 / 科学 / 程序员）");
        // 初始尺寸刻意开小一点，别一上来就占满屏。
        // 这个高度是有下限的：set_size 给的宽高就是页面的视口（客户区），
        // 程序员模式的内容下限是 631px（四行进制面板 + 6 行 40px 的键盘），
        // 而 .calc 拿到的高度 ≈ 窗口高 − 83，所以窗口高至少要 714px，
        // 再矮程序员模式就会在 .calc 里长出滚动条（键会被裁）。
        // 1024x720 比下限高 7px：三种模式都刚好铺满、零裁切。
        // 详细账见 docs/design-decisions.md §15 / §19。
        w.set_size(1024, 720, WEBVIEW_HINT_NONE);
        // 允许窗口放得很小（视口低于约 900 宽会切成"记录区放下面"的单列布局）
        w.set_size(760, 560, WEBVIEW_HINT_MIN);

        // JS → C++ 的绑定
        w.bind("cppEvaluate", &on_evaluate, nullptr);
        w.bind("cppCloseWindow", &on_close_window, nullptr);

        // 让窗口载入本机 HTTP 服务（同源，前端可直接 fetch("/api/...")）
        w.navigate(url);

        w.run();  // 阻塞：直到窗口关闭

        g_webview = nullptr;
    } catch (const webview::exception& e) {
        g_webview = nullptr;
        fatal(std::string("webview 初始化/运行失败: ") + e.what() + "（" +
              k_webview_hint + "）");
    }

    // ---- 5. 收尾 ---------------------------------------------------------
    svr.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    log_line("已退出。");
    return 0;
}
