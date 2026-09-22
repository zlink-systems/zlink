#pragma once

namespace engine_lobby
{
namespace packet
{
inline constexpr char ping[] = "Ping";
inline constexpr char pong[] = "Pong";
inline constexpr char join[] = "Join";
inline constexpr char joined[] = "Joined";
inline constexpr char chat[] = "Chat";
inline constexpr char chat_notify[] = "ChatNotify";
} // namespace packet

namespace field
{
inline constexpr char sent_at_unix_ms[] = "sentAtUnixMs";
inline constexpr char actor_id[] = "actorId";
inline constexpr char name[] = "name";
inline constexpr char text[] = "text";
} // namespace field
} // namespace engine_lobby
