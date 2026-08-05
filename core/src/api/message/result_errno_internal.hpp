/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_RESULT_ERRNO_INTERNAL_HPP_INCLUDED
#define ZLINK_RESULT_ERRNO_INTERNAL_HPP_INCLUDED

#include "core/internal_errno.hpp"

namespace zlink
{
namespace result_errno_internal
{

inline int rc_errno_or_io ()
{
    const int saved_errno = errno;
    return saved_errno != 0 ? saved_errno : EIO;
}

inline bool is_not_supported (int err_)
{
    return err_ == ENOTSUP
#if !defined(EOPNOTSUPP) || EOPNOTSUPP != ENOTSUP
           || err_ == EOPNOTSUPP
#endif
      ;
}

}
}

#endif
