#pragma once
#include "server/shared/http_lobby_contracts.hpp"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <cctype>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string_view>

namespace MyGame
{
inline bool blank (std::string_view value)
{
    return std::all_of (value.begin (), value.end (), [] (unsigned char c) {
        return std::isspace (c) != 0;
    });
}

class Matchmaker
{
  public:
    using Reply = std::function<void (const MatchResponse &)>;
    using Failure = std::function<void (std::exception_ptr)>;
    using Cancel = std::function<void ()>;

    struct Pairing
    {
        std::string roomId;
        std::vector<std::string> members;
        Reply replyFirst;
        Reply replySecond;
        Failure failFirst;
        Failure failSecond;
        Cancel cancelFirst;

        Pairing (std::string roomIdValue, std::vector<std::string> membersValue,
                 Reply replyFirstValue, Reply replySecondValue, Failure failFirstValue,
                 Failure failSecondValue, Cancel cancelFirstValue)
          : roomId (std::move (roomIdValue)), members (std::move (membersValue)),
            replyFirst (std::move (replyFirstValue)), replySecond (std::move (replySecondValue)),
            failFirst (std::move (failFirstValue)), failSecond (std::move (failSecondValue)),
            cancelFirst (std::move (cancelFirstValue))
        {
        }

        void cancel_waiter () { cancelFirst (); }

        void prepared (const MatchResponse &response)
        {
            release ();
            replyFirst (response);
            replySecond (response);
        }

        void failed (std::exception_ptr error)
        {
            release ();
            failFirst (error);
            failSecond (error);
        }

      private:
        friend class Matchmaker;
        std::function<void ()> release_;
        void release ()
        {
            if (release_) {
                auto release = std::move (release_);
                release ();
            }
        }
    };

    std::optional<Pairing> enqueue (const MatchRequest &request,
                                    const void *token,
                                    Reply reply,
                                    Failure fail,
                                    Cancel cancel)
    {
        if (blank (request.playerId) || blank (request.mode)
            || !participants_.insert (request.playerId).second)
            throw std::invalid_argument (
              "Nonempty player/mode and distinct pending player required");

        const auto waiting = queues_.find (request.mode);
        if (waiting == queues_.end ()) {
            queues_.emplace (request.mode,
                             Pending{request.playerId, token, std::move (reply), std::move (fail),
                                     std::move (cancel)});
            return std::nullopt;
        }

        auto partner = std::move (waiting->second);
        queues_.erase (waiting);
        std::vector<std::string> members{partner.player, request.playerId};
        const auto roomId = issue_room_id ();
        Pairing pairing{roomId, members, std::move (partner.reply), std::move (reply),
                        std::move (partner.fail), std::move (fail), std::move (partner.cancel)};
        pairing.release_ = [this, members = std::move (members)] {
            for (const auto &member : members)
                participants_.erase (member);
        };
        return pairing;
    }

    bool cancel (std::string_view mode, std::string_view player, const void *token)
    {
        const auto waiting = queues_.find (std::string (mode));
        if (waiting == queues_.end () || waiting->second.player != player
            || waiting->second.token != token)
            return false;
        participants_.erase (waiting->second.player);
        queues_.erase (waiting);
        return true;
    }

  private:
    struct Pending
    {
        std::string player;
        const void *token{};
        Reply reply;
        Failure fail;
        Cancel cancel;
    };

    static std::string issue_room_id ()
    {
        auto guid = boost::uuids::to_string (boost::uuids::random_generator{} ());
        guid.erase (std::remove (guid.begin (), guid.end (), '-'), guid.end ());
        return std::string (Names::room_prefix) + guid;
    }

    std::map<std::string, Pending> queues_;
    std::set<std::string> participants_;
};
}
