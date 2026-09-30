// ---------------------------------------------------------------------------
//  数字展示层格式化实现（无任何数学计算，只有进制转换与字符串处理）
// ---------------------------------------------------------------------------
#include "core/number_format.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace crosscalc {
namespace {

char digit_char(unsigned d) {
    return static_cast<char>(d < 10 ? ('0' + d) : ('A' + (d - 10)));
}

}  // namespace

std::string format_general(num_t v) {
    if (std::isnan(v)) {
        return "NaN";
    }
    if (std::isinf(v)) {
        return v < 0 ? "-∞" : "∞";
    }
    if (v == 0.0) {
        // 归一化 -0，避免显示成 "-0"
        return "0";
    }

    // 显示精度以 double（17 位有效数字）为准，而不是 num_t 的 80 位扩展精度：
    //   * 同一个表达式必须在 Windows 和 Linux 上显示成同一个字符串，否则黄金向量
    //     没法共用，用户也会觉得"换个系统答案就变了"；
    //   * 按 80 位精度如实展开会带来大量无意义位数，例如
    //     0.1+0.2 变成 0.300000000000000000011，而它并没有比 0.30000000000000004
    //     提供更多有用信息，可读性反而没了。
    // num_t 的额外精度留在【计算】里（见 calc_engine.cpp 的说明），显示层统一
    // 收敛到 double 的"最短且能唯一还原"表示 —— 这也是 standard.json 里
    // 0.1+0.2 期望 0.30000000000000004 的原因。
    const double dv = static_cast<double>(v);
    char buf[64];
    // 从 15 位往上试，取第一个能精确还原的精度。这样 0.1+0.2 显示
    // 0.30000000000000004、sqrt(2) 显示 1.4142135623730951，
    // 而 17 位必然能唯一确定一个 double，循环一定会在 17 位结束。
    for (const int precision : {15, 16, 17}) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, dv);
        if (std::strtod(buf, nullptr) == dv) {
            return buf;
        }
    }
    return buf;
}

std::string format_uint_in_base(uint64_t v, NumBase base, bool group_binary) {
    if (base == NumBase::Dec) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
        return buf;
    }

    const unsigned radix = static_cast<unsigned>(base);
    char tmp[80];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v != 0 && n < static_cast<int>(sizeof(tmp))) {
        tmp[n++] = digit_char(static_cast<unsigned>(v % radix));
        v /= radix;
    }

    std::string out;
    out.reserve(static_cast<size_t>(n) + 16);
    for (int i = n - 1; i >= 0; --i) {
        out.push_back(tmp[i]);
        // 二进制每 4 位加空格，从右往左数；i 是当前字符在 tmp 中的位置，
        // 其"右侧还有多少位" = i，所以 i % 4 == 0 时该补空格（且不是最后一位）。
        if (group_binary && base == NumBase::Bin && i > 0 && (i % 4) == 0) {
            out.push_back(' ');
        }
    }
    return out;
}

uint64_t mask_to_word(uint64_t v, WordSize ws) {
    const int bits = static_cast<int>(ws);
    if (bits >= 64) {
        return v;
    }
    return v & ((uint64_t{1} << bits) - 1);
}

int64_t sign_extend(uint64_t v, WordSize ws) {
    const int bits = static_cast<int>(ws);
    if (bits >= 64) {
        return static_cast<int64_t>(v);
    }
    const uint64_t sign = uint64_t{1} << (bits - 1);
    const uint64_t mask = (sign << 1) - 1;
    v &= mask;
    // 二补数解释：最高位为 1 时减去 2^bits
    return static_cast<int64_t>((v ^ sign) - sign);
}

bool is_integral_value(num_t v) {
    if (!std::isfinite(v)) {
        return false;
    }
    // 除了"是整数"，还要求能装进 64 位位模式：超过 2^64 的整数（例如 1e30）
    // 是数学上的整数，却没有对应的位模式，后面的 static_cast<uint64_t> 会变成
    // 未定义行为。这里一并排除，让调用方走十进制一般显示。
    if (std::fabs(v) >= 18446744073709551616.0L) {  // 2^64
        return false;
    }
    return v == std::trunc(v);
}

std::string format_programmer(num_t v, NumBase base, WordSize ws) {
    if (std::isnan(v)) {
        return "NaN";
    }
    if (std::isinf(v)) {
        return v < 0 ? "-∞" : "∞";
    }
    if (!is_integral_value(v)) {
        // 非整数没法按位显示（例如程序员模式里算了 2.5 这种），
        // 退化成十进制一般显示，前端会额外提示。
        return format_general(v);
    }

    // num_t -> 位模式（二补数）。
    // 走 int64_t 再转 uint64_t 是为了让负数得到正确的二补数位模式
    // （负数直接转 uint64_t 是未定义行为）。
    uint64_t bits = 0;
    if (v < 0) {
        bits = static_cast<uint64_t>(static_cast<int64_t>(v));
    } else if (v < 9223372036854775808.0L) {  // 2^63
        bits = static_cast<uint64_t>(static_cast<int64_t>(v));
    } else {
        // [2^63, 2^64)：int64_t 装不下，但 uint64_t 装得下。
        // 只有支持 64 位精度的平台才会走到这里（见 calc_types.h 的 num_t）。
        bits = static_cast<uint64_t>(v);
    }

    // 先按字长截断，再决定怎么显示：
    //   十六进制 / 八进制 / 二进制：按无符号位模式显示（FFFFFFFF 而不是 -1）
    //   十进制：按二补数有符号值显示（-1 而不是 4294967295）——与 Windows 计算器一致
    const uint64_t masked = mask_to_word(bits, ws);

    if (base == NumBase::Dec) {
        const int64_t signed_val = sign_extend(masked, ws);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(signed_val));
        return buf;
    }

    return format_uint_in_base(masked, base, /*group_binary=*/base == NumBase::Bin);
}

}  // namespace crosscalc
