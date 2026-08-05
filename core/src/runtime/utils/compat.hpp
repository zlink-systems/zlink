/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_COMPAT_HPP_INCLUDED__
#define __ZLINK_COMPAT_HPP_INCLUDED__

#include "utils/precompiled.hpp"
#include <string.h>

#ifdef ZLINK_HAVE_WINDOWS
#define strcasecmp _stricmp
#define strtok_r strtok_s
#else
#ifndef ZLINK_HAVE_STRLCPY
#ifdef ZLINK_HAVE_LIBBSD
#include <bsd/string.h>
#else
static inline size_t strlcpy (char *dest_, const char *src_, const size_t dest_size_)
{
    size_t remain = dest_size_;
    for (; remain && *src_; --remain, ++src_, ++dest_) {
        *dest_ = *src_;
    }
    return dest_size_ - remain;
}
#endif
#endif
template <size_t size> static inline int strcpy_s (char (&dest_)[size], const char *const src_)
{
    const size_t res = strlcpy (dest_, src_, size);
    return res >= size ? ERANGE : 0;
}
#endif

#ifndef HAVE_STRNLEN
static inline size_t strnlen (const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\0')
            return i + 1;
    }

    return len;
}
#endif

#endif
