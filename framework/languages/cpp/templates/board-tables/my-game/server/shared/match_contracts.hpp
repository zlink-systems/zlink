#pragma once
#include <zlink/json.hpp>
#include <string>
#include <vector>

namespace MyGame
{
struct MatchNames
{
    static constexpr auto pool = "Pool";
    static constexpr auto pool_key = "pool-board";
    static constexpr auto room_prefix = "room-";
    static constexpr auto cancelled = "cancelled";
    static constexpr auto expired = "expired";
    static constexpr auto disconnected = "disconnected";
};
struct FindMatch
{
    static constexpr auto packet_name = "FindMatch";
};
struct MatchQueued
{
    static constexpr auto packet_name = "MatchQueued";
    std::string ticketId;
};
struct CancelMatch
{
    static constexpr auto packet_name = "CancelMatch";
    std::string ticketId;
};
struct MatchCancelled
{
    static constexpr auto packet_name = "MatchCancelled";
    std::string ticketId;
    std::string reason;
};
struct MatchFound
{
    static constexpr auto packet_name = "MatchFound";
    std::string ticketId;
    std::string roomId;
    int seat = -1;
};
struct MatchRequest
{
    static constexpr auto packet_name = "MatchRequest";
    std::string requestKey;
    std::string playerId;
};
struct PrepareRoom
{
    static constexpr auto packet_name = "PrepareRoom";
    std::string assignmentId;
    std::vector<std::string> members;
};
struct RoomPrepared
{
    static constexpr auto packet_name = "RoomPrepared";
    std::string assignmentId;
    bool ok = false;
};
struct MatchAssigned
{
    static constexpr auto packet_name = "MatchAssigned";
    std::string ticketId;
    std::string assignmentId;
    std::string roomId;
};
struct EnterReport
{
    static constexpr auto packet_name = "EnterReport";
    std::string ticketId;
    std::string assignmentId;
    bool ok = false;
};
struct CommitRoom
{
    static constexpr auto packet_name = "CommitRoom";
    std::string assignmentId;
};
struct WithdrawRoom
{
    static constexpr auto packet_name = "WithdrawRoom";
    std::string assignmentId;
};
struct CancelTicket
{
    static constexpr auto packet_name = "CancelTicket";
    std::string ticketId;
    std::string reason;
};
}

namespace zlink::json
{
template <> struct dto_traits<::MyGame::FindMatch>
{
    static constexpr auto fields = std::tuple{};
};
template <> struct dto_traits<::MyGame::MatchQueued>
{
    static constexpr auto fields = std::tuple{field ("ticketId", &::MyGame::MatchQueued::ticketId)};
};
template <> struct dto_traits<::MyGame::CancelMatch>
{
    static constexpr auto fields = std::tuple{field ("ticketId", &::MyGame::CancelMatch::ticketId)};
};
template <> struct dto_traits<::MyGame::MatchCancelled>
{
    static constexpr auto fields =
      std::tuple{field ("ticketId", &::MyGame::MatchCancelled::ticketId),
                 field ("reason", &::MyGame::MatchCancelled::reason)};
};
template <> struct dto_traits<::MyGame::MatchFound>
{
    static constexpr auto fields = std::tuple{field ("ticketId", &::MyGame::MatchFound::ticketId),
                                              field ("roomId", &::MyGame::MatchFound::roomId),
                                              field ("seat", &::MyGame::MatchFound::seat)};
};
template <> struct dto_traits<::MyGame::MatchRequest>
{
    static constexpr auto fields =
      std::tuple{field ("requestKey", &::MyGame::MatchRequest::requestKey),
                 field ("playerId", &::MyGame::MatchRequest::playerId)};
};
template <> struct dto_traits<::MyGame::PrepareRoom>
{
    static constexpr auto fields =
      std::tuple{field ("assignmentId", &::MyGame::PrepareRoom::assignmentId),
                 field ("members", &::MyGame::PrepareRoom::members)};
};
template <> struct dto_traits<::MyGame::RoomPrepared>
{
    static constexpr auto fields =
      std::tuple{field ("assignmentId", &::MyGame::RoomPrepared::assignmentId),
                 field ("ok", &::MyGame::RoomPrepared::ok)};
};
template <> struct dto_traits<::MyGame::MatchAssigned>
{
    static constexpr auto fields =
      std::tuple{field ("ticketId", &::MyGame::MatchAssigned::ticketId),
                 field ("assignmentId", &::MyGame::MatchAssigned::assignmentId),
                 field ("roomId", &::MyGame::MatchAssigned::roomId)};
};
template <> struct dto_traits<::MyGame::EnterReport>
{
    static constexpr auto fields =
      std::tuple{field ("ticketId", &::MyGame::EnterReport::ticketId),
                 field ("assignmentId", &::MyGame::EnterReport::assignmentId),
                 field ("ok", &::MyGame::EnterReport::ok)};
};
template <> struct dto_traits<::MyGame::CommitRoom>
{
    static constexpr auto fields =
      std::tuple{field ("assignmentId", &::MyGame::CommitRoom::assignmentId)};
};
template <> struct dto_traits<::MyGame::WithdrawRoom>
{
    static constexpr auto fields =
      std::tuple{field ("assignmentId", &::MyGame::WithdrawRoom::assignmentId)};
};
template <> struct dto_traits<::MyGame::CancelTicket>
{
    static constexpr auto fields = std::tuple{field ("ticketId", &::MyGame::CancelTicket::ticketId),
                                              field ("reason", &::MyGame::CancelTicket::reason)};
};
}
