/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/common.h>
#include <zlink/socket/api.h>

#include <type_traits>
#include <zlink/Contracts/Sockets/connection_attempt.hpp>
#include <zlink/Contracts/Eventing/poll_event.hpp>

static_assert(ZLINK_VERSION_MAJOR == 1, "zlink/common.h major version must match Core version");
static_assert(ZLINK_VERSION_MINOR == 20, "zlink/common.h minor version must match Core version");
static_assert(ZLINK_VERSION_PATCH == 0, "zlink/common.h patch version must match Core version");
static_assert(ZLINK_VERSION == ZLINK_MAKE_VERSION(1, 20, 0),
  "zlink/common.h aggregate version must match Core version");
static_assert(ZLINK_COMPLETION_WRITABLE == 3,
  "the public raw header must expose the WRITABLE completion ABI value");
static_assert(
  std::is_same<decltype (ZLINK_COMPLETION_WRITABLE), zlink_completion_kind_t>::value,
  "WRITABLE must be a zlink_completion_kind_t enumerator");

static_assert (static_cast<short> (zlink::poll_event_flag_t::pollconnect) == ZLINK_POLLCONNECT);
static_assert (ZLINK_POLLCONNECT == 128);
static_assert (std::is_same_v<decltype (zlink::connection_failure_t::normalized_errno), int32_t>);
static_assert (std::is_same_v<decltype (zlink::connection_failure_t::disconnect_reason), uint32_t>);
static_assert (std::is_same_v<decltype (zlink::connection_attempt_t::intent_id), uint64_t>);
static_assert (std::is_same_v<decltype (zlink::connection_attempt_t::revision), uint64_t>);
static_assert (std::is_same_v<decltype (zlink::connection_attempt_t::last_failure_id), uint64_t>);

int main()
{
    return 0;
}
