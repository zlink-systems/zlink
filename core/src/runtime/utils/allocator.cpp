/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/allocator.hpp"
#include "utils/macros.hpp"

#include <cstdlib>

namespace zlink
{
void *alloc (std::size_t size_)
{
    return std::malloc (size_);
}

void dealloc (void *ptr_)
{
    std::free (ptr_);
}

void *alloc_tl (std::size_t size_)
{
    return std::malloc (size_);
}

void dealloc_tl (void *ptr_)
{
    std::free (ptr_);
}
}
