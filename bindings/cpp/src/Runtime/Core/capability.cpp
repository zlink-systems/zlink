/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Core/capability.hpp>

#include <zlink.h>

namespace zlink
{

void version (int &major_, int &minor_, int &patch_)
{
    ::zlink_version (&major_, &minor_, &patch_);
}

const char *error_text (int errnum_) noexcept
{
    return ::zlink_strerror (errnum_);
}

bool has (const std::string &capability_)
{
    return zlink_has (capability_.c_str ());
}

} // namespace zlink
