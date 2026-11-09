/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "encoding.hpp"
#include <charconv>
#include <set>
#include <stdexcept>

namespace zlink::framework::etcd::detail
{
[[noreturn]] void store_failure ()
{
    throw framework_exception_t (framework_error_kind_t::internal_failure, store_failure_message);
}

void validate_key (std::string_view value, bool prefix)
{
    if ((!prefix && value.empty ()) || value.size () > 1024 || !is_valid_utf8 (value))
        throw std::invalid_argument ("Invalid Store key.");
}

void validate_write (const store_write_request_t &request)
{
    std::set<std::string> conditions, mutations, all;
    std::size_t size = 0;
    for (const auto &condition : request.conditions) {
        std::visit (
          [&] (const auto &item) {
              validate_key (item.key.value);
              if (!conditions.insert (item.key.value).second)
                  throw std::invalid_argument ("Duplicate Store condition key.");
              all.insert (item.key.value);
              size += item.key.value.size ();
              using T = std::decay_t<decltype (item)>;
              if constexpr (std::is_same_v<T, store_version_condition_t>) {
                  if (item.expected.value.empty () || item.expected.value.size () > 4096
                      || !is_valid_utf8 (item.expected.value))
                      throw std::invalid_argument ("Invalid Store version length.");
                  size += item.expected.value.size ();
              } else if constexpr (std::is_same_v<T, store_value_condition_t>) {
                  if (item.expected.size () > max_payload)
                      throw std::invalid_argument ("Store value exceeds 1 MiB.");
                  size += item.expected.size ();
              }
          },
          condition);
    }
    for (const auto &mutation : request.mutations) {
        std::visit (
          [&] (const auto &item) {
              validate_key (item.key.value);
              if (!mutations.insert (item.key.value).second)
                  throw std::invalid_argument ("Duplicate Store mutation key.");
              all.insert (item.key.value);
              size += item.key.value.size ();
              if constexpr (std::is_same_v<std::decay_t<decltype (item)>, store_put_t>) {
                  if (item.bytes.size () > max_payload)
                      throw std::invalid_argument ("Store value exceeds 1 MiB.");
                  if (item.retention
                      && (item.retention->count () <= 0
                          || static_cast<std::uint64_t> (item.retention->count ()) > max_time))
                      throw std::invalid_argument ("Invalid Store retention.");
                  size += item.bytes.size ();
              }
          },
          mutation);
    }
    if (all.size () > 2048 || size > max_page_bytes)
        throw std::invalid_argument ("Store batch exceeds its bound.");
}

std::string physical_key (std::string_view ns, char kind, std::string_view key)
{
    std::string result (ns);
    result += '\0';
    result += kind;
    if (kind != 'c') {
        result += '\0';
        result += key;
    }
    return result;
}

std::string prefix_end (std::string value)
{
    for (auto i = value.size (); i > 0; --i) {
        const auto byte = static_cast<unsigned char> (value[i - 1]);
        if (byte != 255) {
            value[i - 1] = static_cast<char> (byte + 1);
            value.resize (i);
            return value;
        }
    }
    return std::string (1, '\0');
}

std::string encode_clock (std::uint64_t value)
{
    if (value > max_time)
        throw std::invalid_argument ("Store time exceeds its bound.");
    std::string bytes (8, '\0');
    for (unsigned i = 0; i < 8; ++i)
        bytes[7 - i] = static_cast<char> (value >> (i * 8));
    return bytes;
}

std::uint64_t decode_clock (std::string_view value)
{
    if (value.size () != 8)
        store_failure ();
    std::uint64_t result = 0;
    for (unsigned char byte : value)
        result = (result << 8) | byte;
    if (result > max_time)
        store_failure ();
    return result;
}

std::string encode_metadata (metadata_t value)
{
    if ((!value.has_expiry && value.expires_at != 0) || value.payload_length > max_payload)
        throw std::invalid_argument ("Invalid Store metadata.");
    std::string bytes;
    bytes += '\1';
    bytes += value.has_expiry ? '\1' : '\0';
    bytes += encode_clock (value.expires_at);
    for (int i = 3; i >= 0; --i)
        bytes += static_cast<char> (value.payload_length >> (i * 8));
    return bytes;
}

metadata_t decode_metadata (std::string_view value)
{
    if (value.size () != 14 || value[0] != 1 || static_cast<unsigned char> (value[1]) > 1)
        store_failure ();
    metadata_t result{value[1] == 1, decode_clock (value.substr (2, 8)), 0};
    for (unsigned char byte : value.substr (10))
        result.payload_length = (result.payload_length << 8) | byte;
    if ((!result.has_expiry && result.expires_at != 0) || result.payload_length > max_payload)
        store_failure ();
    return result;
}

std::string encode_version (std::uint64_t cluster, std::int64_t revision)
{
    if (revision <= 0)
        store_failure ();
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result = "e1:";
    for (int i = 15; i >= 0; --i)
        result += hex[(cluster >> (i * 4)) & 15];
    result += ':';
    char digits[20];
    const auto written = std::to_chars (digits, digits + sizeof (digits), revision);
    result.append (digits, written.ptr);
    return result;
}

std::string to_string (const std::vector<std::byte> &value)
{
    return {reinterpret_cast<const char *> (value.data ()), value.size ()};
}
std::vector<std::byte> to_bytes (std::string_view value)
{
    const auto *start = reinterpret_cast<const std::byte *> (value.data ());
    return {start, start + value.size ()};
}
std::chrono::system_clock::time_point from_ms (std::uint64_t value)
{
    if (value > max_time)
        store_failure ();
    return std::chrono::system_clock::time_point (std::chrono::milliseconds (value));
}
} // namespace zlink::framework::etcd::detail
