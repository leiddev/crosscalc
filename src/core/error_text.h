// ---------------------------------------------------------------------------
//  crosscalc —— 错误提示的"翻译层"
//
//  为什么需要这一层：
//  tinyexpr-plusplus 的错误分两种，任何一种都不能直接丢给用户看 ——
//
//   1) 计算期错误（除零、负数开方、按位运算参数非法……）
//      库用 throw std::runtime_error("Division by zero.") 抛出，消息是英文。
//
//   2) 语法 / 符号解析错误
//      库只通过 get_last_error_position() 给出一个下标，
//      get_last_error_message() 在绝大多数情况下返回【空字符串】。
//      例如 "2+"  ->  位置 1、消息 ""
//           "abc+1" -> 位置 3、消息 ""
//      所以这里必须根据"出错位置 + 上下文"自己合成一句人话。
//
//  本文件不含任何计算逻辑，只做文本映射。
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <string_view>

#include "core/calc_types.h"

namespace crosscalc {

/// 计算期错误（库的英文异常消息）-> 中文友好提示。
std::string friendly_calc_error(std::string_view lib_message, Mode mode);

/// 语法/解析错误 -> 中文友好提示。
/// @param lib_message 库给的错误消息，通常是空的
/// @param pos         库给的出错下标（-1 表示无信息）
/// @param expr        原始表达式，用于从上下文推断原因
std::string friendly_syntax_error(std::string_view lib_message, int pos,
                                  std::string_view expr);

/// 结果无效（库返回 NaN 且没有报错）时的提示。
/// 典型触发：HEX2DEC("ZZ")、参数个数不匹配 —— 库不认为这是错误，只返回 NaN。
std::string friendly_invalid_result(std::string_view expr, Mode mode);

/// 定位表达式 [pos] 处的 token 边界，供前端高亮。
/// @param out_start 输出 token 起始下标（-1 表示没找到）
/// @param out_len   输出 token 长度
void token_span_at(std::string_view expr, int pos, int& out_start, int& out_len);

/// 去掉字符串两端的空白。
std::string_view trim_view(std::string_view s);

}  // namespace crosscalc
