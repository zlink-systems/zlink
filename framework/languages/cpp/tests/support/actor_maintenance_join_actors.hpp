/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "actor_maintenance_join_observations.hpp"
#include "actor_maintenance_join_packets.hpp"
#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/spots/spot.hpp>

namespace zlink::framework::test::actor_maintenance_join
{
class actor_t final : public fw::actor_t
{
  public:
    actor_t (fw::actor_context_t context, std::shared_ptr<observations_t> evidence);
    ~actor_t () override;
    int instance;
    fw::actor_context_t &context () noexcept override;
    const fw::actor_context_t &context () const noexcept override;
    fw::task_t<void> on_join_completed (const fw::actor_join_completion_t &completion) override;

  private:
    friend class consecutive_join_state_t;
    fw::actor_context_t _context;
    // Capture the evidence of the test that created this object; late callbacks must not
    // write into a later test.
    std::shared_ptr<observations_t> observations;
};

class factory_t final : public fw::actor_factory_t<actor_t>
{
  public:
    explicit factory_t (std::shared_ptr<observations_t> evidence);
    fw::task_t<std::shared_ptr<actor_t>> create (fw::actor_context_t context,
                                                 std::stop_token) override;

  private:
    // Capture the evidence of the test that created this object; late callbacks must not
    // write into a later test.
    std::shared_ptr<observations_t> observations;
};

class entry_t final : public fw::entry_spot_t<actor_t>
{
  public:
    entry_t (fw::entry_spot_context_t context, std::shared_ptr<observations_t> evidence);
    fw::entry_spot_context_t &context () noexcept override;
    const fw::entry_spot_context_t &context () const noexcept override;
    void configure () override;
    void record (actor_t &actor, fw::message_context_t &, const record_t &request);
    fw::task_t<void> on_disconnect_actor (actor_t &) override;
    fw::task_t<probe_t>
    close_session (actor_t &actor, fw::message_context_t &, const close_session_t &request);
    fw::task_t<void> view (actor_t &actor, fw::message_context_t &, const view_request_t &);
    probe_t probe (actor_t &actor, fw::message_context_t &, const probe_t &request);
    void move (actor_t &actor, fw::message_context_t &, const move_t &request);
    fw::task_t<fw::actor_create_response_t> on_create_actor (actor_t &,
                                                             const fw::message_t &) override;
    fw::task_t<void> on_actor_joined (actor_t &) override;
    fw::task_t<void> on_leave_actor (actor_t &) override;

  private:
    fw::entry_spot_context_t _context;
    // Capture the evidence of the test that created this object; late callbacks must not
    // write into a later test.
    std::shared_ptr<observations_t> observations;
};

class room_t final : public fw::spot_t<actor_t>
{
  public:
    room_t (fw::spot_context_t context, std::shared_ptr<observations_t> evidence);
    fw::spot_context_t &context () noexcept override;
    const fw::spot_context_t &context () const noexcept override;
    void configure () override;
    fw::task_t<void> on_disconnect_actor (actor_t &) override;
    fw::task_t<probe_t>
    close_session (actor_t &actor, fw::message_context_t &, const close_session_t &request);
    fw::task_t<void> view (actor_t &actor, fw::message_context_t &, const view_request_t &);
    fw::task_t<probe_t> ordinary_probe (const probe_t &request);
    fw::task_t<void> ready (const ready_t &);
    void move (actor_t &actor, fw::message_context_t &, const move_t &request);
    fw::task_t<void> push (actor_t &actor, fw::message_context_t &, const probe_t &message);
    fw::task_t<probe_t> probe (actor_t &actor, fw::message_context_t &, const probe_t &request);
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view joining_actor,
                                                            const fw::message_t &) override;
    fw::task_t<void> on_actor_joined (actor_t &actor) override;
    fw::task_t<void> on_closing (const fw::spot_closing_context_t &context,
                                 std::stop_token) override;
    fw::task_t<void> on_leave_actor (actor_t &) override;

  private:
    fw::spot_context_t _context;
    // Capture the evidence of the test that created this object; late callbacks must not
    // write into a later test.
    std::shared_ptr<observations_t> observations;
};
} // namespace zlink::framework::test::actor_maintenance_join
