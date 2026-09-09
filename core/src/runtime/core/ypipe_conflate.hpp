/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_YPIPE_CONFLATE_HPP_INCLUDED__
#define __ZLINK_YPIPE_CONFLATE_HPP_INCLUDED__

#include "platform.hpp"
#include "utils/dbuffer.hpp"
#include "core/ypipe_base.hpp"

namespace zlink
{
//  Adapter for dbuffer, to plug it in instead of a queue for the sake
//  of implementing the conflate socket option, which, if set, makes
//  the receiving side to keep the latest complete record per topic.

template <typename T> class ypipe_conflate_t ZLINK_FINAL : public ypipe_base_t<T>
{
  public:
    ypipe_conflate_t () {}

    //  Following function (write) deliberately copies uninitialised data
    //  when used with zlink_msg. Initialising the VSM body for
    //  non-VSM messages won't be good for performance.

#ifdef ZLINK_HAVE_OPENVMS
#pragma message save
#pragma message disable(UNINIT)
#endif
    void write (const T &value_, bool incomplete_)
    {
        dbuffer.write (value_, incomplete_);
    }

    void write_with_replacement_accounting (
      const T &value_,
      bool incomplete_,
      uint64_t (*accounted_bytes_) (const T &),
      bool (*counted_message_) (const T &),
      ypipe_replacement_accounting_t *replaced_) ZLINK_OVERRIDE
    {
        zlink_assert (replaced_);
        dbuffer.write_with_replacement_accounting (
          value_, incomplete_, accounted_bytes_, counted_message_, &replaced_->bytes,
          &replaced_->complete_messages);
    }

    void discard_accounting (
      uint64_t (*accounted_bytes_) (const T &),
      bool (*counted_message_) (const T &),
      ypipe_replacement_accounting_t *discarded_) ZLINK_OVERRIDE
    {
        zlink_assert (discarded_);
        dbuffer.discard_accounting (
          accounted_bytes_, counted_message_, &discarded_->bytes,
          &discarded_->complete_messages);
    }

#ifdef ZLINK_HAVE_OPENVMS
#pragma message restore
#endif

    //  Rollback only the producer-owned incomplete record.
    bool unwrite (T *value_) { return dbuffer.unwrite (value_); }

    //  dbuffer has no atomic reader-sleep handshake. Requesting a wake for
    //  every publication prevents a writer from losing a concurrent empty
    //  observation by the reader.
    bool flush () { return false; }

    //  Check whether item is available for reading.
    bool check_read ()
    {
        return dbuffer.check_read ();
    }

    //  Reads an item from the pipe. Returns false if there is no value.
    //  available.
    bool read (T *value_, bool *prefetched_batch_exhausted_ = NULL)
    {
        return dbuffer.read (value_, prefetched_batch_exhausted_);
    }

    bool probe_if_published (void (*fn_) (const T &, void *),
                             void *userdata_)
    {
        //  dbuffer's read-side lock observes the already-published front
        //  without changing receiver state.
        return dbuffer.probe_if_published (fn_, userdata_);
    }

    ypipe_read_result_t
    read_if (T *value_, bool (*fn_) (const T &, void *), void *userdata_,
             bool *prefetched_batch_exhausted_ = NULL)
    {
        return dbuffer.read_if (value_, fn_, userdata_,
                                prefetched_batch_exhausted_);
    }

  protected:
    dbuffer_t<T> dbuffer;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (ypipe_conflate_t)
};
}

#endif
