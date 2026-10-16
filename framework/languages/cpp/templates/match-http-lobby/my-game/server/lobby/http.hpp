#pragma once
#include "matchmaker.hpp"
namespace MyGame
{
// POST /match route on the Framework HTTP host. The handler holds no state of its own.
class MatchHandler final
{
    Matchmaker &matches_;
    fx::spot_manager_t &spots_;
    NodeConfig &config_;

    fx::task_t<MatchResponse> prepare (const std::string &room)
    {
        const auto created =
          co_await spots_.get_or_create (room, Names::room).in_mesh (Names::mesh).async ();
        if (created.state == fx::spot_create_state_t::rejected)
            throw std::runtime_error ("Room preparation rejected");
        co_return MatchResponse{room, config_.SessionEndpoint};
    }

  public:
    using request_type = MatchRequest;
    using reply_type = MatchResponse;
    MatchHandler (Matchmaker &matches, fx::spot_manager_t &spots, NodeConfig &config) :
        matches_ (matches), spots_ (spots), config_ (config)
    {
    }
    fx::task_t<MatchResponse> handle (const MatchRequest &request)
    {
        auto ticket = matches_.enter (request);
        if (ticket.pending)
            co_return co_await ticket.pending->task ();
        auto failure = fx::result_t<MatchResponse>::failure (
          fx::framework_error_kind_t::internal_failure, "Room preparation failed");
        try {
            const auto assignment = co_await prepare (ticket.room);
            ticket.partner->complete (fx::result_t<MatchResponse>::success (assignment));
            co_return assignment;
        }
        catch (const fx::framework_exception_t &error) {
            failure = fx::result_t<MatchResponse>::failure (error.kind (), error.what ());
        }
        catch (const std::exception &error) {
            failure = fx::result_t<MatchResponse>::failure (
              fx::framework_error_kind_t::internal_failure, error.what ());
        }
        std::cerr << "HTTP matching failed: " << failure.error ()->what () << std::endl;
        ticket.partner->complete (failure);
        throw *failure.error ();
    }
};
}
