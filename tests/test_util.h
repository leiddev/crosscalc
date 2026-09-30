// ---------------------------------------------------------------------------
//  crosscalc 测试框架（极简，无外部依赖）
//
//  刻意不引入 doctest/Catch2：测试程序要在 Windows 与 Linux 上同时作为
//  普通 CLI 使用，零依赖能让 CI 更快、更不容易出意外。
//
//  用法：
//      TEST_CASE("分组.用例名") { CHECK(1 + 1 == 2); }
// ---------------------------------------------------------------------------
#pragma once

#include <cmath>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace xtest {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();

/// 全局统计
extern int g_checks;
extern int g_failures;
extern int g_cases_run;
extern std::string g_current_case;

struct Reg {
    Reg(const char* name, std::function<void()> fn);
};

void report_failure(const char* file, int line, const std::string& msg);

/// 运行全部（或名称包含 filter 的）用例，返回进程退出码。
int run_all(const std::string& filter);

// ---- 值的字符串化（失败信息用）--------------------------------------------
inline std::string to_str(const std::string& v) { return "\"" + v + "\""; }
inline std::string to_str(const char* v) { return std::string("\"") + (v ? v : "(null)") + "\""; }
inline std::string to_str(bool v) { return v ? "true" : "false"; }
inline std::string to_str(double v) {
    std::ostringstream os;
    os.precision(17);
    os << v;
    return os.str();
}
inline std::string to_str(float v) { return to_str(static_cast<double>(v)); }
template <class T>
std::string to_str(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

// ---- 断言 ------------------------------------------------------------------
template <class A, class B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb,
              const char* file, int line) {
    ++g_checks;
    if (!(a == b)) {
        report_failure(file, line,
                       std::string("CHECK_EQ(") + ea + " == " + eb + ")\n" +
                           "       实际值: " + to_str(a) + "\n" +
                           "       期望值: " + to_str(b));
    }
}

inline void check_true(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        report_failure(file, line, std::string("条件不成立: ") + expr);
    }
}

inline void check_false(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (cond) {
        report_failure(file, line, std::string("条件本应不成立: ") + expr);
    }
}

inline void check_near(double a, double b, double tol, const char* ea,
                       const char* eb, const char* file, int line) {
    ++g_checks;
    const double diff = std::fabs(a - b);
    if (!(diff <= tol)) {
        std::ostringstream os;
        os.precision(17);
        os << "CHECK_NEAR(" << ea << " ~= " << eb << ")\n"
           << "       实际值: " << a << "\n"
           << "       期望值: " << b << "\n"
           << "       误差  : " << diff << " (容差 " << tol << ")";
        report_failure(file, line, os.str());
    }
}

inline void check_contains(const std::string& haystack, const std::string& needle,
                           const char* eh, const char* en, const char* file,
                           int line) {
    ++g_checks;
    if (haystack.find(needle) == std::string::npos) {
        report_failure(file, line,
                       std::string("CHECK_CONTAINS(") + eh + ", " + en + ")\n" +
                           "       文本: " + to_str(haystack) + "\n" +
                           "       未包含: " + to_str(needle));
    }
}

}  // namespace xtest

// ---------------------------------------------------------------------------
//  宏
// ---------------------------------------------------------------------------
#define XTEST_CAT_(a, b) a##b
#define XTEST_CAT(a, b) XTEST_CAT_(a, b)

#define TEST_CASE(name)                                                     \
    static void XTEST_CAT(xtest_fn_, __LINE__)();                           \
    static ::xtest::Reg XTEST_CAT(xtest_reg_, __LINE__)(                    \
        name, XTEST_CAT(xtest_fn_, __LINE__));                              \
    static void XTEST_CAT(xtest_fn_, __LINE__)()

#define CHECK(cond)                                                         \
    ::xtest::check_true(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

#define CHECK_FALSE(cond)                                                   \
    ::xtest::check_false(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

#define CHECK_EQ(a, b) ::xtest::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_NEAR(a, b, tol)                                               \
    ::xtest::check_near((a), (b), (tol), #a, #b, __FILE__, __LINE__)

#define CHECK_CONTAINS(hay, needle)                                         \
    ::xtest::check_contains((hay), (needle), #hay, #needle, __FILE__,       \
                            __LINE__)
