/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_WS_BATCH_POLICY_HPP_INCLUDED__
#define __ZLINK_WS_BATCH_POLICY_HPP_INCLUDED__

#include <stddef.h>

namespace zlink
{
namespace ws_batch_policy
{
// One encoder batch becomes one Beast binary write. Keep the established
// small-connection footprint, then let the existing encoder target policy
// grow sustained full batches up to this bounded maximum.
inline int zmp_send_batch_size ()
{
    return 16 * 1024;
}

inline size_t zmp_send_batch_max_size ()
{
    return 128 * 1024;
}

inline size_t write_buffer_initial_size ()
{
    return 64 * 1024;
}
}
}

#endif
