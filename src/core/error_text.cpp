// ---------------------------------------------------------------------------
//  错误提示翻译层实现
// ---------------------------------------------------------------------------
#include "core/error_text.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace crosscalc {
namespace {

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/// HEX2DEC / BIN2DEC / OCT2DEC 允许的最大数字位数（读自 tinyexpr++ 源码）。
constexpr std::size_t k_max_conversion_digits = 10;

/// 取出表达式里第一个字符串字面量的内容（去掉两侧双引号）。
/// 用来把 "BIN2DEC(\"123\")" 这种输入里的 "123" 抠出来，好把错误说得更具体。
/// 找不到时返回 nullopt。
std::optional<std::string_view> first_string_literal(std::string_view expr) {
    const auto open = expr.find('"');
    if (open == std::string_view::npos) {
        return std::nullopt;
    }
    const auto close = expr.find('"', open + 1);
    if (close == std::string_view::npos) {
        // 只有开引号、没有闭引号：属于语法错误，这里按"没有参数"处理
        return std::nullopt;
    }
    return expr.substr(open + 1, close - open - 1);
}

/// 把函数名写成人习惯的全大写形式：hex2dec -> HEX2DEC
std::string fn_name_pretty(std::string_view fn) {
    std::string out(fn);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

/// 程序员模式下追加的写法提示。
///
/// 按位运算出错时最常见的两个原因是：把十六进制常量写成了裸的 `FF`（库不认识，
/// 只有 `0xFF` 才认），以及以为 `&` / `|` 是按位运算（它们在本项目里始终是逻辑
/// 运算，见 docs/design-decisions.md 第 2 条）。
/// 其它模式返回空串，因此标准/科学模式的提示文案完全不受影响。
std::string bitwise_mode_hint(Mode mode) {
    if (mode != Mode::Programmer) {
        return {};
    }
    return "\n提示：程序员模式下十六进制常量要写成 0x 前缀（如 0xFF）；"
           "按位与/或/异或请用 BITAND() / BITOR() / BITXOR() 函数。";
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' ||
           c == '.';
}

/// 是不是"名字"（函数名/常量名）的开头？数字开头的一律当成数字字面量。
/// 用来区分"手敲 sqrt 打了半截"和"1.2.3 这种数字写错了"。
bool is_name_head(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
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
    //
    // 下面统一用 "先建 std::string 再 += 提示" 的写法，而不是 "字面量 + 提示"。
    // 后者在 GCC 11 -O3 下会触发 -Wstringop-overflow 误报
    // （char_traits.h: writing N bytes into a region of size M，SSO 缓冲被误判），
    // 而 CI 的 ubuntu-22.04 正好是 GCC 11。
    if (contains(m, "Value is too large for bitwise NOT.")) {
        std::string out = "数值超出按位取反（BITNOT）允许的范围。\n"
                          "本库的按位运算只接受 2^48−1 以内的整数。";
        out += bitwise_mode_hint(mode);
        return out;
    }
    if (contains(m, "Value is too large for bitwise operation.")) {
        std::string out = "数值超出按位运算允许的范围。\n"
                          "本库的按位运算只接受 2^48−1 以内的整数。";
        out += bitwise_mode_hint(mode);
        return out;
    }
    if (contains(m, "must use positive")) {
        std::string out = "该按位运算的操作数必须是非负整数。";
        out += bitwise_mode_hint(mode);
        return out;
    }
    if (contains(m, "must use integers") || contains(m, "must be an integer")) {
        std::string out = "该按位运算只能用于整数，请去掉小数部分。";
        out += bitwise_mode_hint(mode);
        return out;
    }
    if (contains(m, "cannot be negative")) {
        std::string out = "该运算的操作数不能为负数。";
        out += bitwise_mode_hint(mode);
        return out;
    }
    if (contains(m, "Overflow in left shift")) {
        std::string out = "左移溢出：被移位的数值太大。\n"
                          "建议：减小被移位的数，或使用更小的字长。";
        out += bitwise_mode_hint(mode);
        return out;
    }
    if (contains(m, "Rotation operation must be between")) {
        std::string out = "循环移位的位数超出当前字长允许的范围。";
        out += bitwise_mode_hint(mode);
        return out;
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

SyntaxDiagnosis diagnose_syntax_error(std::string_view lib_message, int pos,
                                      std::string_view expr) {
    const std::string_view m = trim_view(lib_message);

    // 库偶尔会给出有效消息（例如 64 位按位运算、内部错误），优先翻译它。
    // 库既然开口了，就说明它认得出这是个正经错误，不是"还没输完"。
    if (!m.empty()) {
        return {friendly_calc_error(m, Mode::Standard), false};
    }

    const int len = static_cast<int>(expr.size());

    // 1) 括号不配平是最常见的输入错误，且能给出非常明确的提示
    int missing_close = 0;
    int extra_close = 0;
    if (paren_imbalance(expr, missing_close, extra_close)) {
        if (extra_close > 0) {
            return {"右括号 ')' 多余，请检查括号是否配对。", false};
        }
        // 右括号少几个，几乎总是"还没敲到那里"
        return {"缺少 " + std::to_string(missing_close) + " 个右括号 ')'。",
                true};
    }

    // 2) 没有位置信息
    if (pos < 0) {
        return {"表达式无法解析，请检查输入。", false};
    }

    const int human_pos = pos + 1;  // 给用户看的位置从 1 开始数

    // 3) 位置落在末尾之后 -> 表达式被截断了
    if (pos >= len) {
        return {"表达式不完整：末尾缺少操作数或右括号（位置 " +
                    std::to_string(human_pos) + "）。",
                true};
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
            return {"未知的函数：" + quote_token(tok) +
                        "（位置 " + std::to_string(human_pos) + "）。\n"
                        "请检查函数名拼写，或确认它属于当前计算器模式。",
                    false};
        }
        // 出错 token 一直顶到表达式末尾，而且是以字母开头的名字，说明用户可能
        // 正一个字母一个字母地敲函数名（"s"、"sq"、"sqr"、"sqrt"），此刻还看不出
        // 他到底要写什么，所以按"还没输完"处理，等按下等号/回车再判错。
        // 数字开头的 token（"1.2.3"、"0b1010"）不算：那一眼就是写错了。
        const bool name_being_typed =
            after >= expr.size() &&
            is_name_head(expr[static_cast<size_t>(start)]);
        return {"未知的符号：" + quote_token(tok) + "（位置 " +
                    std::to_string(human_pos) + "）。\n"
                    "本计算器不支持变量，请检查拼写或改用库内建的常量（如 PI、E）。",
                name_being_typed};
    }

    if (c == ')') {
        return {"右括号 ')' 多余（位置 " + std::to_string(human_pos) + "）。",
                false};
    }
    if (c == ',') {
        return {"参数分隔符 ',' 的位置不正确（位置 " +
                    std::to_string(human_pos) + "）。",
                false};
    }
    if (c == '.') {
        return {"小数点位置不正确（位置 " + std::to_string(human_pos) + "）。",
                false};
    }

    // 5) 二元运算符附近缺操作数 —— 库给出的位置就是那个运算符，
    //    这是最常见的一类输入错误（"2+"、"*3"、"2+*3"），值得讲清楚。
    if (is_binary_operator(c)) {
        const bool has_rhs = has_nonspace_after(expr, pos);
        const bool has_lhs = has_nonspace_before(expr, pos);
        if (!has_rhs && !has_lhs) {
            // 刚敲下一个运算符，等号右边还什么都没有
            return {std::string("表达式不完整：只输入了一个运算符 '") + c + "'。",
                    true};
        }
        if (!has_rhs) {
            // "2+" —— 用户多半正准备敲下一个数
            return {std::string("表达式不完整：运算符 '") + c +
                        "' 后面缺少操作数（位置 " + std::to_string(human_pos) +
                        "）。",
                    true};
        }
        if (!has_lhs) {
            // "*3"：以运算符开头，是明确的输入错误，不是"还没输完"
            return {std::string("表达式不能以运算符 '") + c +
                        "' 开头（位置 " + std::to_string(human_pos) + "）。",
                    false};
        }
        // "2+*3"：两个运算符挨在一起，是写错了，不是还没写完
        return {std::string("运算符 '") + c + "' 附近的操作数不完整（位置 " +
                    std::to_string(human_pos) + "）。",
                false};
    }

    return {"表达式在位置 " + std::to_string(human_pos) + " 处无法解析：" +
                describe_char(c) + "。",
            false};
}

std::string friendly_invalid_result(std::string_view expr, Mode mode) {
    const std::string_view e = trim_view(expr);

    // 程序员模式下补一句"这个模式里该怎么写"，因为最容易踩的坑就是十六进制字面量
    // 没有 0x 前缀（库不认识裸的 FF），以及把 & 当成按位与。
    const std::string prog_hint =
        (mode == Mode::Programmer)
            ? "\n提示：十六进制常量要写成 0x 前缀（如 0xFF）；"
              "按位与/或/异或请用 BITAND() / BITOR() / BITXOR() 函数。"
            : std::string();

    // 以转换函数开头的输入，绝大多数是"字符串内容不是合法数字"或"位数超限"。
    //
    // 这三个转换函数的真实行为（读自 tinyexpr++ 源码，见
    // docs/design-decisions.md 第 11 条）都很反直觉，报错时必须讲清楚，
    // 否则用户会以为是自己数字打错了：
    //   * 参数最长 10 个字符，超了直接得到 NaN（不是抛异常）；
    //   * 正好 10 个字符时按【有符号】解释：bin 是 10 位、oct 是 30 位、
    //     hex 是 40 位。所以 BIN2DEC("1111111111") 是 -1 而不是 1023。
    static const char* kConverters[] = {"hex2dec", "bin2dec", "oct2dec",
                                        "numbervalue"};
    for (const char* fn : kConverters) {
        const std::size_t fn_len = std::char_traits<char>::length(fn);
        if (e.size() >= fn_len &&
            std::equal(fn, fn + fn_len, e.begin(),
                       [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) ==
                                  std::tolower(static_cast<unsigned char>(b));
                       })) {
            std::string out = std::string("进制转换失败：") + fn +
                              "() 的参数无效（参数个数不正确，或含该进制不允许的数字）。\n";

            // 能把参数里的字符串抠出来时，给出更具体的原因
            if (const auto arg = first_string_literal(expr); arg.has_value()) {
                if (arg->size() > k_max_conversion_digits) {
                    out = std::string("进制转换失败：") + fn +
                          "() 的参数有 " + std::to_string(arg->size()) +
                          " 位数字，超过上限 " +
                          std::to_string(k_max_conversion_digits) +
                          " 位。\n本库的 HEX2DEC / BIN2DEC / OCT2DEC 最多处理 " +
                          std::to_string(k_max_conversion_digits) +
                          " 位数字；更长的数值请直接写成十六进制字面量"
                          "（如 0x1234ABCD），字面量可以精确表示 64 位整数。";
                    out += prog_hint;
                    return out;
                }
                out = std::string("进制转换失败：\"") + std::string(*arg) +
                      "\" 不是 " + fn_name_pretty(fn) + " 能识别的合法数字。\n"
                      "例如 HEX2DEC(\"FF\") = 255；HEX2DEC(\"ZZ\") 不是合法输入。";
            } else {
                out += "例如 HEX2DEC(\"FF\") = 255；HEX2DEC(\"ZZ\") 不是合法输入。";
            }
            out += prog_hint;
            return out;
        }
    }
    std::string out = "无法计算出有效结果（结果为 NaN）。\n请检查输入的数字格式是否正确。";
    out += prog_hint;
    return out;
}

}  // namespace crosscalc
