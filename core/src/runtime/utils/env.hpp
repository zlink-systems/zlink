/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_UTILS_ENV_HPP_INCLUDED__
#define __ZLINK_UTILS_ENV_HPP_INCLUDED__

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdint.h>

namespace zlink
{
namespace env
{
inline bool flag_enabled (const char *name_)
{
    const char *value = std::getenv (name_);
    return value && *value && *value != '0';
}

inline int positive_int (const char *name_, int fallback_)
{
    const char *value = std::getenv (name_);
    if (!value || !*value)
        return fallback_;

    char *end = NULL;
    const long parsed = std::strtol (value, &end, 10);
    if (!end || end == value || parsed <= 0 || parsed > INT_MAX)
        return fallback_;
    return static_cast<int> (parsed);
}

inline int non_negative_int (const char *name_, int fallback_)
{
    const char *value = std::getenv (name_);
    if (!value || !*value)
        return fallback_;

    char *end = NULL;
    const long parsed = std::strtol (value, &end, 10);
    if (!end || end == value || parsed < 0 || parsed > INT_MAX)
        return fallback_;
    return static_cast<int> (parsed);
}

inline size_t positive_size (const char *name_,
                             size_t fallback_,
                             size_t max_ = std::numeric_limits<size_t>::max ())
{
    const char *value = std::getenv (name_);
    if (!value || !*value)
        return fallback_;

    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (value, &end, 10);
    if (errno != 0 || end == value || parsed == 0)
        return fallback_;
    if (parsed > static_cast<unsigned long long> (max_))
        return max_;
    return static_cast<size_t> (parsed);
}

inline unsigned int
positive_uint (const char *name_, unsigned int fallback_, unsigned int max_ = UINT_MAX)
{
    const char *value = std::getenv (name_);
    if (!value || !*value)
        return fallback_;

    char *end = NULL;
    const unsigned long parsed = std::strtoul (value, &end, 10);
    if (!end || end == value || *end != '\0' || parsed == 0)
        return fallback_;
    if (parsed > max_)
        return max_;
    return static_cast<unsigned int> (parsed);
}

inline uint64_t positive_u64 (const char *name_, uint64_t fallback_)
{
    const char *value = std::getenv (name_);
    if (!value || !*value)
        return fallback_;

    char *end = NULL;
    const unsigned long parsed = std::strtoul (value, &end, 10);
    if (end == value || parsed == 0)
        return fallback_;
    return static_cast<uint64_t> (parsed);
}

inline size_t asio_stream_accept_concurrency ()
{
    return positive_size ("ZLINK_ASIO_STREAM_ACCEPT_CONCURRENCY", 4, 128);
}

inline bool equals (const char *name_, const char *lower_, const char *upper_)
{
    const char *value = std::getenv (name_);
    if (!value || !*value)
        return false;
    return std::strcmp (value, lower_) == 0 || std::strcmp (value, upper_) == 0;
}
}
}

#endif
