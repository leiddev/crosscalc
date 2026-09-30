// ---------------------------------------------------------------------------
//  黄金向量（JSON）驱动
//
//  同一批向量会被三方复用，保证 C++ 引擎、HTTP 接口、前端三者行为一致：
//    · C++ 单测   ：crosscalc_tests --vectors tests/vectors/*.json
//    · 端到端脚本 ：tests/e2e_api.py（打真实进程的 /api/eval）
//
//  向量文件格式（tests/vectors/*.json）：
//    {
//      "name": "standard",
//      "defaults": { "mode": "standard" },          // 可选，作为每条用例的默认值
//      "cases": [
//        { "expr": "2+3*4", "expect": "14" },                       // 比对展示字符串
//        { "expr": "1/3", "expect_value": 0.3333333333333333,      // 比对数值
//          "tolerance": 1e-15 },
//        { "expr": "1/0", "expect_error": true,
//          "error_contains": "除数不能为零" },
//        { "expr": "0xFFFFFFFF", "mode": "programmer", "base": "hex",
//          "expect": "FFFFFFFF" },
//        { "expr": "0xFFFFFFFFFFFFFFFF", "requires_64bit": true,
//          "expect": "FFFFFFFFFFFFFFFF" },                          // 平台不支持时跳过
//        { "expr": "FF+1", "mode": "programmer", "base": "hex",
//          "expect_error": true, "note": "十六进制字面量提示" }
//      ]
//    }
// ---------------------------------------------------------------------------
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/calc_engine.h"
#include "test_util.h"

namespace {

using nlohmann::json;

std::string get_string(const json& obj, const char* key, const std::string& def) {
    if (obj.contains(key) && obj[key].is_string()) {
        return obj[key].get<std::string>();
    }
    return def;
}

}  // namespace

int run_vector_files(const std::vector<std::string>& files) {
    int total = 0;
    int skipped = 0;
    const crosscalc::Engine engine;

    for (const std::string& path : files) {
        std::ifstream in(path);
        if (!in) {
            std::printf("  [失败] 无法打开向量文件: %s\n", path.c_str());
            ++xtest::g_failures;
            continue;
        }

        json doc;
        try {
            in >> doc;
        } catch (const std::exception& e) {
            std::printf("  [失败] 向量文件 JSON 解析错误: %s\n         %s\n",
                        path.c_str(), e.what());
            ++xtest::g_failures;
            continue;
        }

        const std::string file_name = get_string(doc, "name", path);
        // 文件级默认值
        std::string def_mode = "standard";
        std::string def_base = "dec";
        std::string def_wordsize = "32";
        if (doc.contains("defaults")) {
            const auto& d = doc["defaults"];
            def_mode = get_string(d, "mode", def_mode);
            def_base = get_string(d, "base", def_base);
            def_wordsize = get_string(d, "wordsize", def_wordsize);
        }

        std::printf("\n=== 向量文件: %s (%s) ===\n", file_name.c_str(),
                    path.c_str());

        if (!doc.contains("cases") || !doc["cases"].is_array()) {
            std::printf("  [失败] 缺少 cases 数组\n");
            ++xtest::g_failures;
            continue;
        }

        int index = 0;
        for (const auto& c : doc["cases"]) {
            ++index;
            const std::string expr = get_string(c, "expr", "");
            const std::string label =
                "#" + std::to_string(index) + " " + expr;

            if (!c.contains("expr") || !c["expr"].is_string()) {
                std::printf("  [失败] %s: 缺少 expr\n", label.c_str());
                ++xtest::g_failures;
                continue;
            }

            // 平台能力门控 ①：需要 64 位精确整数的用例，在不支持的平台上跳过
            const bool requires_64bit =
                c.value("requires_64bit", false);
            if (requires_64bit && !crosscalc::Engine::supports_64bit()) {
                ++skipped;
                std::printf("  [跳过] %s （本平台不支持 64 位精确整数）\n",
                            label.c_str());
                continue;
            }

            // 平台能力门控 ②：arith 指定"这条用例只在哪种运算精度下成立"。
            //
            //   "double"   = 引擎内部用 double 运算（Windows/MSVC，53 位尾数）
            //   "extended" = 引擎内部用 80 位扩展精度（x86-64 的 Linux/macOS）
            //
            // 为什么需要它：运算精度不同会让【显示结果】真的不一样，而这并不是 bug。
            // 最典型的是 0.1+0.2：
            //   * double 下是 0.30000000000000004（二进制无法精确表示 0.1/0.2）；
            //   * 80 位扩展精度下，两个加数的表示误差在更高精度里部分抵消，
            //     舍入回 17 位有效数字反而正好是 0.3。
            // 还有两个方向相反的溢出用例：1e999 超出 double 但没超出 long double，
            // 1e9999 则两者都超出。
            // 遇到这类用例就必须写明平台，否则在另一个平台上必然误报失败。
            const std::string arith = get_string(c, "arith", "");
            if (!arith.empty()) {
                const bool wide = crosscalc::Engine::supports_64bit();
                const bool want_wide = (arith == "extended");
                const bool want_narrow = (arith == "double");
                if (!want_wide && !want_narrow) {
                    std::printf("  [失败] %s: arith 只能是 \"double\" 或 \"extended\"\n",
                                label.c_str());
                    ++xtest::g_failures;
                    continue;
                }
                if ((want_wide && !wide) || (want_narrow && wide)) {
                    ++skipped;
                    std::printf("  [跳过] %s （只适用于 %s 运算精度的平台，本平台是 %s）\n",
                                label.c_str(), arith.c_str(),
                                wide ? "extended" : "double");
                    continue;
                }
            }

            crosscalc::EvalRequest req;
            req.expression = expr;
            crosscalc::parse_mode(get_string(c, "mode", def_mode), req.mode);
            crosscalc::parse_base(get_string(c, "base", def_base), req.base);
            crosscalc::parse_word_size(get_string(c, "wordsize", def_wordsize),
                                       req.word_size);

            ++total;
            xtest::g_current_case = label;
            ++xtest::g_cases_run;

            const crosscalc::EvalResult r = engine.evaluate(req);
            const bool expect_error = c.value("expect_error", false);
            const int before = xtest::g_failures;

            if (expect_error) {
                xtest::check_false(r.ok, ("应当失败: " + label).c_str(),
                                   __FILE__, __LINE__);
                if (c.contains("error_contains")) {
                    const std::string needle =
                        c["error_contains"].get<std::string>();
                    ::xtest::check_contains(r.error, needle,
                                            ("错误消息 " + label).c_str(),
                                            needle.c_str(), __FILE__, __LINE__);
                }
            } else {
                xtest::check_true(r.ok, ("应当成功: " + label).c_str(), __FILE__,
                                  __LINE__);
                if (!r.ok) {
                    std::printf("         实际错误: %s\n", r.error.c_str());
                }
                if (c.contains("expect")) {
                    const std::string want = c["expect"].get<std::string>();
                    ::xtest::check_eq(r.display, want,
                                      (label + " 展示结果").c_str(),
                                      want.c_str(), __FILE__, __LINE__);
                }
                if (c.contains("expect_value")) {
                    const double want = c["expect_value"].get<double>();
                    const double tol = c.value("tolerance", 1e-9);
                    ::xtest::check_near(r.value, want, tol,
                                        (label + " 数值").c_str(), "", __FILE__,
                                        __LINE__);
                }
            }

            if (xtest::g_failures == before) {
                std::printf("  [通过] %s\n", label.c_str());
            }
        }
    }

    std::printf("\n----------------------------------------------------------\n");
    std::printf("  向量用例 %d 条（跳过 %d 条），累计断言 %d 条，失败 %d 条\n",
                total, skipped, xtest::g_checks, xtest::g_failures);
    std::printf("----------------------------------------------------------\n");
    if (xtest::g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::printf("\nTESTS FAILED\n");
    return 1;
}
