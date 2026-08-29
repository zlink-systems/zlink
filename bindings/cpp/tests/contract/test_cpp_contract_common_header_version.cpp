/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/common.h>

static_assert(ZLINK_VERSION_MAJOR == 0, "zlink/common.h major version must match Core version");
static_assert(ZLINK_VERSION_MINOR == 14, "zlink/common.h minor version must match Core version");
static_assert(ZLINK_VERSION_PATCH == 6, "zlink/common.h patch version must match Core version");
static_assert(ZLINK_VERSION == ZLINK_MAKE_VERSION(0, 14, 6),
  "zlink/common.h aggregate version must match Core version");

int main()
{
    return 0;
}
