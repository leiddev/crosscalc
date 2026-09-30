// ---------------------------------------------------------------------------
//  crosscalc 测试程序入口
//
//  这个可执行文件同时承担两个角色：
//    1. CTest 用例（无参数运行时执行全部断言）
//    2. 命令行工具，便于脚本与人工排查
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/calc_engine.h"
#include "test_util.h"

namespace xtest {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

int g_checks = 0;
int g_failures = 0;
int g_cases_run = 0;
std::string g_current_case;

Reg::Reg(const char* name, std::function<void()> fn) {
    registry().push_back({name, std::move(fn)});
}

void report_failure(const char* file, int line, const std::string& msg) {
    ++g_failures;
    std::printf("  [失败] %s\n", g_current_case.c_str());
    std::printf("         %s:%d\n", file, line);
    std::printf("         %s\n", msg.c_str());
}

int run_all(const std::string& filter) {
    for (auto& tc : registry()) {
        if (!filter.empty() && tc.name.find(filter) == std::string::npos) {
            continue;
        }
        g_current_case = tc.name;
        ++g_cases_run;
        const int before = g_failures;
        tc.fn();
        if (g_failures == before) {
            std::printf("  [通过] %s\n", tc.name.c_str());
        }
    }

    std::printf("\n----------------------------------------------------------\n");
    std::printf("  用例 %d 个，断言 %d 条，失败 %d 条\n", g_cases_run, g_checks,
                g_failures);
    std::printf("----------------------------------------------------------\n");
    if (g_failures == 0) {
        // 这一行是 CTest 的 PASS_REGULAR_EXPRESSION，改动请同步 CMakeLists.txt
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::printf("\nTESTS FAILED\n");
    return 1;
}

}  // namespace xtest

// 由 vector_runner.cpp 提供：跑一批 JSON 黄金向量文件
int run_vector_files(const std::vector<std::string>& files);

namespace {

void print_usage() {
    std::printf(
        "crosscalc_tests —— crosscalc 测试与求值工具\n"
        "\n"
        "用法:\n"
        "  crosscalc_tests                          运行全部内建用例\n"
        "  crosscalc_tests --filter 程序            只运行名称含关键字的用例\n"
        "  crosscalc_tests --vectors a.json b.json  运行黄金向量文件\n"
        "  crosscalc_tests --eval \"2+3*4\"           单条求值\n"
        "\n"
        "--eval 可选参数:\n"
        "  --mode standard|scientific|programmer   默认 standard\n"
        "  --base dec|hex|oct|bin                  默认 dec（程序员模式生效）\n"
        "  --wordsize 8|16|32|64                   默认 32（程序员模式生效）\n"
        "  --json                                  以 JSON 输出结果\n"
        "\n"
        "其它:\n"
        "  --platform-info                          打印平台整数精度信息\n"
        "  --list-functions                         打印引擎提供的全部函数\n"
        "  --help                                   显示本帮助\n");
}

void print_platform_info() {
    const auto info = crosscalc::Engine::platform_info();
    std::printf("运行平台信息\n");
    std::printf("  精确支持 64 位整数   : %s\n", info.supports_64bit ? "是" : "否");
    std::printf("  精确整数位数         : %d bit\n", info.max_integer_bitness);
    // %L 修饰符：num_t 是 long double，用 %f 是未定义行为
    std::printf("  最大精确整数         : %.0Lf\n", info.max_integer);
    std::printf("  按位运算最大数值     : %.0Lf (2^48−1)\n", info.max_bitops_value);
    std::printf("  引擎版本             : %s\n", CROSSCALC_VERSION);
}

/// 把结果里的特殊字符转义，避免破坏 JSON
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
        }
    }
    return out;
}

/// 单条求值并打印（成功返回 0，失败返回 1，便于脚本判断）
int eval_one(const crosscalc::EvalRequest& req, bool as_json) {
    const crosscalc::Engine engine;
    const crosscalc::EvalResult r = engine.evaluate(req);

    if (as_json) {
        std::printf("{\"ok\":%s,", r.ok ? "true" : "false");
        std::printf("\"mode\":\"%s\",", crosscalc::mode_name(req.mode));
        std::printf("\"expression\":\"%s\",", json_escape(req.expression).c_str());
        if (r.ok) {
            std::printf("\"display\":\"%s\",", json_escape(r.display).c_str());
            // 显式窄化成 double 输出：JSON 的数字就是 double 精度，
            // 精确结果看 display。%Lg 会把 long double 直接打印成 double 装不下的
            // 位数，反而是"看起来精确"的假象。
            std::printf("\"value\":%.17g,", static_cast<double>(r.value));
            std::printf("\"integral\":%s", r.integral ? "true" : "false");
        } else {
            std::printf("\"error\":\"%s\",", json_escape(r.error).c_str());
            std::printf("\"error_pos\":%d,", r.error_pos);
            std::printf("\"error_len\":%d", r.error_len);
        }
        std::printf("}\n");
    } else {
        if (r.ok) {
            std::printf("%s\n", r.display.c_str());
        } else {
            std::printf("错误: %s\n", r.error.c_str());
        }
    }
    return r.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> vector_files;
    std::string filter;
    std::string eval_expr;
    bool has_eval = false;
    bool as_json = false;
    crosscalc::EvalRequest req;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "缺少参数: %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--eval") {
            eval_expr = next("--eval");
            has_eval = true;
        } else if (arg == "--vectors") {
            // 后面所有非选项参数都是向量文件
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                vector_files.emplace_back(argv[++i]);
            }
        } else if (arg == "--filter") {
            filter = next("--filter");
        } else if (arg == "--mode") {
            const std::string v = next("--mode");
            if (!crosscalc::parse_mode(v, req.mode)) {
                std::fprintf(stderr, "无法识别的模式: %s\n", v.c_str());
                return 2;
            }
        } else if (arg == "--base") {
            const std::string v = next("--base");
            if (!crosscalc::parse_base(v, req.base)) {
                std::fprintf(stderr, "无法识别的进制: %s\n", v.c_str());
                return 2;
            }
        } else if (arg == "--wordsize") {
            const std::string v = next("--wordsize");
            if (!crosscalc::parse_word_size(v, req.word_size)) {
                std::fprintf(stderr, "无法识别的字长: %s\n", v.c_str());
                return 2;
            }
        } else if (arg == "--json") {
            as_json = true;
        } else if (arg == "--platform-info") {
            print_platform_info();
            return 0;
        } else if (arg == "--list-functions") {
            std::printf("%s\n", crosscalc::Engine::available_functions_text().c_str());
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "无法识别的参数: %s\n", arg.c_str());
            print_usage();
            return 2;
        }
    }

    if (has_eval) {
        req.expression = eval_expr;
        return eval_one(req, as_json);
    }

    if (!vector_files.empty()) {
        return run_vector_files(vector_files);
    }

    return xtest::run_all(filter);
}
