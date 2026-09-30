// ---------------------------------------------------------------------------
//  crosscalc —— 数字的"展示层"格式化
//
//  重要说明（对应需求"尽量使用该库的计算功能，不要自己编写计算函数"）：
//  本文件里没有任何数学计算。它只做"把引擎算出来的数值变成给人看的字符串"，
//  也就是 Windows 计算器里"HEX / DEC / OCT / BIN 显示"那一层。
//  tinyexpr-plusplus 只提供单向的 HEX2DEC()/BIN2DEC()/OCT2DEC()（字符串->数值），
//  没有反向的 DEC2HEX() 之类，所以进制显示必须由展示层完成。
//
//  这里刻意不使用 std::format：CI 的 ubuntu-22.04 上是 GCC 11，
//  标准库还没有 <format>（GCC 13 才提供），为了双平台可编译，统一用 snprintf。
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

#include "core/calc_types.h"

namespace crosscalc {

/// 按"最短且能唯一还原"的精度格式化一般数值（标准 / 科学模式用）。
/// 处理 NaN / ±∞ / -0。
std::string format_general(double v);

/// 把无符号整数按指定进制渲染成字符串。
/// @param group_binary 二进制时每 4 位加一个空格（便于数位）。
std::string format_uint_in_base(uint64_t v, NumBase base, bool group_binary);

/// 把数值按字长截断到 word size 位（二补数语义）。
uint64_t mask_to_word(uint64_t v, WordSize ws);

/// 把已按字长截断的无符号位模式按二补数解释为有符号值。
int64_t sign_extend(uint64_t v, WordSize ws);

/// 程序员模式的显示：按进制与字长渲染。
/// 非整数结果无法按位显示，此时退化为一般十进制显示。
/// @note 数值会先被截断到字长范围内（Windows 计算器在 QWORD/DWORD 等
///     字长下就是这个行为）。
std::string format_programmer(double v, NumBase base, WordSize ws);

/// 数值是否为整数（在 double 可精确表达的范围内）。
bool is_integral_value(double v);

}  // namespace crosscalc
