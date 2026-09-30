// ---------------------------------------------------------------------------
//  枚举 <-> 字符串（JSON API 与前端都用得到）
// ---------------------------------------------------------------------------
#include "core/calc_types.h"

#include <algorithm>
#include <cctype>

namespace crosscalc {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

}  // namespace

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Standard:
            return "standard";
        case Mode::Scientific:
            return "scientific";
        case Mode::Programmer:
            return "programmer";
    }
    return "standard";
}

const char* base_name(NumBase b) {
    switch (b) {
        case NumBase::Bin:
            return "bin";
        case NumBase::Oct:
            return "oct";
        case NumBase::Dec:
            return "dec";
        case NumBase::Hex:
            return "hex";
    }
    return "dec";
}

bool parse_mode(std::string_view s, Mode& out) {
    const std::string v = lower(s);
    if (v == "standard") {
        out = Mode::Standard;
        return true;
    }
    if (v == "scientific") {
        out = Mode::Scientific;
        return true;
    }
    if (v == "programmer") {
        out = Mode::Programmer;
        return true;
    }
    return false;
}

bool parse_base(std::string_view s, NumBase& out) {
    const std::string v = lower(s);
    if (v == "bin" || v == "2") {
        out = NumBase::Bin;
        return true;
    }
    if (v == "oct" || v == "8") {
        out = NumBase::Oct;
        return true;
    }
    if (v == "dec" || v == "10") {
        out = NumBase::Dec;
        return true;
    }
    if (v == "hex" || v == "16") {
        out = NumBase::Hex;
        return true;
    }
    return false;
}

bool parse_word_size(std::string_view s, WordSize& out) {
    const std::string v = lower(s);
    if (v == "8" || v == "byte") {
        out = WordSize::Bits8;
        return true;
    }
    if (v == "16" || v == "word") {
        out = WordSize::Bits16;
        return true;
    }
    if (v == "32" || v == "dword") {
        out = WordSize::Bits32;
        return true;
    }
    if (v == "64" || v == "qword") {
        out = WordSize::Bits64;
        return true;
    }
    return false;
}

}  // namespace crosscalc
