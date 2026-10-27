/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zlink::samples::shoppingmall
{

class decimal_t
{
  public:
    decimal_t () = default;
    explicit decimal_t (std::string_view value) : _value (normalize (value)) {}

    const std::string &value () const noexcept { return _value; }

    static decimal_t from_wire (double value)
    {
        if (!std::isfinite (value) || value < 0)
            throw std::invalid_argument ("Decimal amount must be a non-negative JSON number");
        const auto minor = std::round (value * 100);
        if (minor > 9007199254740991.0 || minor / 100 != value)
            throw std::out_of_range ("Decimal amount must have at most two fractional digits");
        if (minor > max_minor_units)
            throw std::out_of_range ("Decimal amount exceeds the supported JSON number range");
        const auto units = static_cast<std::uint64_t> (minor);
        return decimal_t (std::to_string (units / 100) + "." + (units % 100 < 10 ? "0" : "")
                          + std::to_string (units % 100));
    }

    double to_wire_number () const
    {
        if (_value.front () == '-')
            throw std::invalid_argument ("Decimal amount must be non-negative");
        const auto point = _value.find ('.');
        const auto whole = _value.substr (0, point);
        const auto fraction = point == std::string::npos ? std::string{}
                                                         : _value.substr (point + 1);
        if (fraction.size () > 2 || whole.size () > 14)
            throw std::out_of_range ("Decimal amount exceeds the supported JSON number precision");
        const auto minor = std::stoull (whole) * 100
                           + (fraction.empty ()
                                ? 0
                                : std::stoull (fraction) * (fraction.size () == 1 ? 10 : 1));
        if (minor > max_minor_units)
            throw std::out_of_range ("Decimal amount exceeds the supported JSON number range");
        return static_cast<double> (minor) / 100;
    }

    friend bool operator== (const decimal_t &, const decimal_t &) = default;

  private:
    static constexpr std::uint64_t max_minor_units = (std::uint64_t{1} << 45) * 100 - 1;

    static std::string normalize (std::string_view value)
    {
        if (value.empty ()) {
            throw std::invalid_argument ("decimal value must not be empty");
        }
        bool negative = false;
        if (value.front () == '-' || value.front () == '+') {
            negative = value.front () == '-';
            value.remove_prefix (1);
        }
        const auto point = value.find ('.');
        auto integral = std::string (value.substr (0, point));
        auto fractional = point == std::string_view::npos ? std::string{}
                                                          : std::string (value.substr (point + 1));
        if (integral.empty () || (point != std::string_view::npos && fractional.empty ())
            || !std::all_of (integral.begin (),
                             integral.end (),
                             [] (unsigned char ch) { return std::isdigit (ch) != 0; })
            || !std::all_of (fractional.begin (), fractional.end (), [] (unsigned char ch) {
                   return std::isdigit (ch) != 0;
               })) {
            throw std::invalid_argument ("decimal value must use ordinary JSON number syntax");
        }
        const auto first_digit = integral.find_first_not_of ('0');
        integral = first_digit == std::string::npos ? "0" : integral.substr (first_digit);
        while (!fractional.empty () && fractional.back () == '0') {
            fractional.pop_back ();
        }
        const auto significant_digits = (integral == "0" ? std::size_t{0} : integral.size ())
                                        + fractional.size ();
        if (significant_digits > 29 || fractional.size () > 28) {
            throw std::out_of_range ("decimal value exceeds the .NET decimal range");
        }
        if (integral == "0" && fractional.empty ()) {
            negative = false;
        }
        return std::string (negative ? "-" : "") + integral
               + (fractional.empty () ? std::string{} : "." + fractional);
    }

    std::string _value{"0"};
};

}
