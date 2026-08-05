/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/operations/operation_id.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace zlink::framework::runtime::foundation
{

using operation_id_t = runtime::operation_id_t;

enum class operation_terminal_t
{
    completed,
    timed_out,
    cancelled,
    transport_failed,
    shutdown
};

class operation_registry_t
{
  public:
    using clock_t = std::chrono::steady_clock;
    using callback_t = std::function<void (operation_terminal_t, std::vector<std::uint8_t>)>;

    explicit operation_registry_t (std::size_t capacity);
    ~operation_registry_t () noexcept;

    bool register_operation (operation_id_t id,
                             clock_t::time_point deadline,
                             callback_t callback);
    bool complete (const operation_id_t &id, std::vector<std::uint8_t> payload);
    bool cancel (const operation_id_t &id);
    bool fail (const operation_id_t &id, operation_terminal_t terminal);
    std::size_t expire (clock_t::time_point now);
    std::size_t shutdown ();
    std::size_t size () const;

  private:
    using id_hash_t = runtime::operation_id_hash_t;

    struct pending_t
    {
        clock_t::time_point deadline;
        callback_t callback;
    };

    using entry_t = std::pair<callback_t, operation_terminal_t>;
    bool take (const operation_id_t &id, callback_t &callback);

    const std::size_t _capacity;
    mutable std::mutex _mutex;
    std::unordered_map<operation_id_t, pending_t, id_hash_t> _pending;
    bool _closed = false;
};

} // namespace zlink::framework::runtime::foundation
