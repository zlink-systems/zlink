/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/framework.hpp>
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>

namespace zlink::framework::test::host_lifecycle
{
class configuration_actor_t final : public zlink::framework::actor_t
{
  public:
    explicit configuration_actor_t (zlink::framework::actor_context_t context) :
        _context (std::move (context))
    {
        ++live_count;
    }

    ~configuration_actor_t () override { --live_count; }
    static inline std::atomic_int live_count{0};

    zlink::framework::actor_context_t &context () noexcept override { return _context; }
    const zlink::framework::actor_context_t &context () const noexcept override { return _context; }

  private:
    zlink::framework::actor_context_t _context;
};

class configuration_actor_factory_t final
    : public zlink::framework::actor_factory_t<configuration_actor_t>
{
  public:
    zlink::framework::task_t<std::shared_ptr<configuration_actor_t>>
    create (zlink::framework::actor_context_t context, std::stop_token) override
    {
        create_count.fetch_add (1, std::memory_order_relaxed);
        co_return std::make_shared<configuration_actor_t> (std::move (context));
    }
    static inline std::atomic_int create_count{0};
};

class observing_actor_creation_store_t final : public zlink::framework::location_store_t
{
  public:
    explicit observing_actor_creation_store_t (
      std::shared_ptr<zlink::framework::runtime::in_memory_location_store_t> inner) :
        inner (std::move (inner))
    {
    }

    zlink::framework::task_t<zlink::framework::store_read_result_t>
    read (zlink::framework::store_key_t key) override
    {
        if (on_read)
            on_read (key);
        if (key.value.starts_with (std::string ("creation-terminal") + '\0')) {
            terminal_reads.fetch_add (1, std::memory_order_acq_rel);
            if (hide_terminal_reads.load (std::memory_order_acquire)) {
                return zlink::framework::task_t<zlink::framework::store_read_result_t> (
                  zlink::framework::result_t<zlink::framework::store_read_result_t>::success (
                    zlink::framework::store_missing_t{std::chrono::system_clock::now ()}));
            }
            if (terminal_override) {
                const auto now = std::chrono::system_clock::now ();
                return zlink::framework::task_t<zlink::framework::store_read_result_t> (
                  zlink::framework::result_t<zlink::framework::store_read_result_t>::success (
                    zlink::framework::store_found_t{{*terminal_override,
                                                     {"node-production-schema"},
                                                     now + std::chrono::minutes (1),
                                                     now}}));
            }
        }
        return inner->read (std::move (key));
    }

    zlink::framework::task_t<zlink::framework::store_write_result_t>
    write (zlink::framework::store_write_request_t request) override
    {
        bool writes_terminal = false;
        for (const auto &mutation : request.mutations) {
            if (const auto *put = std::get_if<zlink::framework::store_put_t> (&mutation);
                put && put->key.value.starts_with (std::string ("creation-terminal") + '\0')) {
                writes_terminal = true;
                std::lock_guard lock (terminal_mutex);
                published_terminal = put->bytes;
            }
        }
        if (writes_terminal && republish_descriptor.exchange (false, std::memory_order_acq_rel)) {
            const auto descriptor_condition = std::find_if (
              request.conditions.begin (), request.conditions.end (), [] (const auto &condition) {
                  const auto *version =
                    std::get_if<zlink::framework::store_version_condition_t> (&condition);
                  return version
                         && version->key.value.starts_with (std::string ("mesh-node") + '\0');
              });
            if (descriptor_condition == request.conditions.end ())
                throw std::logic_error ("creation write is missing descriptor StoreVersion");
            const auto key =
              std::get<zlink::framework::store_version_condition_t> (*descriptor_condition).key;
            const auto read = co_await inner->read (key);
            zlink::framework::store_write_request_t republish;
            republish.mutations.emplace_back (zlink::framework::store_put_t{
              key, std::get<zlink::framework::store_found_t> (read).value.bytes, std::nullopt});
            co_await inner->write (std::move (republish));
            descriptor_conflicts.fetch_add (1, std::memory_order_relaxed);
        }
        auto written = inner->write (std::move (request));
        if (!writes_terminal || !force_terminal_write_conflict.load (std::memory_order_acquire))
            co_return co_await written;
        (void) co_await written;
        co_return zlink::framework::store_write_result_t{
          zlink::framework::store_write_conflict_t{std::chrono::system_clock::now ()}};
    }

    zlink::framework::task_t<zlink::framework::store_scan_result_t>
    scan (zlink::framework::store_scan_request_t request) override
    {
        return inner->scan (std::move (request));
    }

    std::function<void (const zlink::framework::store_key_t &)> on_read;
    std::shared_ptr<zlink::framework::runtime::in_memory_location_store_t> inner;
    std::atomic_bool republish_descriptor{false};
    std::atomic_int descriptor_conflicts{0};
    std::atomic_size_t terminal_reads{0};
    std::optional<std::vector<std::byte>> terminal_override;
    std::atomic_bool hide_terminal_reads{false};
    std::atomic_bool force_terminal_write_conflict{false};
    std::mutex terminal_mutex;
    std::vector<std::byte> published_terminal;
};
}
