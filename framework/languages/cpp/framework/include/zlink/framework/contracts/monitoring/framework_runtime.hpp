/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/lifecycle.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace zlink::framework
{

struct observation_loss_t
{
    std::uint64_t coalesced_count = 0;
    std::uint64_t discarded_terminal_count = 0;

    friend bool operator== (const observation_loss_t &,
                            const observation_loss_t &) = default;
};

template <typename TStatus>
struct observed_status_t final
{
    TStatus status;
    observation_loss_t loss;
};

struct inbound_dispatch_status_t
{
    std::uint64_t application_hwm_bytes = 0;
    std::uint64_t pending_payload_bytes = 0;
    std::uint64_t queued_payload_bytes = 0;
    std::uint64_t active_payload_bytes = 0;
    bool application_receive_paused = false;
    std::uint64_t pending_completion_sends = 0;
    std::uint64_t completion_send_limit = 0;

    friend bool operator== (const inbound_dispatch_status_t &,
                            const inbound_dispatch_status_t &) = default;
};

struct framework_runtime_status_t
{
    framework_runtime_state_t state =
      framework_runtime_state_t::preparing;
    bool is_ready = false;
    bool accepting_work = false;
    std::optional<std::chrono::system_clock::time_point>
      operation_deadline;
    std::optional<relocation_result_t> relocation_result;
    std::optional<termination_result_t> termination_result;
    inbound_dispatch_status_t inbound_dispatch;
    std::uint64_t sequence = 0;
    std::chrono::system_clock::time_point observed_at;

    friend bool operator== (const framework_runtime_status_t &,
                            const framework_runtime_status_t &) = default;
};

class runtime_observation_t
{
  public:
    virtual ~runtime_observation_t () = default;
    virtual void close () noexcept = 0;
};

class framework_runtime_t
{
  public:
    virtual ~framework_runtime_t () = default;
    virtual framework_runtime_status_t status () const = 0;
    virtual std::unique_ptr<runtime_observation_t>
    observe (
      std::size_t capacity,
      std::function<void (
        const observed_status_t<framework_runtime_status_t> &)> observer) = 0;
};

} // namespace zlink::framework
