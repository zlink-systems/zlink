/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_MTRIE_HPP_INCLUDED__
#define __ZLINK_MTRIE_HPP_INCLUDED__

#include "utils/generic_mtrie.hpp"

#if __cplusplus >= 201103L || (defined(_MSC_VER) && _MSC_VER > 1600)
#define ZLINK_HAS_EXTERN_TEMPLATE 1
#else
#define ZLINK_HAS_EXTERN_TEMPLATE 0
#endif

namespace zlink
{
class pipe_t;

#if ZLINK_HAS_EXTERN_TEMPLATE
extern template class generic_mtrie_t<pipe_t>;
#endif

typedef generic_mtrie_t<pipe_t> mtrie_t;
}

#endif
