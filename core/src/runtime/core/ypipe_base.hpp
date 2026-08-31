
/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_YPIPE_BASE_HPP_INCLUDED__
#define __ZLINK_YPIPE_BASE_HPP_INCLUDED__

#include <stddef.h>
#include <stdint.h>

#include "utils/macros.hpp"

namespace zlink
{
struct ypipe_replacement_accounting_t
{
    ypipe_replacement_accounting_t () : bytes (0), complete_messages (0) {}

    uint64_t bytes;
    uint64_t complete_messages;
};

enum ypipe_read_result_t
{
    ypipe_read_empty,
    ypipe_read_rejected,
    ypipe_read_consumed
};

// ypipe_base abstracts ypipe and ypipe_conflate specific
// classes, one is selected according to a the conflate
// socket option

template <typename T> class ypipe_base_t
{
  public:
    virtual ~ypipe_base_t () ZLINK_DEFAULT;
    virtual void write (const T &value_, bool incomplete_) = 0;
    virtual void write_with_replacement_accounting (
      const T &value_,
      bool incomplete_,
      uint64_t (*accounted_bytes_) (const T &),
      bool (*counted_message_) (const T &),
      ypipe_replacement_accounting_t *replaced_)
    {
        if (replaced_) {
            replaced_->bytes = 0;
            replaced_->complete_messages = 0;
        }
        write (value_, incomplete_);
    }
    virtual void discard_accounting (
      uint64_t (*accounted_bytes_) (const T &),
      bool (*counted_message_) (const T &),
      ypipe_replacement_accounting_t *discarded_)
    {
        if (discarded_) {
            discarded_->bytes = 0;
            discarded_->complete_messages = 0;
        }
    }
    virtual bool unwrite (T *value_) = 0;
    virtual bool flush () = 0;
    virtual bool check_read () = 0;
    virtual bool read (T *value_) = 0;
    virtual bool probe (bool (*fn_) (const T &)) = 0;
    virtual ypipe_read_result_t
    read_if (T *value_, bool (*fn_) (const T &, void *), void *userdata_) = 0;
};
}

#endif
