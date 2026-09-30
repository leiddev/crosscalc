// ---------------------------------------------------------------------------
//  计算引擎实现
// ---------------------------------------------------------------------------
#include "core/calc_engine.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include "core/error_text.h"
#include "core/number_format.h"
#include "tinyexpr.h"

namespace crosscalc {

// num_t 必须能无损承载 tinyexpr++ 的 te_type，否则计算精度会在进入显示层之前
// 就被截掉（见 number_format.h 与 docs/design-decisions.md 第 3 条）。
// te_type 只可能是 double 或 long double，而 num_t 就是 long double，
// 这个 static_assert 是为了挡住"把 num_t 改成 double/float"这类改动。
static_assert(sizeof(num_t) >= sizeof(te_type),
              "num_t 必须能无损承载 te_type（不能窄化，否则 64 位结果会被舍入）");
namespace {

/// 整个 token 是否只由十六进制数字组成（用于给出"请加 0x 前缀"的提示）
bool looks_like_hex_literal(std::string_view tok) {
    if (tok.empty()) {
        return false;
    }
    for (char c : tok) {
        const bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                            (c >= 'A' && c <= 'F');
        if (!is_hex) {
            return false;
        }
    }
    // "e" 之类会被当成科学计数法，只有当含 a-f 或长度>=2 时才提示，避免误伤
    bool has_alpha = false;
    for (char c : tok) {
        if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            has_alpha = true;
        }
    }
    return has_alpha || tok.size() >= 2;
}

/// 解析失败的兜底：尽最大努力拿到库认为的出错位置。
int safe_error_position(const te_parser& p) {
    const int npos = static_cast<int>(te_parser::npos);
    const auto raw = p.get_last_error_position();
    if (raw == te_parser::npos || raw < 0) {
        return -1;
    }
    (void)npos;
    return static_cast<int>(raw);
}

}  // namespace

PlatformInfo Engine::platform_info() {
    PlatformInfo info;
    info.supports_64bit = te_parser::supports_64bit();
    info.max_integer_bitness = te_parser::get_max_integer_bitness();
    // 不做窄化转换：在支持 TE_LONG_DOUBLE 的平台上 get_max_integer() 是
    // 2^64−1，转成 double 会变成 2^64（3.6e19 已经超出 double 的整数精度）。
    info.max_integer = te_parser::get_max_integer();
    info.max_bitops_value = static_cast<num_t>(te_parser::MAX_BITOPS_VAL);
    return info;
}

bool Engine::supports_64bit() { return te_parser::supports_64bit(); }

num_t Engine::max_bitops_value() {
    return static_cast<num_t>(te_parser::MAX_BITOPS_VAL);
}

std::string Engine::available_functions_text() {
    te_parser p;
    return p.list_available_functions_and_variables();
}

EvalResult Engine::evaluate(const EvalRequest& req) const {
    EvalResult out;

    const std::string_view trimmed = trim_view(req.expression);
    if (trimmed.empty()) {
        out.error = "请输入要计算的表达式。";
        return out;
    }
    std::string expr(trimmed);

    // ---- 程序员模式：QWORD 门控 ------------------------------------------
    // 引擎基于 double 时只能精确表示 2^53-1 以内的整数，
    // 与其给出一个悄悄算错的结果，不如明确拒绝。
    if (req.mode == Mode::Programmer && req.word_size == WordSize::Bits64 &&
        !te_parser::supports_64bit()) {
        out.error =
            "当前平台不支持 64 位（QWORD）整数运算。\n"
            "原因：本平台的计算引擎以 double 为数值类型，只能精确表示 2^53−1 "
            "以内的整数；\n"
            "Windows/MSVC 的 long double 与 double 同样宽度，无法通过编译选项改善。\n"
            "建议：改用 DWORD（32 位）或更小的字长。";
        return out;
    }

    te_parser parser;
    parser.set_decimal_separator('.');
    parser.set_list_separator(',');

    te_type raw = 0;
    try {
        raw = parser.evaluate(expr);
    } catch (const std::exception& e) {
        out.ok = false;
        out.error = friendly_calc_error(e.what(), req.mode);
        out.error_pos = safe_error_position(parser);
        if (out.error_pos >= 0) {
            int start = -1;
            int len = 0;
            token_span_at(expr, out.error_pos, start, len);
            if (start >= 0 && len > 0) {
                out.error_pos = start;
                out.error_len = len;
            }
        }
        return out;
    } catch (...) {
        out.error = "计算失败：发生了未知错误。";
        return out;
    }

    // ---- 形态 2：解析/符号解析失败 ---------------------------------------
    if (!parser.success()) {
        const int pos = safe_error_position(parser);
        out.ok = false;
        out.error_pos = pos;
        out.error = friendly_syntax_error(parser.get_last_error_message(), pos, expr);

        int start = -1;
        int len = 0;
        if (pos >= 0) {
            token_span_at(expr, pos, start, len);
        }
        if (start >= 0 && len > 0) {
            out.error_pos = start;
            out.error_len = len;

            // 程序员模式常见误用：直接敲 FF 而不是 0xFF
            if (req.mode == Mode::Programmer &&
                looks_like_hex_literal(
                    std::string_view(expr).substr(static_cast<size_t>(start),
                                                  static_cast<size_t>(len)))) {
                out.error +=
                    "\n提示：十六进制字面量需要 0x 前缀，例如 0xFF；"
                    "或使用 HEX2DEC(\"FF\")。";
            }
        }
        return out;
    }

    // 这里【不能】窄化成 double：te_type 在支持 TE_LONG_DOUBLE 的平台上就是
    // num_t（80 位扩展精度）。一旦转成 double，64 位的位运算结果会在进入显示层
    // 之前就被舍入 —— 例如 0xFFFFFFFFFFFFFFFF（2^64−1）会变成 2^64，
    // 于是程序员模式显示成 18446744073709551616 而不是 FFFFFFFFFFFFFFFF。
    // 计算全程用 num_t，只在最后"显示"这一步才按 double 精度收敛（见 number_format）。
    const num_t value = static_cast<num_t>(raw);

    // ---- 形态 3：库返回 NaN 但认为"成功" ---------------------------------
    if (std::isnan(value)) {
        out.ok = false;
        const std::string& lib_msg = parser.get_last_error_message();
        if (!lib_msg.empty()) {
            // 有些情况库既返回 NaN 又留了消息，优先用消息
            out.error = friendly_calc_error(lib_msg, req.mode);
        } else {
            out.error = friendly_invalid_result(expr, req.mode);
        }
        return out;
    }

    if (std::isinf(value)) {
        out.ok = false;
        out.error =
            "结果超出可表示范围（溢出）。\n"
            "请缩小参与运算的数值，或检查是否发生了除以极小数的运算。";
        return out;
    }

    // ---- 成功 -------------------------------------------------------------
    out.ok = true;
    out.value = value;
    out.integral = is_integral_value(value);
    out.display = (req.mode == Mode::Programmer)
                      ? format_programmer(value, req.base, req.word_size)
                      : format_general(value);
    return out;
}

}  // namespace crosscalc
