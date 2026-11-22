/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/locations/in_memory_store_providers.hpp"
#include <zlink/framework.hpp>
#include <functional>

namespace zlink::framework::test::opaque_store
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;

class reclaim_scan_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        if (before_read && key.value == before_read_key.value) {
            auto callback = std::exchange (before_read, {});
            co_await callback ();
        }
        auto result = co_await inner.read (key);
        if (after_read && key.value == after_read_key.value) {
            auto callback = std::exchange (after_read, {});
            co_await callback ();
        }
        if (key.value == expired_lease_key.value)
            if (auto *found = std::get_if<store_found_t> (&result))
                found->value.expires_at = found->value.store_now;
        co_return result;
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        if (before_write) {
            auto callback = std::exchange (before_write, {});
            co_await callback ();
        }
        if (before_member_write
            && std::any_of (request.mutations.begin (), request.mutations.end (),
                            [] (const auto &mutation) {
                                const auto *put = std::get_if<store_put_t> (&mutation);
                                return put
                                       && put->key.value.starts_with (std::string ("authority")
                                                                      + '\0' + "actor" + '\0');
                            })) {
            auto callback = std::exchange (before_member_write, {});
            co_await callback ();
        }
        auto result = co_await inner.write (std::move (request));
        if (after_write) {
            auto callback = std::exchange (after_write, {});
            co_await callback ();
        }
        co_return result;
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        if (armed) {
            ++scans;
            if (fail_scan)
                throw framework_exception_t (framework_error_kind_t::unavailable,
                                             "membership snapshot failed");
            if (expire_next_page && request.cursor) {
                expire_next_page = false;
                ++expired;
                co_return store_scan_expired_t{};
            }
            request.limit = 1;
        }
        co_return co_await inner.scan (std::move (request));
    }
    in_memory_location_store_t inner;
    bool armed = false;
    bool fail_scan = false;
    bool expire_next_page = false;
    store_key_t before_read_key;
    store_key_t after_read_key;
    store_key_t expired_lease_key;
    std::function<task_t<void> ()> after_read;
    std::function<task_t<void> ()> before_read;
    std::function<task_t<void> ()> before_member_write;
    std::function<task_t<void> ()> before_write;
    std::function<task_t<void> ()> after_write;
    unsigned scans = 0;
    unsigned expired = 0;
};

}
