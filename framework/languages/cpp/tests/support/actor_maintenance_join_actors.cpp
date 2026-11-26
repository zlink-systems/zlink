/* SPDX-License-Identifier: Elastic-2.0 */

#include "actor_maintenance_join_actors.hpp"
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace zlink::framework::test::actor_maintenance_join
{
actor_t::actor_t (fw::actor_context_t context, std::shared_ptr<observations_t> evidence) :
    _context (std::move (context)), observations (std::move (evidence))
{
    std::lock_guard lock (observations->mutex);
    instance = ++observations->instances;
}

actor_t::~actor_t ()
{
    {
        std::lock_guard lock (observations->mutex);
        observations->destroyed.push_back (instance);
    }
    observations->changed.notify_all ();
}

fw::actor_context_t &actor_t::context () noexcept
{
    return _context;
}

const fw::actor_context_t &actor_t::context () const noexcept
{
    return _context;
}

fw::task_t<void> actor_t::on_join_completed (const fw::actor_join_completion_t &delivered)
{
    // The caller owns `delivered` only until this coroutine first suspends.
    const fw::actor_join_completion_t completion = delivered;
    if (observations->join_completion_view)
        co_await observations->join_completion_view ();
    bool hold = false;
    std::shared_ptr<native_handoff_hold_t> held_suffix;
    const auto current_spot = _context.spot_id ();
    {
        std::lock_guard lock (observations->mutex);
        observations->completion_spots.push_back (current_spot);
        observations->completions.push_back (completion);
        observations->completion_owners.push_back (
          std::string (_context.actor_ref ().node_rid ().value ()));
        hold = observations->completion_to_hold == observations->completions.size ();
        if (observations->completions.size () == 2)
            held_suffix = observations->native_handoff_hold;
    }
    if (held_suffix)
        held_suffix->next_join_terminal.complete (fw::result_t<void>::success ());
    observations->changed.notify_all ();
    observations->join_completed.complete (fw::result_t<void>::success ());
    if (hold)
        co_await observations->release_completion.task ();
    if (observations->fail_completion)
        throw std::runtime_error ("completion callback failed");
    co_return;
}

factory_t::factory_t (std::shared_ptr<observations_t> evidence) :
    observations (std::move (evidence))
{
}

fw::task_t<std::shared_ptr<actor_t>> factory_t::create (fw::actor_context_t context,
                                                        std::stop_token)
{
    if (observations->fail_remote_factory
        && context.actor_ref ().node_rid ().value () == second_rid) {
        {
            std::lock_guard lock (observations->mutex);
            ++observations->remote_factory_failure_count;
        }
        throw std::runtime_error ("target Actor factory failed");
    }
    {
        std::lock_guard lock (observations->mutex);
        observations->factory_nodes.emplace_back (context.actor_ref ().node_rid ().value ());
    }
    co_return std::make_shared<actor_t> (std::move (context), observations);
}

entry_t::entry_t (fw::entry_spot_context_t context, std::shared_ptr<observations_t> evidence) :
    _context (std::move (context)), observations (std::move (evidence))
{
}

fw::entry_spot_context_t &entry_t::context () noexcept
{
    return _context;
}

const fw::entry_spot_context_t &entry_t::context () const noexcept
{
    return _context;
}

void entry_t::configure ()
{
    _context.handlers ().add_actor_send<&entry_t::move> ();
    _context.handlers ().add_actor_request<&entry_t::probe> ();
    _context.handlers ().add_actor_request<&entry_t::close_session> ();
    _context.handlers ().add_actor_send<&entry_t::view> ();
    _context.handlers ().add_actor_send<&entry_t::record> ();
}

void entry_t::record (actor_t &actor, fw::message_context_t &, const record_t &request)
{
    {
        std::lock_guard lock (observations->mutex);
        observations->recorded.push_back (request.value == instance_probe ? actor.instance
                                                                          : request.value);
    }
    observations->changed.notify_all ();
}

fw::task_t<void> entry_t::on_disconnect_actor (actor_t &)
{
    {
        std::lock_guard lock (observations->mutex);
        ++observations->disconnect_callbacks;
    }
    observations->changed.notify_all ();
    if (observations->disconnect_release)
        co_await observations->disconnect_release->task ();
    co_return;
}

fw::task_t<probe_t>
entry_t::close_session (actor_t &actor, fw::message_context_t &, const close_session_t &request)
{
    co_await actor.context ().bound_session ().disconnect ();
    co_return probe_t{request.value};
}

fw::task_t<void> entry_t::view (actor_t &actor, fw::message_context_t &, const view_request_t &)
{
    co_await actor.context ().bound_session ().send (view_t{actor.instance}).async ();
}

probe_t entry_t::probe (actor_t &actor, fw::message_context_t &, const probe_t &request)
{
    {
        std::lock_guard lock (observations->mutex);
        observations->intake_probe_values.push_back (request.value);
    }
    observations->changed.notify_all ();
    return request.value == instance_probe ? probe_t{actor.instance} : request;
}

void entry_t::move (actor_t &actor, fw::message_context_t &, const move_t &request)
{
    if (request.target == entry_target)
        actor.context ().join_entry_spot ().defer ();
    else {
        auto join = actor.context ().join_spot (fw::spot_id_t (request.target));
        if (observations->join_timeout)
            join.timeout (*observations->join_timeout);
        join.defer ();
        if (observations->after_move_deferred)
            observations->after_move_deferred ();
    }
}

fw::task_t<fw::actor_create_response_t> entry_t::on_create_actor (actor_t &, const fw::message_t &)
{
    {
        std::lock_guard lock (observations->mutex);
        ++observations->creation_callbacks;
    }
    if (observations->creation_case == creation_case_t::failed)
        throw std::runtime_error ("Entry creation failed");
    co_return observations->creation_case == creation_case_t::rejected
      ? fw::actor_create_response_t::reject ()
      : fw::actor_create_response_t::accept ();
}

fw::task_t<void> entry_t::on_actor_joined (actor_t &)
{
    std::lock_guard lock (observations->mutex);
    ++observations->entry_joined;
    co_return;
}

fw::task_t<void> entry_t::on_leave_actor (actor_t &)
{
    {
        std::lock_guard lock (observations->mutex);
        ++observations->entry_left;
    }
    observations->changed.notify_all ();
    co_return;
}

room_t::room_t (fw::spot_context_t context, std::shared_ptr<observations_t> evidence) :
    _context (std::move (context)), observations (std::move (evidence))
{
}

fw::spot_context_t &room_t::context () noexcept
{
    return _context;
}

const fw::spot_context_t &room_t::context () const noexcept
{
    return _context;
}

void room_t::configure ()
{
    _context.handlers ().add_actor_send<&room_t::move> ();
    _context.handlers ().add_actor_request<&room_t::probe> ();
    _context.handlers ().add_actor_request<&room_t::close_session> ();
    _context.handlers ().add_actor_send<&room_t::push> ();
    _context.handlers ().add_actor_send<&room_t::view> ();
    _context.handlers ().add_handler<&room_t::ready> ();
    _context.handlers ().add_handler<&room_t::ordinary_probe> ();
}

fw::task_t<void> room_t::on_disconnect_actor (actor_t &)
{
    {
        std::lock_guard lock (observations->mutex);
        ++observations->disconnect_callbacks;
    }
    observations->changed.notify_all ();
    co_return;
}

fw::task_t<probe_t>
room_t::close_session (actor_t &actor, fw::message_context_t &, const close_session_t &request)
{
    co_await actor.context ().bound_session ().disconnect ();
    co_return probe_t{request.value};
}

fw::task_t<void> room_t::view (actor_t &actor, fw::message_context_t &, const view_request_t &)
{
    co_await actor.context ().bound_session ().send (view_t{actor.instance}).async ();
}

fw::task_t<probe_t> room_t::ordinary_probe (const probe_t &request)
{
    const auto observation = observations->ordinary_spot;
    if (observation) {
        if (request.value == 1) {
            observation->entered.complete (fw::result_t<void>::success ());
            co_await observation->release.task ();
        }
        observation->handled.push_back (request.value);
    }
    co_return request;
}

fw::task_t<void> room_t::ready (const ready_t &)
{
    _context.relocation_ready ().defer ();
    co_return;
}

void room_t::move (actor_t &actor, fw::message_context_t &, const move_t &request)
{
    if (request.target == entry_target) {
        actor.context ().join_entry_spot ().defer ();
        return;
    }
    if (!observations->joined_sender || request.target != current_spot) {
        actor.context ().join_spot (fw::spot_id_t (request.target)).defer ();
        return;
    }
    try {
        actor.context ().join_spot (fw::spot_id_t (request.target)).defer ();
    }
    catch (const std::exception &error) {
        {
            std::lock_guard lock (observations->mutex);
            observations->next_join_error = error.what ();
            observations->next_join_attempted = true;
        }
        observations->changed.notify_all ();
        throw;
    }
    {
        std::lock_guard lock (observations->mutex);
        observations->next_join_attempted = true;
    }
    observations->changed.notify_all ();
}

fw::task_t<void> room_t::push (actor_t &actor, fw::message_context_t &, const probe_t &message)
{
    if (!observations->joined_payloads.empty ()) {
        {
            std::lock_guard lock (observations->mutex);
            observations->handled_payloads.push_back (message.value);
            if (observations->transfer_suffix) {
                observations->handled_spots.emplace_back (_context.spot_id ());
                observations->handled_nodes.emplace_back (
                  actor.context ().actor_ref ().node_rid ().value ());
                observations->handled_completion_counts.push_back (
                  observations->completions.size ());
            }
        }
        observations->changed.notify_all ();
        if (message.value < 0)
            throw std::runtime_error ("replayed payload failed");
        if (observations->transfer_suffix && message.value == 1)
            actor.context ().join_spot (fw::spot_id_t (suffix_spot)).defer ();
        co_return;
    }
    co_await actor.context ().bound_session ().send (message).async ();
}

fw::task_t<probe_t> room_t::probe (actor_t &actor, fw::message_context_t &, const probe_t &request)
{
    if (!observations->joined_payloads.empty ()) {
        std::lock_guard lock (observations->mutex);
        observations->handled_payloads.push_back (request.value);
        if (observations->transfer_suffix) {
            observations->handled_spots.emplace_back (_context.spot_id ());
            observations->handled_nodes.emplace_back (
              actor.context ().actor_ref ().node_rid ().value ());
            observations->handled_completion_counts.push_back (observations->completions.size ());
        }
    }
    observations->changed.notify_all ();
    if (observations->before_room_probe_reply)
        co_await observations->before_room_probe_reply ();
    co_return request.value == instance_probe ? probe_t{actor.instance} : request;
}

fw::task_t<fw::spot_actor_join_result_t> room_t::on_actor_join (std::string_view joining_actor,
                                                                const fw::message_t &)
{
    if (observations->consecutive_join_probe && _context.spot_id () == current_spot) {
        {
            std::lock_guard lock (observations->mutex);
            if (joining_actor == actor_id)
                observations->first_admission_accepted = true;
            if (joining_actor == "second-member")
                observations->second_admission_during_capture = observations->first_capture_pending;
        }
        observations->changed.notify_all ();
    }
    if (observations->hold_join_admission) {
        observations->join_admission_entered.complete (fw::result_t<void>::success ());
        co_await observations->release_join_admission.task ();
    }
    if (_context.spot_id () == failed_spot)
        throw std::runtime_error ("target admission failed");
    if (_context.spot_id () == framework_failed_spot)
        throw fw::framework_exception_t (fw::framework_error_kind_t::unavailable,
                                         "target is unavailable");
    if (_context.spot_id () == rejected_spot)
        co_return fw::spot_actor_join_result_t::reject (std::string (rejection_reply));
    co_return _context.spot_id () == maintenance_spot ? fw::spot_actor_join_result_t::reject ()
                                                      : fw::spot_actor_join_result_t::accept ();
}

fw::task_t<void> room_t::on_actor_joined (actor_t &actor)
{
    if (observations->joined_view)
        co_await observations->joined_view (actor.context ().actor_id ());
    if (observations->joined_sender && _context.spot_id () == local_spot) {
        {
            std::lock_guard lock (observations->mutex);
            auto *source = &observations->joined_send_captured;
            observations->joined_admission_completion = observations_t::joined_admission_batch_t{
              std::max<std::size_t> (1, observations->joined_payloads.size ()),
              [source] { source->complete (fw::result_t<void>::success ()); }};
        }
        if (observations->joined_payloads.empty ()) {
            co_await observations->joined_sender.load ()
              ->send (actor.context ().actor_id (), move_t{current_spot})
              .async ();
        } else {
            for (const auto value : observations->joined_payloads) {
                if (observations->suffix_request && value == 2) {
                    auto reply = std::make_shared<fw::task_t<probe_t>> (
                      observations->joined_sender.load ()
                        ->request (actor.context ().actor_id (), probe_t{value})
                        .timeout (wait_budget)
                        .async<probe_t> ());
                    observations->suffix_reply = [reply] {
                        const auto result = reply->result ();
                        return result ? fw::result_t<int>::success (result.value ().value)
                                      : fw::result_t<int>::failure (result.error_kind (),
                                                                    result.error ()->what ());
                    };
                } else {
                    co_await observations->joined_sender.load ()
                      ->send (actor.context ().actor_id (), probe_t{value})
                      .async ();
                }
            }
        }
        co_await observations->joined_send_captured.task ();
    }
    if (observations->joined_sender && _context.spot_id () == current_spot)
        co_await observations->joined_sender.load ()
          ->send (actor.context ().actor_id (), probe_t{1})
          .async ();
    co_return;
}

fw::task_t<void> room_t::on_closing (const fw::spot_closing_context_t &context, std::stop_token)
{
    std::function<fw::task_t<void> ()> hook;
    {
        std::lock_guard lock (observations->mutex);
        observations->closings.emplace_back (context.reason, observations->destroyed.size ());
        hook = observations->closing_hook;
    }
    if (hook)
        co_await hook ();
    co_return;
}

fw::task_t<void> room_t::on_leave_actor (actor_t &)
{
    if (_context.spot_id () == previous_spot) {
        {
            std::lock_guard lock (observations->mutex);
            observations->leave_entered = true;
        }
        observations->changed.notify_all ();
        co_await observations->release_leave.task ();
    }
    co_return;
}
} // namespace zlink::framework::test::actor_maintenance_join
