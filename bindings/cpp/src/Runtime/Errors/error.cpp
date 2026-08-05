/* SPDX-License-Identifier: MPL-2.0 */
#include "zlink/Contracts/Errors/errors.hpp"

#include <zlink.h>

namespace zlink::detail
{

int current_errno () noexcept
{
    return zlink_errno ();
}

} // namespace zlink::detail
