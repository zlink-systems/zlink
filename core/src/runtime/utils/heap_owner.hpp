/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_HEAP_OWNER_HPP_INCLUDED__
#define __ZLINK_HEAP_OWNER_HPP_INCLUDED__

namespace zlink
{
template <typename T> void release_heap_owned (T *object_)
{
    delete object_;
}
}

#endif
