/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace zlink::framework
{

struct store_key_t
{
    std::string value;
};

struct store_version_t
{
    std::string value;
};

struct store_value_t
{
    std::vector<std::byte> bytes;
    store_version_t version;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::chrono::system_clock::time_point store_now{};
};

struct store_missing_t
{
    std::chrono::system_clock::time_point store_now{};
};

struct store_found_t
{
    store_value_t value;
};

using store_read_result_t = std::variant<store_missing_t, store_found_t>;

struct store_missing_condition_t
{
    store_key_t key;
};

struct store_version_condition_t
{
    store_key_t key;
    store_version_t expected;
};

using store_condition_t =
  std::variant<store_missing_condition_t, store_version_condition_t>;

struct store_put_t
{
    store_key_t key;
    std::vector<std::byte> bytes;
    std::optional<std::chrono::milliseconds> retention;
};

struct store_delete_t
{
    store_key_t key;
};

using store_mutation_t = std::variant<store_put_t, store_delete_t>;

struct store_write_request_t
{
    std::vector<store_condition_t> conditions;
    std::vector<store_mutation_t> mutations;
};

struct store_put_version_t
{
    store_key_t key;
    store_version_t version;
};

struct store_write_applied_t
{
    std::vector<store_put_version_t> put_versions;
    std::chrono::system_clock::time_point store_now{};
};

struct store_write_conflict_t
{
    std::chrono::system_clock::time_point store_now{};
};

using store_write_result_t =
  std::variant<store_write_applied_t, store_write_conflict_t>;

struct store_scan_cursor_t
{
    std::string value;
};

struct store_scan_request_t
{
    std::string prefix;
    std::optional<store_scan_cursor_t> cursor;
    std::uint32_t limit = 100;
};

struct store_scan_item_t
{
    store_key_t key;
    store_value_t value;
};

struct store_scan_page_t
{
    std::vector<store_scan_item_t> items;
    std::optional<store_scan_cursor_t> next_cursor;
    std::chrono::system_clock::time_point store_now{};
};

struct store_scan_expired_t
{
};

using store_scan_result_t =
  std::variant<store_scan_page_t, store_scan_expired_t>;

class location_store_t
{
  public:
    virtual ~location_store_t () = default;

    virtual task_t<store_read_result_t> read (store_key_t key) = 0;
    virtual task_t<store_write_result_t> write (
      store_write_request_t request) = 0;
    virtual task_t<store_scan_result_t> scan (
      store_scan_request_t request) = 0;
};

struct blob_reference_t
{
    std::string value;
};

struct blob_stored_t
{
    std::chrono::system_clock::time_point expires_at{};
    std::chrono::system_clock::time_point store_now{};
};

struct blob_already_stored_t
{
    std::chrono::system_clock::time_point expires_at{};
    std::chrono::system_clock::time_point store_now{};
};

struct blob_conflict_t
{
    std::chrono::system_clock::time_point store_now{};
};

using blob_put_result_t =
  std::variant<blob_stored_t, blob_already_stored_t, blob_conflict_t>;

struct blob_missing_t
{
    std::chrono::system_clock::time_point store_now{};
};

struct blob_found_t
{
    std::vector<std::byte> bytes;
    std::chrono::system_clock::time_point expires_at{};
    std::chrono::system_clock::time_point store_now{};
};

using blob_read_result_t = std::variant<blob_missing_t, blob_found_t>;

struct blob_renewed_t
{
    std::chrono::system_clock::time_point expires_at{};
    std::chrono::system_clock::time_point store_now{};
};

using blob_renew_result_t = std::variant<blob_missing_t, blob_renewed_t>;

class relocation_store_t
{
  public:
    virtual ~relocation_store_t () = default;

    virtual task_t<blob_put_result_t> put (
      blob_reference_t reference,
      std::span<const std::byte> payload,
      std::chrono::milliseconds retention) = 0;
    virtual task_t<blob_read_result_t> read (
      blob_reference_t reference) = 0;
    virtual task_t<blob_renew_result_t> renew (
      blob_reference_t reference,
      std::chrono::milliseconds retention) = 0;
    virtual task_t<void> erase (blob_reference_t reference) = 0;
};

} // namespace zlink::framework
