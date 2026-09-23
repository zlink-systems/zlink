#pragma once

namespace engine_lobby {
namespace packet {
inline constexpr char ping_req[] = "PingReq";
inline constexpr char ping_res[] = "PingRes";
inline constexpr char join_req[] = "JoinReq";
inline constexpr char join_res[] = "JoinRes";
inline constexpr char chat_msg[] = "ChatMsg";
inline constexpr char chat_notify[] = "ChatNotify";
} // namespace packet

namespace field {
inline constexpr char sent_at_unix_ms[] = "sentAtUnixMs";
inline constexpr char actor_id[] = "actorId";
inline constexpr char name[] = "name";
inline constexpr char text[] = "text";
} // namespace field
} // namespace engine_lobby
