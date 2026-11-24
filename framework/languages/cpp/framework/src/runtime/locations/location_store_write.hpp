/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "location_store_limits.hpp"

#include <zlink/framework/contracts/locations/stores.hpp>

#include <charconv>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>

namespace zlink::framework::runtime
{
template <typename Visit>
void for_each_condition_key (const store_condition_t &condition, Visit visit)
{
    std::visit (
      [&] (const auto &item) {
          if constexpr (std::is_same_v<std::decay_t<decltype (item)>,
                                       store_sum_at_most_condition_t>) {
              for (const auto &key : item.keys)
                  visit (key);
          } else
              visit (item.key);
      },
      condition);
}

inline std::size_t counter_operand_size (std::int64_t value)
{
    char digits[std::numeric_limits<std::int64_t>::digits10 + 2];
    return static_cast<std::size_t> (std::to_chars (digits, digits + sizeof digits, value).ptr
                                     - digits);
}

template <typename ValidateKey>
void validate_location_write (
  const store_write_request_t &request,
  ValidateKey validate_key,
  bool (*valid_version) (std::string_view) = nullptr,
  std::uint64_t max_retention = std::numeric_limits<std::int64_t>::max ())
{
    constexpr unsigned condition_key = 1;
    constexpr unsigned mutation_key = 2;
    std::map<std::string_view, unsigned> keys;
    std::size_t size = 0;
    const auto count_key = [&] (const store_key_t &key, unsigned role) {
        validate_key (key.value);
        auto &seen = keys[key.value];
        if (seen & role)
            throw std::invalid_argument ("Location write repeats a key in conditions or mutations");
        seen |= role;
        size += key.value.size ();
    };
    for (const auto &condition : request.conditions) {
        for_each_condition_key (condition,
                                [&] (const auto &key) { count_key (key, condition_key); });
        if (const auto *version = std::get_if<store_version_condition_t> (&condition)) {
            const auto &value = version->expected.value;
            if (value.empty () || value.size () > 4096 || (valid_version && !valid_version (value)))
                throw std::invalid_argument ("Invalid location version condition");
            size += value.size ();
        } else if (const auto *value = std::get_if<store_value_condition_t> (&condition)) {
            if (value->expected.size () > location_store_limits::value_bytes)
                throw std::invalid_argument ("Location value condition exceeds 1 MiB");
            size += value->expected.size ();
        } else if (const auto *sum = std::get_if<store_sum_at_most_condition_t> (&condition)) {
            size += counter_operand_size (sum->delta) + counter_operand_size (sum->limit);
        }
    }
    for (const auto &mutation : request.mutations) {
        std::visit ([&] (const auto &item) { count_key (item.key, mutation_key); }, mutation);
        if (const auto *put = std::get_if<store_put_t> (&mutation)) {
            if (put->bytes.size () > location_store_limits::value_bytes)
                throw std::invalid_argument ("Location value exceeds 1 MiB");
            if (put->retention
                && (put->retention->count () <= 0
                    || static_cast<std::uint64_t> (put->retention->count ()) > max_retention))
                throw std::invalid_argument ("Invalid location retention");
            size += put->bytes.size ();
        } else if (const auto *add = std::get_if<store_add_t> (&mutation)) {
            size += counter_operand_size (add->delta);
        }
    }
    if (keys.size () > location_store_limits::write_keys)
        throw std::invalid_argument ("Location write exceeds 2048 unique keys");
    if (size > location_store_limits::write_bytes)
        throw std::invalid_argument ("Location write exceeds 4 MiB");
}

inline constexpr auto location_counter_max = std::numeric_limits<std::int64_t>::max ();
inline constexpr std::size_t location_counter_digits = 19;

inline std::optional<std::int64_t> parse_location_counter (std::string_view bytes)
{
    if (bytes.empty () || bytes.size () > location_counter_digits || bytes.front () < '0'
        || bytes.front () > '9' || (bytes.size () > 1 && bytes.front () == '0'))
        return std::nullopt;
    std::int64_t value;
    const auto end = bytes.data () + bytes.size ();
    const auto parsed = std::from_chars (bytes.data (), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        return std::nullopt;
    return value;
}

enum class location_sum_result_t
{
    matches,
    exceeds,
    corrupt
};

template <typename ReadCounter>
location_sum_result_t evaluate_location_sum (const store_sum_at_most_condition_t &condition,
                                             ReadCounter read_counter)
{
    bool fits = condition.delta <= condition.limit;
    // A nonnegative difference between two int64 values fits in uint64.
    auto budget =
      static_cast<std::uint64_t> (condition.limit) - static_cast<std::uint64_t> (condition.delta);
    for (const auto &key : condition.keys) {
        const auto value = read_counter (key);
        if (!value)
            return location_sum_result_t::corrupt;
        const auto count = static_cast<std::uint64_t> (*value);
        if (fits && count <= budget)
            budget -= count;
        else
            fits = false;
    }
    return fits ? location_sum_result_t::matches : location_sum_result_t::exceeds;
}

inline std::optional<store_put_t> prepare_location_add (const store_add_t &add,
                                                        std::optional<std::int64_t> value)
{
    if (!value || add.delta < -*value || add.delta > location_counter_max - *value)
        return std::nullopt;
    const auto text = std::to_string (*value + add.delta);
    const auto bytes = std::as_bytes (std::span (text));
    return store_put_t{add.key, {bytes.begin (), bytes.end ()}, std::nullopt};
}
}
