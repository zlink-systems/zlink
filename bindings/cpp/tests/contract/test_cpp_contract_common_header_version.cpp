/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/common.h>
#include <zlink/socket/api.h>

#include <type_traits>

static_assert(ZLINK_VERSION_MAJOR == 0, "zlink/common.h major version must match Core version");
static_assert(ZLINK_VERSION_MINOR == 17, "zlink/common.h minor version must match Core version");
static_assert(ZLINK_VERSION_PATCH == 0, "zlink/common.h patch version must match Core version");
static_assert(ZLINK_VERSION == ZLINK_MAKE_VERSION(0, 17, 0),
  "zlink/common.h aggregate version must match Core version");
static_assert(ZLINK_COMPLETION_WRITABLE == 3,
  "the public raw header must expose the WRITABLE completion ABI value");
static_assert(
  std::is_same<decltype (ZLINK_COMPLETION_WRITABLE), zlink_completion_kind_t>::value,
  "WRITABLE must be a zlink_completion_kind_t enumerator");

int main()
{
    return 0;
}
