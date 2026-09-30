// ---------------------------------------------------------------------------
//  crosscalc 内建单元测试
//
//  约定：
//    · 只通过 crosscalc::Engine 访问计算能力 —— 与前端走的是同一条路径；
//    · 期望值尽量用 CHECK_EQ(display, "...") 做【精确字符串】比对，
//      这样"结果算对了但显示错了"也能被抓到；
//    · 浮点近似值用 CHECK_NEAR + 显式容差；
//    · 与平台能力相关的用例显式判断 Engine::supports_64bit()。
//
//  运行：crosscalc_tests            （或 ctest）
// ---------------------------------------------------------------------------
#include <cmath>
#include <limits>
#include <string>

#include "core/calc_engine.h"
#include "core/number_format.h"
#include "test_util.h"

using crosscalc::EvalRequest;
using crosscalc::EvalResult;
using crosscalc::Mode;
using crosscalc::NumBase;
using crosscalc::WordSize;

namespace {

/// 求值快捷方式
EvalResult ev(const std::string& expr, Mode mode = Mode::Standard,
              NumBase base = NumBase::Dec,
              WordSize ws = WordSize::Bits32) {
    EvalRequest req;
    req.expression = expr;
    req.mode = mode;
    req.base = base;
    req.word_size = ws;
    return crosscalc::Engine().evaluate(req);
}

/// 断言表达式成功且展示字符串等于 expect
void expect_display(const std::string& expr, const std::string& want,
                    Mode mode = Mode::Standard, NumBase base = NumBase::Dec,
                    WordSize ws = WordSize::Bits32) {
    const EvalResult r = ev(expr, mode, base, ws);
    if (!r.ok) {
        ::xtest::report_failure(__FILE__, __LINE__,
                                "表达式 \"" + expr + "\" 本应成功，实际报错: " +
                                    r.error);
        return;
    }
    ::xtest::check_eq(r.display, want, expr.c_str(), want.c_str(), __FILE__,
                      __LINE__);
}

/// 断言表达式失败，且错误消息包含指定文字
void expect_error(const std::string& expr, const std::string& needle,
                  Mode mode = Mode::Standard, NumBase base = NumBase::Dec,
                  WordSize ws = WordSize::Bits32) {
    const EvalResult r = ev(expr, mode, base, ws);
    if (r.ok) {
        ::xtest::report_failure(__FILE__, __LINE__,
                                "表达式 \"" + expr + "\" 本应失败，实际结果: " +
                                    r.display);
        return;
    }
    if (!needle.empty()) {
        ::xtest::check_contains(r.error, needle, expr.c_str(), needle.c_str(),
                                __FILE__, __LINE__);
    }
}

}  // namespace

// ===========================================================================
//  标准计算器模式
// ===========================================================================

TEST_CASE("标准模式.四则运算") {
    expect_display("2+3", "5");
    expect_display("2+3*4", "14");
    expect_display("(2+3)*4", "20");
    expect_display("10-4", "6");
    expect_display("10/4", "2.5");
    expect_display("-5+10", "5");
    expect_display("1/3", "0.3333333333333333");
    expect_display("2*3.5", "7");
}

TEST_CASE("标准模式.乘方与取模") {
    // 默认编译下 `^` 是乘方（这是本项目的既定设计，见 docs/design-decisions.md）
    expect_display("2^10", "1024");
    expect_display("2**10", "1024");   // ** 是 ^ 的同义写法
    expect_display("2^0.5", "1.4142135623730951");
    expect_display("9^0.5", "3");
    // 取模用 MOD() 函数（库里的 % 也是取模，但为可读性统一用 MOD）
    expect_display("mod(10,3)", "1");
    expect_display("10%3", "1");
}

TEST_CASE("标准模式.内建函数") {
    expect_display("abs(-5)", "5");
    expect_display("trunc(3.7)", "3");
    expect_display("round(3.5)", "4");
    expect_display("floor(3.9)", "3");
    expect_display("ceil(3.1)", "4");
    expect_display("sign(-9)", "-1");
    expect_display("min(3,1,2)", "1");
    expect_display("max(3,1,2)", "3");
    expect_display("sum(1,2,3)", "6");
    expect_display("average(1,2,3)", "2");
    expect_display("clamp(5,1,3)", "3");
    expect_display("sqr(4)", "16");
    expect_display("sqrt(16)", "4");
}

TEST_CASE("标准模式.比较与条件") {
    expect_display("if(1,2,3)", "2");
    expect_display("if(0,2,3)", "3");
    expect_display("1&&1", "1");
    expect_display("1||0", "1");
    expect_display("not(1)", "0");
    expect_display("isodd(3)", "1");
    expect_display("iseven(3)", "0");
}

TEST_CASE("标准模式.数值显示格式") {
    // 显示采用"最短且能唯一还原该 double"的精度：
    // 0.1+0.2 的真实结果是 0.30000000000000004，如实显示而不是抹成 0.3，
    // 避免让用户以为得到的是一个不精确的结果。
    expect_display("0.1+0.2", "0.30000000000000004");
    expect_display("1.5+1.5", "3");
    expect_display("0-0", "0");
    expect_display("1000000*1000000", "1000000000000");
    expect_display("1/0.0000001", "10000000");
}

// ===========================================================================
//  科学计算器模式
// ===========================================================================

TEST_CASE("科学模式.三角与反三角") {
    expect_display("sin(0)", "0");
    expect_display("cos(0)", "1");
    expect_display("tan(0)", "0");
    expect_display("sin(pi/2)", "1");
    expect_display("cos(pi)", "-1");
    expect_display("cot(pi/4)", "1.0000000000000002");
    expect_display("asin(0)", "0");
    expect_display("atan2(1,1)", "0.7853981633974483");
    expect_display("sin(pi/6)", "0.49999999999999994");
}

TEST_CASE("科学模式.对数与指数") {
    expect_display("ln(e)", "1");
    expect_display("ln(1)", "0");
    expect_display("log10(1000)", "3");
    expect_display("exp(0)", "1");
    expect_display("exp(1)", "2.718281828459045");
    expect_display("pow(2,10)", "1024");
    expect_display("power(2,10)", "1024");
}

TEST_CASE("科学模式.阶乘与排列组合") {
    expect_display("fac(5)", "120");
    expect_display("fact(5)", "120");
    expect_display("fac(0)", "1");
    expect_display("combin(5,2)", "10");
    expect_display("ncr(5,2)", "10");
    expect_display("permut(5,2)", "20");
    expect_display("npr(5,2)", "20");
    expect_display("tgamma(5)", "24");
}

TEST_CASE("科学模式.常量") {
    expect_display("pi", "3.141592653589793");
    expect_display("e", "2.718281828459045");
    expect_display("true", "1");
    expect_display("false", "0");
}

TEST_CASE("科学模式.双曲函数") {
    expect_display("sinh(0)", "0");
    expect_display("cosh(0)", "1");
    expect_display("tanh(0)", "0");
}

// ===========================================================================
//  程序员计算器模式
// ===========================================================================

TEST_CASE("程序员模式.按位函数") {
    expect_display("bitand(12,10)", "8");
    expect_display("bitor(12,10)", "14");
    expect_display("bitxor(12,10)", "6");
    expect_display("bitlshift(1,10)", "1024");
    expect_display("bitrshift(1024,10)", "1");
    expect_display("bitand(255,15)", "15");
    // 带字长后缀的按位取反：结果按该字长截断
    expect_display("bitnot8(0)", "255");
    expect_display("bitnot16(0)", "65535");
    expect_display("bitnot32(0)", "4294967295");
}

TEST_CASE("程序员模式.位移与循环移位运算符") {
    // << >> <<< >>> ~ 是引擎原生的，不依赖任何编译开关
    expect_display("1<<10", "1024");
    expect_display("1024>>10", "1");
    expect_display("255>>4", "15");
    // 循环移位（需要 C++20 特性检测；MSVC 上依赖 /Zc:__cplusplus）
    expect_display("bitlrotate8(1,1)", "2");
    expect_display("bitrrotate8(1,1)", "128");
    expect_display("bitlrotate8(129,1)", "3");
}

TEST_CASE("程序员模式.十六进制字面量") {
    // 0x 字面量是引擎原生支持的
    expect_display("0xFF", "255");
    expect_display("0xff+1", "256");
    expect_display("0x10*0x10", "256");
    expect_display("bitxor(0xF0,0x0F)", "255");
}

TEST_CASE("程序员模式.进制显示") {
    // 同一个数值在不同进制下的展示（十六/八/二进制按无符号位模式显示）
    expect_display("255", "FF", Mode::Programmer, NumBase::Hex);
    expect_display("255", "377", Mode::Programmer, NumBase::Oct);
    expect_display("255", "1111 1111", Mode::Programmer, NumBase::Bin);
    expect_display("255", "255", Mode::Programmer, NumBase::Dec);
    expect_display("bitnot8(0)", "FF", Mode::Programmer, NumBase::Hex);
    expect_display("bitnot16(0)", "FFFF", Mode::Programmer, NumBase::Hex);
}

TEST_CASE("程序员模式.十进制按二补数显示") {
    // 与 Windows 计算器一致：十进制下按有符号解释
    expect_display("bitxor(0,0)-1", "-1", Mode::Programmer, NumBase::Dec);
    expect_display("bitnot8(0)", "-1", Mode::Programmer, NumBase::Dec,
                   WordSize::Bits8);
    expect_display("bitnot16(0)", "-1", Mode::Programmer, NumBase::Dec,
                   WordSize::Bits16);
}

TEST_CASE("程序员模式.字长截断") {
    // 字长会影响显示：超出字长的高位被截掉（Windows 计算器的行为）
    // 255+1 = 256，在 BYTE 字长下截断为 0
    expect_display("bitnot8(0)+1", "0", Mode::Programmer, NumBase::Dec,
                   WordSize::Bits8);
    // 0xFFF 在 WORD(16) 下是 FFF，在 BYTE(8) 下截断为 FF
    expect_display("0xFFF", "FFF", Mode::Programmer, NumBase::Hex,
                   WordSize::Bits16);
    expect_display("0xFFF", "FF", Mode::Programmer, NumBase::Hex,
                   WordSize::Bits8);
}

TEST_CASE("程序员模式.进制转换函数") {
    expect_display("hex2dec(\"FF\")", "255");
    expect_display("bin2dec(\"1010\")", "10");
    expect_display("oct2dec(\"17\")", "15");
    expect_display("hex2dec(\"FF\")+1", "256");
}

TEST_CASE("程序员模式.64位平台门控") {
    if (crosscalc::Engine::supports_64bit()) {
        // 本平台（Linux/macOS 用 TE_LONG_DOUBLE 构建）可以精确表示 64 位整数
        expect_display("0xFFFFFFFFFFFFFFFF", "FFFFFFFFFFFFFFFF", Mode::Programmer,
                       NumBase::Hex, WordSize::Bits64);
        expect_display("bitnot64(0)", "FFFFFFFFFFFFFFFF", Mode::Programmer,
                       NumBase::Hex, WordSize::Bits64);
        expect_display("bitlrotate64(1,32)", "100000000", Mode::Programmer,
                       NumBase::Hex, WordSize::Bits64);
    } else {
        // 本平台（Windows/MSVC）做不到 —— 必须给出明确提示而不是算错
        expect_error("bitnot64(0)", "不支持 64 位", Mode::Programmer,
                     NumBase::Hex, WordSize::Bits64);
        expect_error("1+1", "不支持 64 位", Mode::Programmer, NumBase::Dec,
                     WordSize::Bits64);
    }
}

TEST_CASE("程序员模式.非整数结果的可降级处理") {
    // 程序员模式下算出非整数时，退化成一般十进制显示而不是乱码
    const EvalResult r = ev("10/4", Mode::Programmer, NumBase::Hex,
                            WordSize::Bits32);
    CHECK(r.ok);
    CHECK_EQ(r.display, std::string("2.5"));
    CHECK_FALSE(r.integral);
}

// ===========================================================================
//  错误处理（"计算错误能显示友好的错误提示"）
// ===========================================================================

TEST_CASE("错误处理.除零") {
    expect_error("1/0", "除数不能为零");
    expect_error("0/0", "除数不能为零");
    expect_error("mod(1,0)", "除数不能为零");
    // 错误结果必须是失败状态，不能是 inf
    CHECK_FALSE(ev("1/0").ok);
}

TEST_CASE("错误处理.负数开方") {
    expect_error("sqrt(-1)", "不能对负数开平方");
}

TEST_CASE("错误处理.反三角定义域") {
    expect_error("asin(2)", "ASIN");
}

TEST_CASE("错误处理.未知符号带位置") {
    const EvalResult r = ev("foo+1");
    CHECK_FALSE(r.ok);
    CHECK_CONTAINS(r.error, "未知的符号");
    CHECK_CONTAINS(r.error, "'foo'");
    CHECK_EQ(r.error_pos, 0);   // 出错 token 从下标 0 开始
    CHECK_EQ(r.error_len, 3);   // "foo"
}

TEST_CASE("错误处理.未知函数") {
    const EvalResult r = ev("nosuchfn(1)");
    CHECK_FALSE(r.ok);
    CHECK_CONTAINS(r.error, "未知的函数");
    CHECK_CONTAINS(r.error, "'nosuchfn'");
}

TEST_CASE("错误处理.表达式不完整") {
    const EvalResult r = ev("2+");
    CHECK_FALSE(r.ok);
    CHECK_CONTAINS(r.error, "表达式不完整");
    CHECK(r.error_pos >= 0);
}

TEST_CASE("错误处理.括号不匹配") {
    {
        const EvalResult r = ev("(1+2");
        CHECK_FALSE(r.ok);
        CHECK_CONTAINS(r.error, "右括号");
    }
    {
        const EvalResult r = ev("1+2)");
        CHECK_FALSE(r.ok);
        CHECK_CONTAINS(r.error, "右括号");
    }
}

TEST_CASE("错误处理.空表达式") {
    const EvalResult r = ev("   ");
    CHECK_FALSE(r.ok);
    CHECK_CONTAINS(r.error, "请输入");
}

TEST_CASE("错误处理.静默NaN被识别为错误") {
    // 库对非法字符串输入不报错，只返回 NaN —— 必须被我们拦住
    expect_error("hex2dec(\"ZZ\")", "进制转换失败");
    expect_error("bin2dec(\"2\")", "进制转换失败");
    expect_error("oct2dec(\"9\")", "进制转换失败");
    CHECK_FALSE(ev("hex2dec(\"ZZ\")").ok);
}

TEST_CASE("错误处理.按位运算参数非法") {
    expect_error("bitand(1.5,1)", "整数", Mode::Programmer);
    expect_error("bitnot8(-1)", "", Mode::Programmer);
}

TEST_CASE("错误处理.按位运算超出范围") {
    // 库限制按位运算数值不能超过 2^48-1（0xFFFFFFFFFFFF 恰好是上限，合法），
    // 超过一位即应报错。
    expect_display("bitand(0xFFFFFFFFFFFF,1)", "1", Mode::Programmer);
    expect_error("bitand(0x1000000000000,1)", "超出按位运算允许的范围",
                 Mode::Programmer);
}

TEST_CASE("错误处理.程序员模式十六进制字面量提示") {
    // 直接敲 FF 而不是 0xFF 是最常见的误用，要给出明确指引
    const EvalResult r = ev("FF+1", Mode::Programmer, NumBase::Hex);
    CHECK_FALSE(r.ok);
    CHECK_CONTAINS(r.error, "0x");
}

TEST_CASE("错误处理.错误提示均为中文") {
    // 抽查几个典型错误，确认没有把英文原文直接抛给用户
    const char* exprs[] = {"1/0", "sqrt(-1)", "foo+1", "2+", "(1+2",
                           "hex2dec(\"ZZ\")", "bitand(1.5,1)"};
    for (const char* e : exprs) {
        const EvalResult r = ev(e);
        CHECK_FALSE(r.ok);
        CHECK(!r.error.empty());
        // 至少要包含中文字符，说明经过了翻译层
        bool has_chinese = false;
        for (char c : r.error) {
            if (static_cast<unsigned char>(c) >= 0xE0) {
                has_chinese = true;
                break;
            }
        }
        ::xtest::check_true(has_chinese,
                            (std::string("错误提示应为中文: ") + r.error).c_str(),
                            __FILE__, __LINE__);
    }
}

// ===========================================================================
//  引擎能力 / 运行平台
// ===========================================================================

TEST_CASE("引擎能力.函数清单来自库本身") {
    const std::string listing = crosscalc::Engine::available_functions_text();
    CHECK(listing.size() > 200);
    // 三种模式依赖的关键函数必须存在
    const char* required[] = {"sin",  "cos",  "tan",  "ln",    "log10",
                             "exp",  "pow",  "sqrt", "fac",   "combin",
                             "mod",  "abs",  "hex2dec", "bin2dec", "oct2dec",
                             "bitand", "bitor", "bitxor", "bitlshift",
                             "bitrshift", "if", "clamp"};
    for (const char* fn : required) {
        ::xtest::check_contains(listing, fn, "函数清单", fn, __FILE__, __LINE__);
    }
}

TEST_CASE("引擎能力.平台精度信息自洽") {
    const auto info = crosscalc::Engine::platform_info();
    CHECK(info.max_integer_bitness > 0);
    CHECK(info.max_integer > 0);
    // 库的按位运算上限固定为 2^48-1
    CHECK_NEAR(info.max_bitops_value, 281474976710655.0, 1.0);
    // 不精确支持 64 位时，整数位数必然小于 64
    if (!info.supports_64bit) {
        CHECK(info.max_integer_bitness < 64);
    }
}

TEST_CASE("引擎能力.枚举与字符串互转") {
    Mode m{};
    CHECK(crosscalc::parse_mode("standard", m));
    CHECK(m == Mode::Standard);
    CHECK(crosscalc::parse_mode("scientific", m));
    CHECK(m == Mode::Scientific);
    CHECK(crosscalc::parse_mode("programmer", m));
    CHECK(m == Mode::Programmer);
    CHECK_FALSE(crosscalc::parse_mode("nope", m));

    NumBase b{};
    CHECK(crosscalc::parse_base("hex", b));
    CHECK(b == NumBase::Hex);
    CHECK(crosscalc::parse_base("16", b));
    CHECK(b == NumBase::Hex);
    CHECK(crosscalc::parse_base("bin", b));
    CHECK(b == NumBase::Bin);
    CHECK_FALSE(crosscalc::parse_base("x", b));

    WordSize w{};
    CHECK(crosscalc::parse_word_size("8", w));
    CHECK(w == WordSize::Bits8);
    CHECK(crosscalc::parse_word_size("qword", w));
    CHECK(w == WordSize::Bits64);
    CHECK_FALSE(crosscalc::parse_word_size("7", w));

    CHECK_EQ(std::string(crosscalc::mode_name(Mode::Programmer)),
             std::string("programmer"));
    CHECK_EQ(std::string(crosscalc::base_name(NumBase::Oct)),
             std::string("oct"));
}

// ===========================================================================
//  展示层格式化（number_format）
// ===========================================================================

TEST_CASE("格式化.一般数值") {
    CHECK_EQ(crosscalc::format_general(0.0), std::string("0"));
    CHECK_EQ(crosscalc::format_general(-0.0), std::string("0"));
    CHECK_EQ(crosscalc::format_general(1.0), std::string("1"));
    CHECK_EQ(crosscalc::format_general(-1.5), std::string("-1.5"));
    CHECK_EQ(crosscalc::format_general(1.0 / 3.0),
             std::string("0.3333333333333333"));
    CHECK_EQ(crosscalc::format_general(std::nan("")), std::string("NaN"));
    // 不用 1.0/0.0 字面量：MSVC 会以 C2124（编译期除零）直接报错
    CHECK_EQ(crosscalc::format_general(std::numeric_limits<double>::infinity()),
             std::string("∞"));
    CHECK_EQ(crosscalc::format_general(
                 -std::numeric_limits<double>::infinity()),
             std::string("-∞"));
}

TEST_CASE("格式化.进制转换") {
    CHECK_EQ(crosscalc::format_uint_in_base(255, NumBase::Hex, false),
             std::string("FF"));
    CHECK_EQ(crosscalc::format_uint_in_base(255, NumBase::Bin, true),
             std::string("1111 1111"));
    CHECK_EQ(crosscalc::format_uint_in_base(255, NumBase::Oct, false),
             std::string("377"));
    CHECK_EQ(crosscalc::format_uint_in_base(0, NumBase::Hex, false),
             std::string("0"));
    CHECK_EQ(crosscalc::format_uint_in_base(0xDEADBEEFULL, NumBase::Hex, false),
             std::string("DEADBEEF"));
    CHECK_EQ(crosscalc::format_uint_in_base(1, NumBase::Bin, true),
             std::string("1"));
    CHECK_EQ(crosscalc::format_uint_in_base(0x1234, NumBase::Bin, true),
             std::string("1 0010 0011 0100"));
}

TEST_CASE("格式化.字长截断与符号扩展") {
    CHECK_EQ(crosscalc::mask_to_word(0x1FF, WordSize::Bits8), 0xFFULL);
    CHECK_EQ(crosscalc::mask_to_word(0x1FF, WordSize::Bits16), 0x1FFULL);
    CHECK_EQ(crosscalc::mask_to_word(0xFFFFFFFFFFFFFFFFULL, WordSize::Bits32),
             0xFFFFFFFFULL);

    CHECK_EQ(crosscalc::sign_extend(0xFF, WordSize::Bits8), -1);
    CHECK_EQ(crosscalc::sign_extend(0x7F, WordSize::Bits8), 127);
    CHECK_EQ(crosscalc::sign_extend(0xFFFF, WordSize::Bits16), -1);
    CHECK_EQ(crosscalc::sign_extend(0xFFFFFFFF, WordSize::Bits32), -1);
}

TEST_CASE("格式化.程序员模式十进制按有符号显示") {
    CHECK_EQ(crosscalc::format_programmer(-1.0, NumBase::Hex, WordSize::Bits32),
             std::string("FFFFFFFF"));
    CHECK_EQ(crosscalc::format_programmer(-1.0, NumBase::Dec, WordSize::Bits32),
             std::string("-1"));
    CHECK_EQ(crosscalc::format_programmer(255.0, NumBase::Bin, WordSize::Bits32),
             std::string("1111 1111"));
    CHECK_EQ(crosscalc::format_programmer(2.5, NumBase::Hex, WordSize::Bits32),
             std::string("2.5"));
}

// ===========================================================================
//  已知限制（写成测试，防止以后误以为是 bug 而改坏）
// ===========================================================================

TEST_CASE("已知限制.按位与没有运算符形式") {
    // 引擎里 & 和 | 是【逻辑】运算符，不是按位运算符；
    // 按位与/或/异或必须走 BITAND()/BITOR()/BITXOR() 函数。
    // 写成测试是为了留下证据：& 的行为不是我们配错了。
    expect_display("1&&0", "0");
    expect_display("bitand(12,10)", "8");   // 位与是 8
    // 下面这个如果换成 12&10 会得到 1（逻辑与的真值），而不是 8
    const EvalResult r = ev("12&10");
    CHECK(r.ok);
    CHECK_EQ(r.display, std::string("1"));   // 逻辑与：两者都非零 -> 1
}

TEST_CASE("已知限制.库不提供某些科学函数") {
    // log2 / sec / csc / asinh 等库内没有，按需求约定不自行实现，
    // 因此这些输入应当报"未知的函数"而不是给出结果。
    expect_error("log2(8)", "未知的函数");
    expect_error("sec(0)", "未知的函数");
    expect_error("csc(1)", "未知的函数");
    expect_error("asinh(0)", "未知的函数");
    expect_error("gcd(4,6)", "未知的函数");
    // 对数只能用 ln / log10
    expect_display("ln(e)", "1");
    expect_display("log10(100)", "2");
}

TEST_CASE("已知限制.没有百分号语义") {
    // 库里的 % 是取模，不是"百分比"。需求已确认不支持百分比。
    expect_display("50%3", "2");
    // "50%" 单独出现是语法错误
    expect_error("50%", "表达式不完整");
}

TEST_CASE("已知限制.不支持二进制八进制字面量") {
    // 0b / 0o 不是引擎支持的写法，必须用转换函数
    expect_error("0b1010", "未知的符号");
    expect_error("0o17", "未知的符号");
    expect_display("bin2dec(\"1010\")", "10");
    expect_display("oct2dec(\"17\")", "15");
}
