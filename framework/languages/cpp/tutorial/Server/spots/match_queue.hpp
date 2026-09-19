#pragma once

#include "../../Shared/contracts.hpp"

#include <zlink/framework.hpp>

#include <string>
#include <utility>
#include <vector>

namespace fw = zlink::framework;

// --8<-- [start:instance-spot-class]
// Unlike a room, a match queue is never created explicitly. The first message
// addressed to a queue id brings it into being and is then handled by it.
// Players do not join it as members; it only processes requests.
class match_queue_t : public fw::instance_spot_t
{
  public:
    explicit match_queue_t (
      fw::instance_spot_context_t context)
      : _context (std::move (context))
    {
    }

    fw::instance_spot_context_t &context ()
      noexcept override
    {
        return _context;
    }

    const fw::instance_spot_context_t &context () const
      noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_handler<&match_queue_t::join> ();
    }

    int waiting () const { return static_cast<int> (_waiting.size ()); }

  private:
    // --8<-- [start:instance-spot-handler]
    // Handlers are written the same way as room handlers. The return value is
    // the reply, and serial execution lets the queue update its state directly.
    match_queue_status_t join (const join_match_queue_t &request)
    {
        _waiting.push_back (request.player_id);
        return match_queue_status_t{waiting ()};
    }
    // --8<-- [end:instance-spot-handler]

    fw::instance_spot_context_t _context;
    std::vector<std::string> _waiting;
};
// --8<-- [end:instance-spot-class]
