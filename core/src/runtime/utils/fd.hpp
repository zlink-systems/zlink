/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_FD_HPP_INCLUDED__
#define __ZLINK_FD_HPP_INCLUDED__

#if defined _WIN32
#include "utils/windows.hpp"
#endif

namespace zlink
{
typedef zlink_fd_t fd_t;

#ifdef ZLINK_HAVE_WINDOWS
#if defined _MSC_VER && _MSC_VER <= 1400
enum
{
    retired_fd = (fd_t) (~0)
};
#else
enum
#if _MSC_VER >= 1800
  : fd_t
#endif
{
    retired_fd = INVALID_SOCKET
};
#endif
#else
enum
{
    retired_fd = -1
};
#endif
}
#endif
