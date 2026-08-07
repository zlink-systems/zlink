/* SPDX-License-Identifier: MPL-2.0 */

#pragma once

#include "addon_common_api.h"

#include <errno.h>

inline int classify_try_send_errno ()
{
    switch (zlink_errno ()) {
        case EAGAIN:
            return ZLINK_SUBMIT_BACKPRESSURED;
#ifdef ENOTCONN
        case ENOTCONN:
#endif
#ifdef EHOSTUNREACH
        case EHOSTUNREACH:
#endif
#ifdef ETIMEDOUT
        case ETIMEDOUT:
#endif
            return ZLINK_SUBMIT_NOT_CONNECTED;
        default:
            return -1;
    }
}
