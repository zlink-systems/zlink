#pragma once
#include "player.hpp"

namespace MyGame
{
class RoomProvisioner final : public fx::instance_spot_t
{
    fx::instance_spot_context_t context_;
    fx::spot_manager_t &spots_;

  public:
    RoomProvisioner (fx::instance_spot_context_t context, fx::spot_manager_t &spots) :
        context_ (std::move (context)), spots_ (spots)
    {
    }

    fx::instance_spot_context_t &context () noexcept override { return context_; }
    const fx::instance_spot_context_t &context () const noexcept override { return context_; }

    void configure () override
    {
        context_.handlers ()
          .add_handler<&RoomProvisioner::prepare> ()
          .add_handler<&RoomProvisioner::withdraw> ();
    }

    fx::task_t<void> prepare (const PrepareRoom &request)
    {
        const auto created = co_await spots_.get_or_create (request.assignmentId, Names::table)
                               .in_mesh (Names::mesh)
                               .creation_request (request)
                               .async ();
        const RoomPrepared reply{request.assignmentId,
                                 created.state != fx::spot_create_state_t::rejected};
        co_await context_.send_to_spot (MatchNames::pool_key, reply)
          .instance_spot (MatchNames::pool)
          .in_mesh (Names::mesh)
          .async ();
    }

    fx::task_t<void> withdraw (const WithdrawRoom &request)
    {
        if (co_await spots_.find (request.assignmentId))
            co_await context_.send_to_spot (request.assignmentId, request).async ();
    }
};
}
