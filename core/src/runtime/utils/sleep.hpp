/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_UTILS_SLEEP_HPP_INCLUDED__
#define __ZLINK_UTILS_SLEEP_HPP_INCLUDED__

#ifdef ZLINK_HAVE_WINDOWS
#include "utils/windows.hpp"
#else
#include <unistd.h>
#endif

namespace zlink
{
inline void sleep_ms (unsigned int ms_)
{
    if (ms_ == 0)
        return;
#ifdef ZLINK_HAVE_WINDOWS
    Sleep (ms_);
#else
    usleep (ms_ * 1000);
#endif
}
}

#endif
