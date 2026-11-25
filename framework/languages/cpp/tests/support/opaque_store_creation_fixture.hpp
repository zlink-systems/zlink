/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "opaque_store_record_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <functional>

namespace zlink::framework::test::opaque_store
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;

class creation_terminal_failure_store_t final : public location_store_t
{
  public:
    enum class fault_t
    {
        none,
        before_commit,
        capacity_conflict_once,
        reserve_counter_conflict,
        concurrent_terminal,
        after_commit,
        between_writes
    };

    task_t<store_read_result_t> read (store_key_t key) override
    {
        const auto terminal_key = key.value.starts_with (std::string ("creation-terminal") + '\0');
        auto result = co_await inner.read (std::move (key));
        if (terminal_key) {
            if (const auto *missing = std::get_if<store_missing_t> (&result))
                terminal_read_store_now = missing->store_now;
        }
        co_return result;
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        creation_writes.push_back (request);
        if (before_next_write) {
            auto action = std::exchange (before_next_write, {});
            co_await action ();
        }
        for (const auto &mutation : request.mutations) {
            const auto *put = std::get_if<store_put_t> (&mutation);
            if (!put || !put->key.value.starts_with (std::string ("authority") + '\0'))
                continue;
            const auto authority = nlohmann::json::parse (put->bytes.begin (), put->bytes.end ());
            const auto &pending = authority.at (location_record_fields::pendingCreation);
            if (!pending.is_null ())
                reservation_attempts.push_back (
                  pending.at (location_record_fields::reservationId).get<std::string> ());
        }
        if (fault == fault_t::reserve_counter_conflict) {
            fault = fault_t::none;
            const auto counter = std::get<store_put_t> (request.mutations.at (1));
            store_write_request_t advance{{request.conditions.at (1)}, {counter}};
            (void) co_await inner.write (std::move (advance));
            co_return store_write_result_t{
              store_write_conflict_t{std::chrono::system_clock::now ()}};
        }
        if (fault == fault_t::none) {
            const auto applied = co_await inner.write (request);
            if (attempted) {
                attempted = std::move (request);
                if (const auto *written = std::get_if<store_write_applied_t> (&applied))
                    terminal_write_store_now = written->store_now;
            }
            co_return applied;
        }
        ++writes;
        attempted = request;
        if (fault == fault_t::concurrent_terminal) {
            auto terminal = std::get<store_put_t> (request.mutations.back ());
            terminal.bytes = bytes ("winning-terminal");
            store_write_request_t publish{{store_missing_condition_t{terminal.key}}, {terminal}};
            (void) co_await inner.write (std::move (publish));
            co_return store_write_result_t{
              store_write_conflict_t{std::chrono::system_clock::now ()}};
        }
        if (fault == fault_t::before_commit) {
            const auto authority_key = std::visit (
              [] (const auto &mutation) { return mutation.key; }, request.mutations.front ());
            const auto row = co_await inner.read (authority_key);
            const auto &found = std::get<store_found_t> (row);
            store_write_request_t change{
              {store_version_condition_t{authority_key, found.value.version}},
              {store_put_t{authority_key, found.value.bytes, std::nullopt}}};
            (void) co_await inner.write (std::move (change));
            co_return store_write_result_t{
              store_write_conflict_t{std::chrono::system_clock::now ()}};
        }
        if (fault == fault_t::capacity_conflict_once) {
            if (std::none_of (request.conditions.begin (), request.conditions.end (),
                              [this] (const auto &condition) {
                                  bool matches = false;
                                  runtime::for_each_condition_key (
                                    condition, [&] (const auto &key) {
                                        matches |= key.value == conflicting_capacity_key.value;
                                    });
                                  return matches;
                              }))
                co_return co_await inner.write (std::move (request));
            fault = fault_t::none;
            const auto row = co_await inner.read (conflicting_capacity_key);
            const auto &found = std::get<store_found_t> (row);
            store_write_request_t change{
              {store_version_condition_t{conflicting_capacity_key, found.value.version}},
              {store_put_t{conflicting_capacity_key, found.value.bytes, std::nullopt}}};
            (void) co_await inner.write (std::move (change));
            co_return store_write_result_t{
              store_write_conflict_t{std::chrono::system_clock::now ()}};
        }
        if (fault == fault_t::between_writes && writes > 1)
            co_return result_t<store_write_result_t>::failure (
              framework_error_kind_t::internal_failure, "provider failed between writes");
        auto applied = co_await inner.write (std::move (request));
        if (const auto *written = std::get_if<store_write_applied_t> (&applied))
            terminal_write_store_now = written->store_now;
        if (fault == fault_t::after_commit) {
            co_return result_t<store_write_result_t>::failure (
              framework_error_kind_t::internal_failure, "provider lost the atomic write reply");
        }
        co_return applied;
    }

    std::vector<store_write_request_t> creation_writes;
    std::function<task_t<void> ()> before_next_write;
    in_memory_location_store_t inner;
    fault_t fault = fault_t::none;
    unsigned writes = 0;
    std::vector<std::string> reservation_attempts;
    store_key_t conflicting_capacity_key;
    std::optional<store_write_request_t> attempted;
    std::optional<std::chrono::system_clock::time_point> terminal_read_store_now;
    std::optional<std::chrono::system_clock::time_point> terminal_write_store_now;
};
} // namespace zlink::framework::test::opaque_store
