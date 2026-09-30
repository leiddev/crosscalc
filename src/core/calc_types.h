// ---------------------------------------------------------------------------
//  crosscalc —— 三种模式共用的类型定义
//
//  这里只定义"引擎对外能看见的东西"，不包含任何计算逻辑。
//  所有实际计算都发生在 crosscalc_core/calc_engine.cpp，最终由
//  tinyexpr-plusplus 完成。
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace crosscalc {

/// 三种计算器模式。
enum class Mode {
    Standard,    ///< 标准计算器：四则运算、括号、取模、乘方
    Scientific,  ///< 科学计算器：三角 / 对数 / 指数 / 阶乘 / 排列组合等
    Programmer,  ///< 程序员计算器：按位运算、位移、循环移位、多进制显示
};

/// 进制（程序员模式的显示进制，同时也是输入数字的进制）。
enum class NumBase : int {
    Bin = 2,
    Oct = 8,
    Dec = 10,
    Hex = 16,
};

/// 引擎的宽数值类型，与 tinyexpr-plusplus 的 te_type 一一对应。
///
/// tinyexpr++ 的内部数值类型 te_type 默认是 double，只能精确表示 2^53−1 以内的
/// 整数。CMakeLists 会在 long double 比 double 更宽的平台（x86-64 的 Linux/macOS）
/// 上给 tinyexpr++ 定义 TE_LONG_DOUBLE，此时 te_type 就是 80 位扩展精度，
/// 能精确装下 uint64_t。MSVC 的 long double 与 double 同宽，定义了也没用，
/// 所以 Windows 上恒为 "53 位精确整数"。
///
/// 因此本项目全程用这个宽类型承载计算结果，而不是 double：否则 64 位的位运算
/// 结果在存进 EvalResult 的那一瞬间就被舍入成 53 位了。
/// QWORD 是否可用由 te_parser::supports_64bit() 在运行时决定，
/// 见 Engine::supports_64bit()。
///
/// @note /api/eval 返回的 JSON 数字字段（value / max_integer）仍是 double 精度
///       ——JSON 的数字本来就承载不了 64 位整数。程序员模式下要看精确值请用
///       display 字符串（十六进制/二进制位模式）。
using num_t = long double;

/// 程序员模式的字长。
///
/// 8 / 16 / 32 位在所有平台上都是精确的；64 位（QWORD）只在
/// te_parser::supports_64bit() 为真的平台上开放，见上面的 num_t 说明。
enum class WordSize : int {
    Bits8 = 8,
    Bits16 = 16,
    Bits32 = 32,
    Bits64 = 64,
};

/// 一次求值请求。
struct EvalRequest {
    std::string expression;                ///< 用户输入的表达式
    Mode mode = Mode::Standard;            ///< 模式
    NumBase base = NumBase::Dec;           ///< 显示进制（程序员模式生效）
    WordSize word_size = WordSize::Bits32; ///< 字长（程序员模式生效）
};

/// 一次求值结果。
struct EvalResult {
    bool ok = false;         ///< 是否成功
    num_t value = 0.0;       ///< 引擎给出的原始数值（成功时有意义）
    std::string display;     ///< 展示用字符串（按模式与进制格式化）
    std::string error;       ///< 中文友好错误提示（ok == false 时非空）

    /// 出错位置（0 基下标）；-1 表示没有位置信息（例如纯计算错误）。
    int error_pos = -1;
    /// 出错位置对应的 token 长度，供前端高亮；0 表示无。
    int error_len = 0;
    /// 结果是否为整数（程序员模式判断要不要按整数渲染）。
    bool integral = false;
};

/// 运行期能力信息，供前端决定按钮可用性并给出解释。
struct PlatformInfo {
    bool supports_64bit = false;  ///< te_type 能否精确容纳 uint64_t
    int max_integer_bitness = 0;  ///< 精确整数位数（double 为 53，80 位 long double 为 64）
    num_t max_integer = 0.0;      ///< 最大精确整数
    num_t max_bitops_value = 0.0; ///< 按位运算允许的最大数值（库限制 2^48−1）
};

/// 引擎版本号（与 CMake 的 project(VERSION) 保持一致，由 CMake 注入）。
#ifndef CROSSCALC_VERSION
#define CROSSCALC_VERSION "0.0.0"
#endif

const char* mode_name(Mode m);
const char* base_name(NumBase b);

/// 字符串 -> 枚举。只接受小写形式（"standard"/"scientific"/"programmer"、
/// "bin"/"oct"/"dec"/"hex"、"8"/"16"/"32"/"64"）。
bool parse_mode(std::string_view s, Mode& out);
bool parse_base(std::string_view s, NumBase& out);
bool parse_word_size(std::string_view s, WordSize& out);

}  // namespace crosscalc
