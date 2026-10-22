/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/spots/spot.hpp>
#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"

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

// A process-local accepted target. Its linear terminal returns the existing
// activation admission on completion, export, or a lost mailbox publication.
struct serial_application_target_t
{
    serial_application_target_t () = default;
    serial_application_target_t (const serial_application_target_t &) = delete;
    serial_application_target_t &operator= (const serial_application_target_t &) = delete;
    std::shared_ptr<serial_execution_queue_t> queue;
    std::variant<std::shared_ptr<void>, framework_exception_t> context;
    std::shared_ptr<void> instance;
    std::function<void ()> terminal;
    ~serial_application_target_t () { release (); }
    void release ()
    {
        if (auto complete = std::exchange (terminal, {}))
            complete ();
    }
};

struct serial_application_record_t
{
    using invocation_t = std::pair<zlink::message_t, spot_inbound_message_t>;

    struct spot_identity_t
    {
        protocol::spot_message_header_t header;
        protocol::request_source_fence_t source;
    };

    void set_spot_identity (spot_identity_t identity, zlink::message_t header)
    {
        native_t native;
        native.identity = std::move (identity);
        native.header = std::move (header);
        _storage = std::move (native);
    }

    invocation_t *invocation () noexcept
    {
        auto *native = std::get_if<native_t> (&_storage);
        return native ? &native->invocation : nullptr;
    }

    const stateful::turn_record_t *identity () const noexcept
    {
        const auto *native = std::get_if<native_t> (&_storage);
        const auto *owned_native = native ? std::get_if<frozen_t> (&native->identity) : nullptr;
        if (native && !owned_native)
            return nullptr;
        const auto &owned = native ? *owned_native : std::get<frozen_t> (_storage);
        return owned ? &*owned : nullptr;
    }

    zlink::message_t application_header () const
    {
        if (const auto *native = std::get_if<native_t> (&_storage))
            return native->header.copy ();
        const auto *frozen = identity ();
        if (!frozen)
            throw protocol::service_wire_error_t ("accepted record has no payload identity");
        return stateful::with_accepted_application (*frozen, [] (auto, const auto &application) {
            return protocol::decode_application_header (application);
        });
    }

    void set_invocation (zlink::message_t body, spot_inbound_message_t metadata)
    {
        if (auto *native = std::get_if<native_t> (&_storage)) {
            native->invocation = {std::move (body), std::move (metadata)};
            return;
        }
        native_t native;
        auto &frozen = std::get<frozen_t> (_storage);
        if (frozen) {
            auto &turn = *frozen;
            if (!turn.application_record && !turn.frozen_record && !turn.payload.empty ())
                turn.frozen_record = protocol::decode_frozen_record (turn.payload);
            native.header = application_header ();
            native.identity = std::move (frozen);
            auto &owned = *std::get<frozen_t> (native.identity);
            // The accepted routing identity survives; the invocation is the sole body owner.
            owned.payload = std::vector<std::uint8_t>{};
            const auto discard_body = [] (protocol::application_payload_t &application) {
                application = protocol::application_payload_t{std::move (application.packet_name),
                                                              std::move (application.content_type),
                                                              {},
                                                              std::move (application.flow_id),
                                                              application.flow_origin};
            };
            if (owned.application_record) {
                std::visit ([&] (auto &value) { discard_body (value.application); },
                            owned.application_record->body);
                owned.frozen_record.reset ();
            }
            if (owned.frozen_record) {
                if (owned.frozen_record->application)
                    discard_body (*owned.frozen_record->application);
                owned.frozen_record->canonical_bytes = std::vector<std::uint8_t>{};
            }
        }
        native.invocation = {std::move (body), std::move (metadata)};
        _storage = std::move (native);
    }

    std::optional<stateful::turn_record_t> &payload ()
    {
        if (auto *native = std::get_if<native_t> (&_storage)) {
            auto *owned = std::get_if<frozen_t> (&native->identity);
            if (owned && !*owned)
                return *owned;
            std::vector<zlink::message_t> parts;
            parts.push_back (native->header.copy ());
            parts.push_back (native->invocation.first.copy ());
            auto application = protocol::application_payload_t::from_parts (std::move (parts));
            if (const auto *spot = std::get_if<spot_identity_t> (&native->identity)) {
                stateful::turn_record_t turn;
                turn.application_record = stateful::accepted_spot_record (
                  spot->header, spot->source, std::move (application),
                  spot->header.correlation.has_value ());
                turn.application_payload_bytes = protocol::application_payload_hwm_bytes (
                  std::get<protocol::frozen_spot_application_body_t> (turn.application_record->body)
                    .application);
                _storage = frozen_t{std::move (turn)};
                return std::get<frozen_t> (_storage);
            }
            auto turn = std::move (*owned);
            const auto restore_body = [&] (protocol::application_payload_t &identity) {
                application.flow_id = std::move (identity.flow_id);
                application.flow_origin = identity.flow_origin;
                identity = std::move (application);
            };
            if (turn->application_record)
                std::visit ([&] (auto &value) { restore_body (value.application); },
                            turn->application_record->body);
            else
                restore_body (*turn->frozen_record->application);
            _storage = std::move (turn);
        }
        return std::get<frozen_t> (_storage);
    }
    std::shared_ptr<serial_application_target_t> target;
    serial_application_admission_t admission;
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
        const auto *owned = identity ();
        return admission.durable ()
               || (owned && (owned->application_record || owned->frozen_record));
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

  private:
    using frozen_t = std::optional<stateful::turn_record_t>;
    struct native_t
    {
        std::variant<frozen_t, spot_identity_t> identity;
        zlink::message_t header;
        invocation_t invocation;
    };
    std::variant<frozen_t, native_t> _storage;
};
} // namespace zlink::framework::runtime
