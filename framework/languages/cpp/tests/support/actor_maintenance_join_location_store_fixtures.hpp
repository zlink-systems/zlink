/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "actor_maintenance_join_observations.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <array>
#include <thread>

namespace zlink::framework::test::actor_maintenance_join
{
// A Store write is visible before its caller receives the result. Exercise that
// public provider boundary without changing the relocation runtime or its lanes.
class observed_location_store_t final : public fw::location_store_t
{
  public:
    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override;
    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override;
    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override;
    void after_next_read (std::string key, std::function<fw::task_t<void> ()> action);
    void observe (std::function<fw::task_t<void> ()> after_write);
    void before_next_write_when (
      std::function<fw::task_t<bool> (const fw::store_write_request_t &)> ready,
      std::function<fw::task_t<void> ()> action);
    void before_scan_when (std::function<bool ()> ready, std::function<fw::task_t<void> ()> action);
    bool intercepted_write_conflicted ();
    void observe_authority_reads (const std::string &authority_key);
    std::pair<std::size_t, std::size_t> accepted_store_calls ();

  private:
    void count_accepted_read (const fw::store_key_t &key);
    void count_accepted_scan ();
    fw::runtime::in_memory_location_store_t _inner;
    std::mutex _mutex;
    std::string _held_read_key;
    std::function<fw::task_t<void> ()> _after_read;
    std::function<fw::task_t<void> ()> _after_write;
    std::function<fw::task_t<bool> (const fw::store_write_request_t &)> _before_write_ready;
    std::function<fw::task_t<void> ()> _before_write;
    std::function<bool ()> _scan_ready;
    std::function<fw::task_t<void> ()> _before_scan;
    bool _intercepted_write_conflicted = false;
    std::string _observed_authority_key;
    std::optional<std::thread::id> _acceptance_thread;
    std::size_t _acceptance_reads = 0;
    std::size_t _acceptance_scans = 0;
};

// Trap an accidental accepted-source Store lookup while using the production resolver.
class intake_store_t final : public fw::location_store_t
{
  public:
    intake_store_t ();
    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override;
    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override;
    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override;
    void observe (std::size_t index);
    void release (int index, bool failure);
    void release_all ();
    ~intake_store_t () override;
    std::string authority_store_key;
    std::string *capture_key = nullptr;
    std::atomic<int> stage{0};
    std::array<std::atomic_bool, 3> reached{};
    const std::thread::id releaser = std::this_thread::get_id ();
    std::optional<fw::store_read_result_t> authority_value, lease_value;
    std::optional<fw::store_scan_result_t> nodes_value;
    fw::task_completion_source_t<fw::store_read_result_t> authority_source, lease_source;
    fw::task_completion_source_t<fw::store_scan_result_t> nodes_source;
    fw::runtime::in_memory_location_store_t inner;
};
} // namespace zlink::framework::test::actor_maintenance_join
