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
//  the receiving side to discard all incoming messages but the last one.
//
//  reader_awake flag is needed here to mimic ypipe delicate behaviour
//  around the reader being asleep (see 'c' pointer being NULL in ypipe.hpp)

template <typename T> class ypipe_conflate_t ZLINK_FINAL : public ypipe_base_t<T>
{
  public:
    //  Initialises the pipe.
    ypipe_conflate_t () : reader_awake (false) {}

    //  Following function (write) deliberately copies uninitialised data
    //  when used with zlink_msg. Initialising the VSM body for
    //  non-VSM messages won't be good for performance.

#ifdef ZLINK_HAVE_OPENVMS
#pragma message save
#pragma message disable(UNINIT)
#endif
    void write (const T &value_, bool incomplete_)
    {
        (void) incomplete_;

        dbuffer.write (value_);
    }

    void write_with_replacement_accounting (
      const T &value_,
      bool incomplete_,
      uint64_t (*accounted_bytes_) (const T &),
      bool (*counted_message_) (const T &),
      ypipe_replacement_accounting_t *replaced_) ZLINK_OVERRIDE
    {
        (void) incomplete_;
        zlink_assert (replaced_);
        dbuffer.write_with_replacement_accounting (
          value_, accounted_bytes_, counted_message_, &replaced_->bytes,
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

    // There are no incomplete items for conflate ypipe
    bool unwrite (T *) { return false; }

    //  Flush is no-op for conflate ypipe. Reader asleep behaviour
    //  is as of the usual ypipe.
    //  Returns false if the reader thread is sleeping. In that case,
    //  caller is obliged to wake the reader up before using the pipe again.
    bool flush () { return reader_awake; }

    //  Check whether item is available for reading.
    bool check_read ()
    {
        const bool res = dbuffer.check_read ();
        if (!res)
            reader_awake = false;

        return res;
    }

    //  Reads an item from the pipe. Returns false if there is no value.
    //  available.
    bool read (T *value_)
    {
        if (!check_read ())
            return false;

        return dbuffer.read (value_);
    }

    //  Applies the function fn to the first element in the pipe
    //  and returns the value returned by the fn.
    //  The pipe mustn't be empty or the function crashes.
    bool probe (bool (*fn_) (const T &)) { return dbuffer.probe (fn_); }

  protected:
    dbuffer_t<T> dbuffer;
    bool reader_awake;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (ypipe_conflate_t)
};
}

#endif
