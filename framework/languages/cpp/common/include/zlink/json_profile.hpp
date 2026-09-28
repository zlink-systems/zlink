/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define ZLINK_HAS_EXCEPTIONS 1
#else
#define ZLINK_HAS_EXCEPTIONS 0
#endif

namespace zlink::detail::json_profile
{

inline bool has_finite_numbers (const nlohmann::json &value)
{
    if (value.is_number_float () && !std::isfinite (value.get<double> ())) {
        return false;
    }
    if (value.is_array ()) {
        for (const auto &element : value)
            if (!has_finite_numbers (element))
                return false;
    }
    if (value.is_object ()) {
        for (const auto &[_, element] : value.items ())
            if (!has_finite_numbers (element))
                return false;
    }
    return true;
}

template <typename TIterator>
std::optional<nlohmann::json>
try_parse (TIterator begin, TIterator end, std::string *error = nullptr)
{
    const auto fail = [error] (const char *message) -> std::optional<nlohmann::json> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (begin != end) {
        auto cursor = begin;
        const auto first = static_cast<unsigned char> (*cursor++);
        if (cursor != end) {
            const auto second = static_cast<unsigned char> (*cursor++);
            if (cursor != end) {
                const auto third = static_cast<unsigned char> (*cursor);
                if (first == 0xef && second == 0xbb && third == 0xbf) {
                    return fail ("framework-json-v1 rejects a UTF-8 BOM");
                }
            }
        }
    }

    std::map<int, std::unordered_set<std::string>> object_keys;
    bool duplicate_key = false;
    auto reject_duplicate_keys = [&object_keys,
                                  &duplicate_key] (int depth, nlohmann::json::parse_event_t event,
                                                   nlohmann::json &parsed) {
        if (event == nlohmann::json::parse_event_t::object_start) {
            object_keys[depth + 1].clear ();
        } else if (event == nlohmann::json::parse_event_t::key) {
            auto &keys = object_keys[depth];
            if (!keys.insert (parsed.get<std::string> ()).second) {
                duplicate_key = true;
                return false;
            }
        } else if (event == nlohmann::json::parse_event_t::object_end) {
            object_keys.erase (depth + 1);
        }
        return true;
    };
    auto parsed = nlohmann::json::parse (begin, end, reject_duplicate_keys, false);
    if (duplicate_key)
        return fail ("framework-json-v1 rejects duplicate properties");
    if (parsed.is_discarded ())
        return fail ("framework-json-v1 rejects invalid JSON");
    if (!has_finite_numbers (parsed))
        return fail ("framework-json-v1 rejects non-finite numbers");
    return parsed;
}

#if ZLINK_HAS_EXCEPTIONS
template <typename TIterator> nlohmann::json parse (TIterator begin, TIterator end)
{
    std::string error;
    auto parsed = try_parse (begin, end, &error);
    if (!parsed)
        throw std::invalid_argument (error);
    return std::move (*parsed);
}
#endif

inline std::string dump (const nlohmann::json &value)
{
    if (!has_finite_numbers (value)) {
#if ZLINK_HAS_EXCEPTIONS
        throw std::invalid_argument ("framework-json-v1 rejects non-finite numbers");
#else
        return {};
#endif
    }
    return value.dump ();
}

} // namespace zlink::detail::json_profile
