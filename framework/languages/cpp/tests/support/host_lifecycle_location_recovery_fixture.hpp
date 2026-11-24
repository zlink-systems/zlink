/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/framework.hpp>
#include "runtime/locations/in_memory_store_providers.hpp"
#include <atomic>

namespace zlink::framework::test::host_lifecycle
{
class recovering_location_store_t final : public zlink::framework::location_store_t
{
  public:
    zlink::framework::task_t<zlink::framework::store_read_result_t>
    read (zlink::framework::store_key_t key) override
    {
        if (!_available.load (std::memory_order_acquire))
            return failed<zlink::framework::store_read_result_t> ();
        return _inner->read (std::move (key));
    }

    zlink::framework::task_t<zlink::framework::store_write_result_t>
    write (zlink::framework::store_write_request_t request) override
    {
        if (!_available.load (std::memory_order_acquire))
            co_return co_await failed<zlink::framework::store_write_result_t> ();
        auto result = co_await _inner->write (std::move (request));
        {
            std::lock_guard lock (_publication_mutex);
        }
        _publication_changed.notify_all ();
        co_return result;
    }

    void wait_for_publication (const std::function<bool ()> &published)
    {
        std::unique_lock lock (_publication_mutex);
        _publication_changed.wait (lock, published);
    }

    zlink::framework::task_t<zlink::framework::store_scan_result_t>
    scan (zlink::framework::store_scan_request_t request) override
    {
        return _inner->scan (std::move (request));
    }

    void recover () noexcept { _available.store (true, std::memory_order_release); }

  private:
    template <typename T> static zlink::framework::task_t<T> failed ()
    {
        return zlink::framework::task_t<T> (zlink::framework::result_t<T>::failure (
          zlink::framework::framework_error_kind_t::unavailable, "injected Location Store outage"));
    }

    std::shared_ptr<zlink::framework::runtime::in_memory_location_store_t> _inner =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    std::atomic_bool _available{false};
    std::mutex _publication_mutex;
    std::condition_variable _publication_changed;
};
}
