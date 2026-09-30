// ---------------------------------------------------------------------------
//  错误提示翻译层实现
// ---------------------------------------------------------------------------
#include "core/error_text.h"

#include <algorithm>
#include <cctype>

namespace crosscalc {
namespace {

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' ||
           c == '.';
}

std::string quote_token(std::string_view t) {
    return "'" + std::string(t) + "'";
}

/// 二元运算符（一元 +/- 不在此列，因为 "2*-3" 是合法的）
bool is_binary_operator(char c) {
    switch (c) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '^':
        case '&':
        case '|':
        case '<':
        case '>':
            return true;
        default:
            return false;
    }
}

bool has_nonspace_after(std::string_view s, int pos) {
    for (size_t i = static_cast<size_t>(pos) + 1; i < s.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(s[i])) == 0) {
            return true;
        }
    }
    return false;
}

bool has_nonspace_before(std::string_view s, int pos) {
    for (int i = pos - 1; i >= 0; --i) {
        if (std::isspace(static_cast<unsigned char>(s[static_cast<size_t>(i)])) ==
            0) {
            return true;
        }
    }
    return false;
}

/// 括号是否配对（忽略字符串字面量里的括号，避免误判 HEX2DEC("(") 这类输入）
bool paren_imbalance(std::string_view expr, int& missing_close, int& extra_close) {
    missing_close = 0;
    extra_close = 0;
    int depth = 0;
    bool in_string = false;
    for (char c : expr) {
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth == 0) {
                ++extra_close;
            } else {
                --depth;
            }
        }
    }
    missing_close = depth;
    return missing_close != 0 || extra_close != 0;
}

/// 表达式里的中文数字字符 -> 描述，用于把"位置 N"讲得更清楚
std::string describe_char(char c) {
    switch (c) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '^':
        case '&':
        case '|':
            return "运算符 '" + std::string(1, c) + "'";
        case '(':
            return "左括号 '('";
        case ')':
            return "右括号 ')'";
        case ',':
            return "参数分隔符 ','";
        default:
            return "字符 '" + std::string(1, c) + "'";
    }
}

}  // namespace

std::string_view trim_view(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) {
        --e;
    }
    return s.substr(b, e - b);
}

void token_span_at(std::string_view expr, int pos, int& out_start, int& out_len) {
    out_start = -1;
    out_len = 0;
    if (pos < 0 || pos > static_cast<int>(expr.size())) {
        return;
    }
    // 出错位置常常指向 token 的"末尾之后"或中间的某个字符，
    // 这里向前后各扩到非标识符字符为止。
    int idx = pos;
    if (idx >= static_cast<int>(expr.size())) {
        idx = static_cast<int>(expr.size()) - 1;
    }
    if (idx < 0) {
        return;
    }
    if (!is_ident_char(expr[static_cast<size_t>(idx)])) {
        // 指向的不是标识符（例如是括号/运算符），就把这一个字符当作 token
        out_start = idx;
        out_len = 1;
        return;
    }
    int b = idx;
    while (b > 0 && is_ident_char(expr[static_cast<size_t>(b - 1)])) {
        --b;
    }
    int e = idx;
    while (e + 1 < static_cast<int>(expr.size()) &&
           is_ident_char(expr[static_cast<size_t>(e + 1)])) {
        ++e;
    }
    out_start = b;
    out_len = e - b + 1;
}

std::string friendly_calc_error(std::string_view lib_message, Mode mode) {
    const std::string_view m = trim_view(lib_message);
    if (m.empty()) {
        return "计算失败：表达式无法求值。";
    }

    // ---- 与"平台整数精度"相关的，要说得最清楚，因为这是本项目的已知硬限制 ----
    if (contains(m, "64-bit bitwise operations are not supported")) {
        return "当前平台无法进行 64 位按位运算。\n"
               "原因：本平台的计算引擎基于 double，只能精确表示 2^53−1 以内的整数"
               "（Windows/MSVC 上 long double 与 double 同宽，无法改善）。\n"
               "建议：改用 32 位或更小的字长。";
    }
    if (contains(m, "32-bit bitwise operations are not supported")) {
        return "当前平台无法进行 32 位按位运算。\n"
               "原因：计算引擎被编译成浮点模式（TE_FLOAT），整数按位运算不可用。";
    }

    // ---- 除零 / 取模零 ----
    if (m == "Division by zero.") {
        return "除数不能为零。";
    }
    if (m == "Modulus by zero.") {
        return "取模运算的除数不能为零。";
    }

    // ---- 定义域 ----
    if (m == "Negative value passed to SQRT.") {
        return "不能对负数开平方：SQRT 的参数必须 ≥ 0。";
    }
    if (m == "Argument passed to ASIN must be between -1 and 1.") {
        return "ASIN 的参数必须介于 −1 与 1 之间。";
    }

    // ---- 按位运算的参数类型/范围 ----
    if (contains(m, "Value is too large for bitwise NOT.")) {
        return "数值超出按位取反（BITNOT）允许的范围。\n"
               "本库的按位运算只接受 2^48−1 以内的整数。";
    }
    if (contains(m, "Value is too large for bitwise operation.")) {
        return "数值超出按位运算允许的范围。\n"
               "本库的按位运算只接受 2^48−1 以内的整数。";
    }
    if (contains(m, "must use positive")) {
        return "该按位运算的操作数必须是非负整数。";
    }
    if (contains(m, "must use integers") || contains(m, "must be an integer")) {
        return "该按位运算只能用于整数，请去掉小数部分。";
    }
    if (contains(m, "cannot be negative")) {
        return "该运算的操作数不能为负数。";
    }
    if (contains(m, "Overflow in left shift")) {
        return "左移溢出：被移位的数值太大。\n"
               "建议：减小被移位的数，或使用更小的字长。";
    }
    if (contains(m, "Rotation operation must be between")) {
        return "循环移位的位数超出当前字长允许的范围。";
    }
    if (contains(m, "List and decimal separators cannot be the same")) {
        return "小数点与列表分隔符不能相同（内部配置错误）。";
    }
    if (contains(m, "Internal error in parser")) {
        return "内部解析错误，请检查表达式后重试。";
    }

    // 兜底：保留库的原文，便于排查，但不让用户只看到英文
    return "计算失败：" + std::string(m);
}

std::string friendly_syntax_error(std::string_view lib_message, int pos,
                                  std::string_view expr) {
    const std::string_view m = trim_view(lib_message);

    // 库偶尔会给出有效消息（例如 64 位按位运算、内部错误），优先翻译它
    if (!m.empty()) {
        return friendly_calc_error(m, Mode::Standard);
    }

    const int len = static_cast<int>(expr.size());

    // 1) 括号不配平是最常见的输入错误，且能给出非常明确的提示
    int missing_close = 0;
    int extra_close = 0;
    if (paren_imbalance(expr, missing_close, extra_close)) {
        if (extra_close > 0) {
            return "右括号 ')' 多余，请检查括号是否配对。";
        }
        return "缺少 " + std::to_string(missing_close) + " 个右括号 ')'。";
    }

    // 2) 没有位置信息
    if (pos < 0) {
        return "表达式无法解析，请检查输入。";
    }

    const int human_pos = pos + 1;  // 给用户看的位置从 1 开始数

    // 3) 位置落在末尾之后 -> 表达式被截断了
    if (pos >= len) {
        return "表达式不完整：末尾缺少操作数或右括号（位置 " +
               std::to_string(human_pos) + "）。";
    }

    // 4) 尝试取出出错 token
    int start = -1;
    int tlen = 0;
    token_span_at(expr, pos, start, tlen);
    const char c = expr[static_cast<size_t>(pos)];

    if (start >= 0 && tlen > 0 && is_ident_char(c)) {
        const std::string_view tok = expr.substr(static_cast<size_t>(start),
                                                static_cast<size_t>(tlen));
        // 后面紧跟 '(' 说明是想调用函数，否则是个未知变量/常量
        const size_t after = static_cast<size_t>(start + tlen);
        const bool looks_like_call =
            after < expr.size() && expr[after] == '(';
        if (looks_like_call) {
            return "未知的函数：" + quote_token(tok) +
                   "（位置 " + std::to_string(human_pos) + "）。\n"
                   "请检查函数名拼写，或确认它属于当前计算器模式。";
        }
        return "未知的符号：" + quote_token(tok) +
               "（位置 " + std::to_string(human_pos) + "）。\n"
               "本计算器不支持变量，请检查拼写或改用库内建的常量（如 PI、E）。";
    }

    if (c == ')') {
        return "右括号 ')' 多余（位置 " + std::to_string(human_pos) + "）。";
    }
    if (c == ',') {
        return "参数分隔符 ',' 的位置不正确（位置 " + std::to_string(human_pos) +
               "）。";
    }
    if (c == '.') {
        return "小数点位置不正确（位置 " + std::to_string(human_pos) + "）。";
    }

    // 5) 二元运算符附近缺操作数 —— 库给出的位置就是那个运算符，
    //    这是最常见的一类输入错误（"2+"、"*3"、"2+*3"），值得讲清楚。
    if (is_binary_operator(c)) {
        const bool has_rhs = has_nonspace_after(expr, pos);
        const bool has_lhs = has_nonspace_before(expr, pos);
        if (!has_rhs && !has_lhs) {
            return std::string("表达式不完整：只输入了一个运算符 '") + c + "'。";
        }
        if (!has_rhs) {
            return std::string("表达式不完整：运算符 '") + c +
                   "' 后面缺少操作数（位置 " + std::to_string(human_pos) +
                   "）。";
        }
        if (!has_lhs) {
            return std::string("表达式不能以运算符 '") + c +
                   "' 开头（位置 " + std::to_string(human_pos) + "）。";
        }
        return std::string("运算符 '") + c + "' 附近的操作数不完整（位置 " +
               std::to_string(human_pos) + "）。";
    }

    return "表达式在位置 " + std::to_string(human_pos) + " 处无法解析：" +
           describe_char(c) + "。";
}

std::string friendly_invalid_result(std::string_view expr, Mode mode) {
    const std::string_view e = trim_view(expr);
    // 以转换函数开头的输入，绝大多数是"字符串内容不是合法数字"
    static const char* kConverters[] = {"hex2dec", "bin2dec", "oct2dec",
                                        "numbervalue"};
    for (const char* fn : kConverters) {
        if (e.size() >= std::char_traits<char>::length(fn) &&
            std::equal(fn, fn + std::char_traits<char>::length(fn), e.begin(),
                       [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) ==
                                  std::tolower(static_cast<unsigned char>(b));
                       })) {
            return std::string("进制转换失败：") + fn +
                   "() 的参数不是该进制的合法数字，或参数个数不正确。\n"
                   "例如 HEX2DEC(\"FF\") = 255；HEX2DEC(\"ZZ\") 不是合法输入。";
        }
    }
    (void)mode;
    return "无法计算出有效结果（结果为 NaN）。\n请检查输入的数字格式是否正确。";
}

}  // namespace crosscalc
