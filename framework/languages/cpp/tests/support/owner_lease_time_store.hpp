/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/locations/in_memory_store_providers.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace zlink::framework::tests
{

class owner_lease_time_store_t final : public location_store_t
{
  public:
    enum class lease_view_t
    {
        expired,
        live,
        missing_expiry
    };

    owner_lease_time_store_t (runtime::in_memory_location_store_t &inner,
                              std::string owner_id,
                              lease_view_t lease_view) :
        _inner (&inner),
        _owner_key (std::string ("owner-lease") + '\0' + std::move (owner_id)),
        _lease_view (lease_view)
    {
    }

    task_t<store_read_result_t> read (store_key_t key) override
    {
        using namespace std::chrono_literals;
        const auto inject_owner_time = key.value == _owner_key;
        {
            const std::lock_guard lock (_observation_mutex);
            if (std::this_thread::get_id () == _observation_thread) {
                if (!_watched_authority_fragment.empty ()
                    && key.value.find (_watched_authority_fragment) != std::string::npos)
                    ++_watched_authority_reads;
                if (inject_owner_time
                    && _watched_authority_reads == _authority_reads_before_owner)
                    _owner_read_in_expected_order = true;
            }
        }
        auto result = _inner->read (std::move (key)).result ().value ();
        if (inject_owner_time) {
            if (auto *found = std::get_if<store_found_t> (&result)) {
                if (_lease_view == lease_view_t::missing_expiry)
                    found->value.expires_at.reset ();
                else
                    found->value.expires_at =
                      found->value.store_now
                      + (_lease_view == lease_view_t::live ? 1min : 0min);
            }
        }
        return task_t<store_read_result_t> (
          result_t<store_read_result_t>::success (std::move (result)));
    }

    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        return _inner->write (std::move (request));
    }

    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return _inner->scan (std::move (request));
    }

    void observe_owner_read_order (std::string authority_fragment,
                                   unsigned authority_reads_before_owner)
    {
        const std::lock_guard lock (_observation_mutex);
        _observation_thread = std::this_thread::get_id ();
        _watched_authority_fragment = std::move (authority_fragment);
        _authority_reads_before_owner = authority_reads_before_owner;
        _watched_authority_reads = 0;
        _owner_read_in_expected_order = false;
    }

    bool owner_was_read_in_expected_order () const
    {
        const std::lock_guard lock (_observation_mutex);
        return _owner_read_in_expected_order;
    }

  private:
    runtime::in_memory_location_store_t *_inner;
    std::string _owner_key;
    lease_view_t _lease_view;
    mutable std::mutex _observation_mutex;
    std::thread::id _observation_thread;
    std::string _watched_authority_fragment;
    unsigned _authority_reads_before_owner = 0;
    unsigned _watched_authority_reads = 0;
    bool _owner_read_in_expected_order = false;
};

} // namespace zlink::framework::tests
