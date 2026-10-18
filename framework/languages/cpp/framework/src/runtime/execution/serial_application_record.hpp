/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/spots/spot.hpp>
#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace zlink::framework::runtime
{
// The FIFO owns payload, position and the single current supply lease.
struct serial_application_admission_t
{
    using initial_t = std::function<void ()>;
    struct durable_t
    {
        std::
          variant<initial_t, application_job_queue_t::waiter_t, application_job_queue_t::permit_t>
            supply;
    };
    std::shared_ptr<application_job_queue_t> jobs;
    std::variant<initial_t, durable_t> reservation;

    serial_application_admission_t () = default;
    serial_application_admission_t (serial_application_admission_t &&) = default;
    serial_application_admission_t &operator= (serial_application_admission_t &&) = default;
    serial_application_admission_t (const serial_application_admission_t &) = delete;
    serial_application_admission_t &operator= (const serial_application_admission_t &) = delete;
    ~serial_application_admission_t () { release_initial (); }

    bool durable () const noexcept { return std::holds_alternative<durable_t> (reservation); }
    bool ready () const noexcept
    {
        return !durable () || !jobs
               || std::holds_alternative<application_job_queue_t::permit_t> (
                 std::get<durable_t> (reservation).supply);
    }
    void retain_initial (initial_t release)
    {
        if (durable ())
            std::get<durable_t> (reservation).supply = std::move (release);
        else
            reservation = std::move (release);
    }
    void release_initial ()
    {
        auto *initial = durable ()
                          ? std::get_if<initial_t> (&std::get<durable_t> (reservation).supply)
                          : std::get_if<initial_t> (&reservation);
        if (initial) {
            if (auto release = std::exchange (*initial, {}))
                release ();
        }
    }
    void take_over ()
    {
        if (durable ()) {
            release_initial ();
            return;
        }
        auto release = std::move (std::get<initial_t> (reservation));
        reservation.emplace<durable_t> ();
        if (release)
            release ();
    }
    void discard ()
    {
        release_initial ();
        if (durable ())
            std::get<durable_t> (reservation).supply.emplace<initial_t> ();
    }
    void enter_handler ()
    {
        if (!durable ()) {
            release_initial ();
            return;
        }
        auto &supply = std::get<durable_t> (reservation).supply;
        if (auto *permit = std::get_if<application_job_queue_t::permit_t> (&supply))
            permit->release_for_handler_entry ();
        supply.emplace<initial_t> ();
    }
};

struct serial_application_record_t
{
    std::optional<stateful::turn_record_t> payload;
    serial_application_admission_t admission;
    std::optional<std::pair<zlink::message_t, spot_inbound_message_t>> invocation;
    std::function<void (result_t<void>)> retire_invocation;
    // Original native terminal follows the accepted payload across local
    // handoffs; the raw dispatcher remains its sole reply owner.
    std::function<bool (std::optional<protocol::application_payload_t>)> native_reply;
    // Returning this claim does not complete the pending native reply.
    std::function<void ()> transferred;

    bool complete_native (std::optional<protocol::application_payload_t> reply)
    {
        if (auto terminal = std::exchange (native_reply, {}))
            return terminal (std::move (reply));
        return false;
    }

    bool saved_work () const noexcept
    {
        return admission.durable ()
               || (payload && (payload->application_record || payload->frozen_record));
    }
    void release_native_claim ()
    {
        if (auto release = std::exchange (transferred, {}))
            release ();
    }
    void take_over ()
    {
        admission.take_over ();
        release_native_claim ();
    }
};
} // namespace zlink::framework::runtime
