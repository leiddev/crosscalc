// ---------------------------------------------------------------------------
//  crosscalc 计算引擎
//
//  这一层是整个程序【唯一】的计算入口，职责有三：
//
//   1. 驱动 tinyexpr-plusplus 完成求值（不自己实现任何数学运算）；
//   2. 把库的三种失败形态统一成 EvalResult.error（中文提示 + 位置）：
//        · 抛异常（除零、负数开方、按位运算参数非法、溢出……）
//        · 返回 false 且 get_last_error_message() 为空（语法 / 未知符号）
//        · 返回 NaN 且 success() 仍为 true（HEX2DEC("ZZ")、参数个数不匹配）
//   3. 按模式/进制把结果格式化好交给前端。
//
//  设计说明见 docs/design-decisions.md
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

#include "core/calc_types.h"

namespace crosscalc {

class Engine {
public:
    Engine() = default;

    /// 求值。永不抛异常 —— 所有失败都通过 EvalResult::ok == false + error 返回。
    EvalResult evaluate(const EvalRequest& req) const;

    /// 运行期能力信息（前端据此禁用 QWORD 等按钮并显示原因）。
    static PlatformInfo platform_info();

    /// 本平台能否精确进行 64 位整数运算。
    static bool supports_64bit();

    /// 库实际提供的全部函数与常量名（用于前端的"函数参考"面板）。
    /// 内容直接来自 tinyexpr-plusplus 的
    /// list_available_functions_and_variables()，不做任何加工。
    static std::string available_functions_text();

    /// 库支持的按位运算最大数值（2^48−1）。
    static double max_bitops_value();
};

}  // namespace crosscalc
