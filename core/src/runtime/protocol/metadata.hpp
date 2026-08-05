/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_METADATA_HPP_INCLUDED__
#define __ZLINK_METADATA_HPP_INCLUDED__

#include <map>
#include <string>

#include "utils/atomic_counter.hpp"

namespace zlink
{
class metadata_t
{
  public:
    typedef std::map<std::string, std::string> dict_t;

    metadata_t (const dict_t &dict_);

    //  Returns pointer to property value or NULL if
    //  property is not found.
    const char *get (const std::string &property_) const;

    void add_ref ();

    //  Drop reference. Returns true iff the reference
    //  counter drops to zero.
    bool drop_ref ();

  private:
    //  Reference counter.
    atomic_counter_t _ref_cnt;

    //  Dictionary holding metadata.
    const dict_t _dict;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (metadata_t)
};
}

#endif
