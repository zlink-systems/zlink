/* SPDX-License-Identifier: Elastic-2.0 */

#include "actor_maintenance_join_location_store_fixtures.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/provider_location_repository.hpp"

namespace zlink::framework::test::actor_maintenance_join
{
fw::task_t<fw::store_read_result_t> observed_location_store_t::read (fw::store_key_t key)
{
    count_accepted_read (key);
    auto result = co_await _inner.read (key);
    std::function<fw::task_t<void> ()> after_read;
    {
        std::lock_guard lock (_mutex);
        if (key.value == _held_read_key)
            after_read = std::exchange (_after_read, {});
    }
    if (after_read)
        co_await after_read ();
    co_return result;
}

fw::task_t<fw::store_scan_result_t>
observed_location_store_t::scan (fw::store_scan_request_t request)
{
    count_accepted_scan ();
    std::function<fw::task_t<void> ()> before_scan;
    {
        std::lock_guard lock (_mutex);
        if (_scan_ready && _scan_ready ())
            before_scan = std::exchange (_before_scan, {});
    }
    if (before_scan)
        co_await before_scan ();
    co_return co_await _inner.scan (std::move (request));
}

fw::task_t<fw::store_write_result_t>
observed_location_store_t::write (fw::store_write_request_t request)
{
    std::function<fw::task_t<bool> (const fw::store_write_request_t &)> ready;
    {
        std::lock_guard lock (_mutex);
        if (_before_write)
            ready = _before_write_ready;
    }
    std::function<fw::task_t<void> ()> before_write;
    if (ready && co_await ready (request)) {
        std::lock_guard lock (_mutex);
        before_write = std::exchange (_before_write, {});
    }
    if (before_write)
        co_await before_write ();
    auto result = co_await _inner.write (std::move (request));
    if (before_write) {
        std::lock_guard lock (_mutex);
        _intercepted_write_conflicted = std::holds_alternative<fw::store_write_conflict_t> (result);
    }
    if (std::holds_alternative<fw::store_write_applied_t> (result)) {
        std::function<fw::task_t<void> ()> after_write;
        {
            std::lock_guard lock (_mutex);
            after_write = _after_write;
        }
        if (after_write)
            co_await after_write ();
    }
    co_return result;
}

void observed_location_store_t::after_next_read (std::string key,
                                                 std::function<fw::task_t<void> ()> action)
{
    std::lock_guard lock (_mutex);
    _held_read_key = std::move (key);
    _after_read = std::move (action);
}

void observed_location_store_t::observe (std::function<fw::task_t<void> ()> after_write)
{
    std::lock_guard lock (_mutex);
    _after_write = std::move (after_write);
}

void observed_location_store_t::before_next_write_when (
  std::function<fw::task_t<bool> (const fw::store_write_request_t &)> ready,
  std::function<fw::task_t<void> ()> action)
{
    std::lock_guard lock (_mutex);
    _before_write_ready = std::move (ready);
    _before_write = std::move (action);
}

void observed_location_store_t::before_scan_when (std::function<bool ()> ready,
                                                  std::function<fw::task_t<void> ()> action)
{
    std::lock_guard lock (_mutex);
    _scan_ready = std::move (ready);
    _before_scan = std::move (action);
}

bool observed_location_store_t::intercepted_write_conflicted ()
{
    std::lock_guard lock (_mutex);
    return _intercepted_write_conflicted;
}

void observed_location_store_t::observe_authority_reads (const std::string &authority_key)
{
    std::lock_guard lock (_mutex);
    _observed_authority_key = authority_key;
    _acceptance_thread.reset ();
    _acceptance_reads = _acceptance_scans = 0;
}

std::pair<std::size_t, std::size_t> observed_location_store_t::accepted_store_calls ()
{
    std::lock_guard lock (_mutex);
    return {_acceptance_reads, _acceptance_scans};
}

void observed_location_store_t::count_accepted_read (const fw::store_key_t &key)
{
    std::lock_guard lock (_mutex);
    if (!_observed_authority_key.empty () && key.value == _observed_authority_key)
        _acceptance_thread = std::this_thread::get_id ();
    if (_acceptance_thread == std::this_thread::get_id ())
        ++_acceptance_reads;
}

void observed_location_store_t::count_accepted_scan ()
{
    std::lock_guard lock (_mutex);
    if (_acceptance_thread == std::this_thread::get_id ())
        ++_acceptance_scans;
}

intake_store_t::intake_store_t ()
{
    // Capture the provider's key through its existing authority conversion.
    capture_key = &authority_store_key;
    fw::runtime::provider_location_repository_t repository (*this);
    repository.read_authority (fw::runtime::actor_authority_key (actor_id)).result ().value ();
    capture_key = nullptr;
}

fw::task_t<fw::store_read_result_t> intake_store_t::read (fw::store_key_t key)
{
    if (capture_key)
        *capture_key = key.value;
    auto actual = co_await inner.read (key);
    const bool infrastructure = fw::detail::current_infrastructure_wait ();
    int expected = 1;
    if (infrastructure && std::this_thread::get_id () != releaser
        && key.value == authority_store_key && stage.compare_exchange_strong (expected, 0)) {
        authority_value = std::move (actual);
        observe (0);
        co_return co_await authority_source.task ();
    }
    expected = 3;
    if (std::this_thread::get_id () == releaser && stage.compare_exchange_strong (expected, 0)) {
        lease_value = std::move (actual);
        observe (2);
        co_return co_await lease_source.task ();
    }
    co_return actual;
}

fw::task_t<fw::store_scan_result_t> intake_store_t::scan (fw::store_scan_request_t request)
{
    auto actual = co_await inner.scan (std::move (request));
    int expected = 2;
    if (std::this_thread::get_id () == releaser && stage.compare_exchange_strong (expected, 0)) {
        nodes_value = std::move (actual);
        observe (1);
        co_return co_await nodes_source.task ();
    }
    co_return actual;
}

fw::task_t<fw::store_write_result_t> intake_store_t::write (fw::store_write_request_t request)
{
    return inner.write (std::move (request));
}

void intake_store_t::observe (std::size_t index)
{
    reached[index].store (true);
}

void intake_store_t::release (int index, bool failure)
{
    stage.store (failure || index == 2 ? 0 : index + 2);
    if (index == 0)
        authority_source.complete (
          failure ? fw::result_t<fw::store_read_result_t>::failure (
                      fw::framework_error_kind_t::internal_failure, "intake authority failure")
                  : fw::result_t<fw::store_read_result_t>::success (*authority_value));
    else if (index == 1)
        nodes_source.complete (
          failure ? fw::result_t<fw::store_scan_result_t>::failure (
                      fw::framework_error_kind_t::internal_failure, "intake nodes failure")
                  : fw::result_t<fw::store_scan_result_t>::success (*nodes_value));
    else
        lease_source.complete (
          failure ? fw::result_t<fw::store_read_result_t>::failure (
                      fw::framework_error_kind_t::internal_failure, "intake lease failure")
                  : fw::result_t<fw::store_read_result_t>::success (*lease_value));
}

void intake_store_t::release_all ()
{
    stage.store (0);
    authority_source.complete (fw::result_t<fw::store_read_result_t>::failure (
      fw::framework_error_kind_t::shutting_down, "intake fixture settled"));
    nodes_source.complete (fw::result_t<fw::store_scan_result_t>::failure (
      fw::framework_error_kind_t::shutting_down, "intake fixture settled"));
    lease_source.complete (fw::result_t<fw::store_read_result_t>::failure (
      fw::framework_error_kind_t::shutting_down, "intake fixture settled"));
}

intake_store_t::~intake_store_t ()
{
    release_all ();
}
} // namespace zlink::framework::test::actor_maintenance_join
