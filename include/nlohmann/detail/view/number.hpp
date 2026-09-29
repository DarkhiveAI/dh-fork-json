//     __ _____ _____ _____
//  __|  |   __|     |   | |  JSON for Modern C++
// |  |  |__   |  |  | | | |  version 3.12.0
// |_____|_____|_____|_|___|  https://github.com/nlohmann/json
//
// SPDX-FileCopyrightText: 2013-2026 Niels Lohmann <https://nlohmann.me>
// SPDX-License-Identifier: MIT

#pragma once

#include <array> // array
#include <cfloat> // FLT_EVAL_METHOD
#include <cstddef> // size_t
#include <cstdint> // int64_t, uint64_t
#include <cstring> // memcpy
#include <limits> // numeric_limits
#include <string> // string
#include <type_traits> // integral_constant, is_same

#include <nlohmann/json.hpp>
#include <nlohmann/detail/view/document_data.hpp>
#include <nlohmann/detail/view/macro_scope.hpp>
#include <nlohmann/detail/view/node.hpp>
#include <nlohmann/detail/view/scan.hpp>

NLOHMANN_JSON_NAMESPACE_BEGIN
namespace detail
{
namespace view
{

/*!
@brief locate the decimal point and the end of the mantissa of a float token

Also checks that the token is a JSON number. Tokens of the parser and of edits
always are; an image loaded with image_check::bounds can hold any bytes, which
must not reach the conversion (it expects a well-formed token).
*/
inline bool float_token_layout(const char* first, const char* last, std::size_t& dot, std::size_t& mantissa_end) noexcept
{
    const auto digit = [last](const char* q)
    {
        return q != last && is_digit(static_cast<unsigned char>(*q));
    };
    const char* p = first;
    p += (p != last && *p == '-') ? 1 : 0;
    if (!digit(p) || (*p == '0' && digit(p + 1)))
    {
        return false;
    }
    while (digit(p))
    {
        ++p;
    }
    dot = std::string::npos;
    if (p != last && *p == '.')
    {
        dot = static_cast<std::size_t>(p - first);
        if (!digit(++p))
        {
            return false;
        }
        while (digit(p))
        {
            ++p;
        }
    }
    mantissa_end = static_cast<std::size_t>(p - first);
    if (p != last && (*p == 'e' || *p == 'E'))
    {
        ++p;
        p += (p != last && (*p == '+' || *p == '-')) ? 1 : 0;
        if (!digit(p))
        {
            return false;
        }
        while (digit(p))
        {
            ++p;
        }
    }
    return p == last;
}

/*!
@brief the value of the float token of a node, as parse() converts it

Uses the lexer's conversion (detail::convert_float_fast, then the locale-aware
strtod fallback), so that the values are bit-identical to parse(). A token
that is not a JSON number (only in a damaged image loaded with
image_check::bounds) yields 0.
*/
template<typename FloatType>
NLOHMANN_VIEW_NOINLINE FloatType float_value(const char* first, const node& n)
{
    const char* const last = first + n.len;
    std::size_t dot = 0;
    std::size_t mantissa_end = 0;
    if (NLOHMANN_VIEW_UNLIKELY(!float_token_layout(first, last, dot, mantissa_end)))
    {
        return FloatType{};
    }
    FloatType v{};
    if (!convert_float_fast(first, last, dot, mantissa_end, v))
    {
        std::string token(first, last);
        convert_float_locale_aware(token, dot, v);
    }
    return v;
}

/// the significand (at most 19 digits) and the decimal exponent of a float token
struct token_decimal
{
    std::uint64_t w;
    std::int64_t q;
    bool negative;
};

/*!
@brief the digits of a float token with at most 19 digits, from its layout

The digit layout recorded while parsing says where the integer digits, the
fraction digits, and the exponent are, so the digits are read eight at a
time without scanning.

@param[in] p  first character of the token
@param[in] e  end of the token
@param[in] limit  end of the readable memory (the source text)
*/
NLOHMANN_VIEW_ALWAYS_INLINE token_decimal layout_decimal(const unsigned char* p, const unsigned char* e, unsigned int_digits, unsigned frac_digits, const unsigned char* limit) noexcept
{
    const bool negative = *p == '-';
    p += negative ? 1 : 0;
    std::uint64_t w = parse_upto19(p, int_digits, limit);
    p += int_digits;
    std::int64_t q = 0;
    if (frac_digits != 0)
    {
        w = (w * int_pow10(frac_digits)) + parse_upto19(p + 1, frac_digits, limit);
        p += 1 + frac_digits;
        q = -static_cast<std::int64_t>(frac_digits);
    }
    if (p != e)
    {
        // [eE][+-]digits; huge exponents saturate (the parser rejected
        // overflow). The token is not read beyond e, and the digits are taken
        // as unsigned, so that a token that is not well-formed (a damaged
        // image loaded with image_check::bounds) yields a wrong value, but no
        // overflow.
        ++p;
        const bool exp_negative = p != e && *p == '-';
        p += (p != e && (*p == '-' || *p == '+')) ? 1 : 0;
        std::int64_t exp_value = 0;
        for (; p != e; ++p)
        {
            if (exp_value < 0x10000000)
            {
                exp_value = (exp_value * 10) + static_cast<unsigned char>(*p - '0');
            }
        }
        q += exp_negative ? -exp_value : exp_value;
    }

    return token_decimal{w, q, negative};
}

/*!
@brief the double of the digits of a float token (at most 19 digits)

The result is correctly rounded (Clinger's fast path where both operands are
exact, else the Eisel-Lemire algorithm, which needs no fallback for up to 19
digits), so it is the value parse() produces.
*/
NLOHMANN_VIEW_ALWAYS_INLINE double decimal_to_double(const token_decimal& d) noexcept
{
    const std::uint64_t w = d.w;
    const std::int64_t q = d.q;
    const bool negative = d.negative;
    double result = 0;
    if (w != 0)
    {
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD == 0
        static const std::array<double, 23> pow10 = {{1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22}};
        if (q >= -22 && q <= 22 && w <= (std::uint64_t{1} << 53))
        {
            // Clinger's fast path: both operands exact, one rounding
            result = static_cast<double>(w);
            result = q < 0 ? result / pow10[static_cast<std::size_t>(-q)] : result * pow10[static_cast<std::size_t>(q)];
            return negative ? -result : result;
        }
#endif
        const std::uint64_t bits = eisel_lemire(q, w);
        std::memcpy(&result, &bits, sizeof(result));
    }
    return negative ? -result : result;
}

/// the double of a float token with at most 19 digits, from its layout
NLOHMANN_VIEW_ALWAYS_INLINE double layout_double(const unsigned char* p, const unsigned char* e, unsigned int_digits, unsigned frac_digits, const unsigned char* limit) noexcept
{
    return decimal_to_double(layout_decimal(p, e, int_digits, frac_digits, limit));
}

/// the value of a float set by an edit: its token (the shortest round-trip
/// text, or "nan", "inf", "-inf") in the edit arena
template<typename FloatType>
NLOHMANN_VIEW_NOINLINE FloatType edited_float(const char* token, const node& n)
{
    if (token[0] == 'n')
    {
        return std::numeric_limits<FloatType>::quiet_NaN();
    }
    if (token[0] == 'i' || (token[0] == '-' && token[1] == 'i'))
    {
        return token[0] == 'i' ? std::numeric_limits<FloatType>::infinity() : -std::numeric_limits<FloatType>::infinity();
    }
    return float_value<FloatType>(token, n);
}

/// the value of the float token of a node, as parse() converts it; doubles
/// with at most 19 digits are converted from the digit layout
template<typename FloatType>
FloatType float_value(const document_data& d, const node& n)
{
    if (NLOHMANN_VIEW_UNLIKELY((n.flags & node_flags::storage) == node_flags::edited))
    {
        return edited_float<FloatType>(d.str(n), n);
    }
    return float_value<FloatType>(d, n, std::is_same<FloatType, double> {});
}

template<typename FloatType>
FloatType float_value(const document_data& d, const node& n, std::true_type /*double*/)
{
    const unsigned int_digits = n.extra & 0xFFu;
    const unsigned frac_digits = n.extra >> 8u;
    if (NLOHMANN_VIEW_LIKELY(int_digits + frac_digits <= 19)) // (255 marks "many")
    {
        // (a float token not written by an edit is in the text)
        const auto* const first = reinterpret_cast<const unsigned char*>(d.src + n.off); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        return layout_double(first, first + n.len, int_digits, frac_digits, reinterpret_cast<const unsigned char*>(d.src + d.size)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }
    return float_value<FloatType>(d.str(n), n);
}

template<typename FloatType>
FloatType float_value(const document_data& d, const node& n, std::false_type /*other*/)
{
    return float_value<FloatType>(d.str(n), n);
}

}  // namespace view
}  // namespace detail
NLOHMANN_JSON_NAMESPACE_END
