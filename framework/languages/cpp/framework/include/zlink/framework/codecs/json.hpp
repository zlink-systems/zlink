/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace zlink::framework::codecs::json
{

namespace detail
{

inline void validate_finite_numbers (const nlohmann::json &value)
{
    if (value.is_number_float () && !std::isfinite (value.get<double> ())) {
        throw std::invalid_argument ("framework-json-v1 rejects non-finite numbers");
    }
    if (value.is_array ()) {
        for (const auto &element : value)
            validate_finite_numbers (element);
        return;
    }
    if (value.is_object ()) {
        for (const auto &[_, element] : value.items ())
            validate_finite_numbers (element);
    }
}

template <typename TIterator>
nlohmann::json parse_profile (TIterator begin, TIterator end)
{
    if (begin != end) {
        auto cursor = begin;
        const auto first = static_cast<unsigned char> (*cursor++);
        if (cursor != end) {
            const auto second = static_cast<unsigned char> (*cursor++);
            if (cursor != end) {
                const auto third = static_cast<unsigned char> (*cursor);
                if (first == 0xef && second == 0xbb && third == 0xbf) {
                    throw std::invalid_argument (
                      "framework-json-v1 rejects a UTF-8 BOM");
                }
            }
        }
    }

    std::map<int, std::unordered_set<std::string>> object_keys;
    auto reject_duplicate_keys =
      [&object_keys] (int depth, nlohmann::json::parse_event_t event,
                      nlohmann::json &parsed) {
          if (event == nlohmann::json::parse_event_t::object_start) {
              object_keys[depth + 1].clear ();
          }
          else if (event == nlohmann::json::parse_event_t::key) {
              auto &keys = object_keys[depth];
              if (!keys.insert (parsed.get<std::string> ()).second) {
                  throw std::invalid_argument (
                    "framework-json-v1 rejects duplicate properties");
              }
          }
          else if (event == nlohmann::json::parse_event_t::object_end) {
              object_keys.erase (depth + 1);
          }
          return true;
      };
    auto parsed = nlohmann::json::parse (begin, end, reject_duplicate_keys);
    validate_finite_numbers (parsed);
    return parsed;
}

inline std::string dump_profile (const nlohmann::json &value)
{
    validate_finite_numbers (value);
    return value.dump ();
}

} // namespace detail

template <typename T> T parse_message (const message_t &message)
{
    const auto *begin = reinterpret_cast<const char *> (message.data ());
    const auto *end = begin ? begin + message.size () : begin;
    return detail::parse_profile (begin, end).template get<T> ();
}

template <typename T> message_t make_message (const T &value)
{
    const auto json = nlohmann::json (value);
    const auto text = detail::dump_profile (json);
    return message_t::from (std::as_bytes (std::span<const char> (text.data (), text.size ())));
}

} // namespace zlink::framework::codecs::json

namespace zlink
{

template <typename T> message_t message_t::from_json (const T &value_)
{
    return framework::codecs::json::make_message (value_);
}

template <typename T> T message_t::parse_json () const
{
    return framework::codecs::json::parse_message<T> (*this);
}

} // namespace zlink
