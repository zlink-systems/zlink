/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_DBUFFER_HPP_INCLUDED__
#define __ZLINK_DBUFFER_HPP_INCLUDED__

#include <atomic>
#include <new>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vector>

#include "utils/err.hpp"
#include "core/msg.hpp"
#include "core/ypipe.hpp"
#include "core/ypipe_base.hpp"

namespace zlink
{
//  Single-producer/single-consumer conflation buffer. The producer assembles
//  a complete record before publication. Only unread records of the same
//  topic are replaced, and a replacement keeps the replaced record's position
//  in receive order; a reader owns the remainder of a started record.
//
//  Receive order is an SPSC ypipe of nodes. A control record (delimiter,
//  command, routing id, credential, subscribe, cancel) is its own node and is
//  never replaced. A topic cell holds the latest unread record of one topic
//  in `latest`: the writer exchanges a new record in and frees the unread one
//  it takes out, or queues the cell when it takes out NULL. The reader claims
//  a record by exchanging NULL in, and a conditional read that rejects the
//  record puts it back with a CAS.
//
//  The reader returns every record it finishes, and every queue entry, to
//  the writer through a second SPSC queue; the writer reuses those records.
template <typename T> class dbuffer_t;

template <> class dbuffer_t<msg_t>
{
  public:
    dbuffer_t () :
        _cell_count (0),
        _spare (NULL),
        _spare_cells (NULL),
        _spare_count (0),
        _spare_cell_count (0),
        _front (NULL),
        _started (NULL)
    {
    }

    ~dbuffer_t ()
    {
        uint64_t ignored_bytes = 0;
        uint64_t ignored_messages = 0;
        discard (NULL, NULL, &ignored_bytes, &ignored_messages);
        for (size_t i = 0; i != _pending.size (); ++i) {
            const int rc = _pending[i].close ();
            zlink_assert (rc == 0);
        }
    }

    void write (const msg_t &value_, bool incomplete_)
    {
        uint64_t ignored_bytes = 0;
        uint64_t ignored_messages = 0;
        write_with_replacement_accounting (
          value_, incomplete_, NULL, NULL, &ignored_bytes, &ignored_messages);
    }

    void write_with_replacement_accounting (
      const msg_t &value_, bool incomplete_,
      uint64_t (*accounted_bytes_) (const msg_t &),
      bool (*counted_message_) (const msg_t &),
      uint64_t *replaced_bytes_, uint64_t *replaced_messages_)
    {
        zlink_assert (value_.check ());
        zlink_assert (replaced_bytes_ && replaced_messages_);
        *replaced_bytes_ = 0;
        *replaced_messages_ = 0;
        _pending.push_back (value_);
        if (incomplete_)
            return;

        collect_returns (accounted_bytes_, counted_message_, replaced_bytes_,
                         replaced_messages_);
        record_t *const record = take_record (_pending.size ());
        record->assign (_pending);
        _pending.clear ();
        cell_t *const cell = find_or_insert_cell (*record);
        if (!cell) {
            _queue.write (record, false);
            return;
        }
        record_t *const unread =
          cell->latest.exchange (record, std::memory_order_acq_rel);
        if (unread) {
            unread->account (accounted_bytes_, counted_message_,
                             replaced_bytes_, replaced_messages_);
            spare_record (unread);
            return;
        }
        ++cell->refs;
        _queue.write (cell, false);
    }

    bool unwrite (msg_t *value_)
    {
        if (_pending.empty ())
            return false;
        *value_ = _pending.back ();
        _pending.pop_back ();
        return true;
    }

    //  Publishes queued nodes. Returns false if the reader was asleep, in
    //  which case the caller wakes it (ypipe_t::flush).
    bool flush () { return _queue.flush (); }

    bool read (msg_t *value_, bool *batch_exhausted_ = NULL)
    {
        return read_if (value_, NULL, NULL, batch_exhausted_)
               == ypipe_read_consumed;
    }

    //  Each reader operation publishes the returns it made before it ends.
    //  The writer polls the return queue without sleeping, so the result of
    //  that flush never requires a wake.
    bool check_read ()
    {
        const bool readable = _started || advance (true);
        _returns.flush ();
        return readable;
    }

    bool probe_if_published (void (*fn_) (const msg_t &, void *),
                             void *userdata_)
    {
        const bool published = probe_front (fn_, userdata_);
        _returns.flush ();
        return published;
    }

    ypipe_read_result_t
    read_if (msg_t *value_, bool (*fn_) (const msg_t &, void *), void *userdata_,
             bool *batch_exhausted_ = NULL)
    {
        const ypipe_read_result_t result =
          read_front (value_, fn_, userdata_, batch_exhausted_);
        _returns.flush ();
        return result;
    }

    //  Both ends are quiescent when a pipe is discarded. Every unread record
    //  is freed here, and its remaining frames are charged exactly once.
    void discard_accounting (uint64_t (*accounted_bytes_) (const msg_t &),
                             bool (*counted_message_) (const msg_t &),
                             uint64_t *discarded_bytes_,
                             uint64_t *discarded_messages_)
    {
        zlink_assert (discarded_bytes_ && discarded_messages_);
        *discarded_bytes_ = 0;
        *discarded_messages_ = 0;
        // Pending frames have provisional accounting, released by rollback.
        discard (accounted_bytes_, counted_message_, discarded_bytes_,
                 discarded_messages_);
    }

  private:
    struct node_t
    {
        explicit node_t (bool cell_) : cell (cell_) {}
        const bool cell;
    };

    //  A complete record and its frames in one allocation.
    struct record_t : node_t
    {
        explicit record_t (size_t capacity_) :
            node_t (false),
            capacity (capacity_),
            size (0),
            read_pos (0),
            next_spare (NULL)
        {
        }

        static size_t frames_offset ()
        {
            return (sizeof (record_t) + alignof (msg_t) - 1)
                   & ~(alignof (msg_t) - 1);
        }

        msg_t *frames ()
        {
            return reinterpret_cast<msg_t *> (reinterpret_cast<char *> (this)
                                              + frames_offset ());
        }

        static record_t *allocate (size_t capacity_)
        {
            void *const storage = ::operator new (
              frames_offset () + capacity_ * sizeof (msg_t), std::nothrow);
            alloc_assert (storage);
            return new (storage) record_t (capacity_);
        }

        static void deallocate (record_t *record_)
        {
            zlink_assert (record_->read_pos == record_->size);
            record_->~record_t ();
            ::operator delete (record_);
        }

        void assign (const std::vector<msg_t> &frames_)
        {
            zlink_assert (frames_.size () <= capacity);
            for (size_t i = 0; i != frames_.size (); ++i)
                new (&frames ()[i]) msg_t (frames_[i]);
            size = frames_.size ();
            read_pos = 0;
        }

        //  Closes the frames the reader has not taken.
        void release ()
        {
            for (; read_pos != size; ++read_pos) {
                const int rc = frames ()[read_pos].close ();
                zlink_assert (rc == 0);
            }
        }

        void account (uint64_t (*bytes_) (const msg_t &),
                      bool (*messages_) (const msg_t &), uint64_t *total_bytes_,
                      uint64_t *total_messages_)
        {
            for (size_t i = read_pos; i != size; ++i) {
                if (bytes_) {
                    const uint64_t bytes = bytes_ (frames ()[i]);
                    *total_bytes_ = UINT64_MAX - *total_bytes_ < bytes
                                      ? UINT64_MAX : *total_bytes_ + bytes;
                }
                if (messages_ && messages_ (frames ()[i]))
                    ++*total_messages_;
            }
        }

        const size_t capacity;
        size_t size;
        size_t read_pos;
        //  Writer-owned link of the spare record list.
        record_t *next_spare;
        ZLINK_NON_COPYABLE_NOR_MOVABLE (record_t)
    };

    //  The latest unread record of one topic; the key bytes follow the cell.
    struct cell_t : node_t
    {
        explicit cell_t (size_t key_capacity_) :
            node_t (true),
            latest (NULL),
            key_capacity (key_capacity_),
            hash (0),
            topic (false),
            key_size (0),
            refs (0),
            stale (0),
            next_spare (NULL)
        {
        }

        static cell_t *allocate (size_t key_capacity_)
        {
            void *const storage =
              ::operator new (sizeof (cell_t) + key_capacity_, std::nothrow);
            alloc_assert (storage);
            return new (storage) cell_t (key_capacity_);
        }

        static void deallocate (cell_t *cell_)
        {
            cell_->~cell_t ();
            ::operator delete (cell_);
        }

        unsigned char *key () { return reinterpret_cast<unsigned char *> (this + 1); }

        std::atomic<record_t *> latest;
        const size_t key_capacity;
        //  Writer-owned key, set when the cell enters the index.
        uint64_t hash;
        bool topic;
        size_t key_size;
        //  Writer-owned: queue entries and reader fronts not yet returned.
        size_t refs;
        //  Reader-owned: queue entries whose position the reader's front
        //  already holds (see unclaim_front).
        size_t stale;
        //  Writer-owned link of the spare cell list.
        cell_t *next_spare;
        ZLINK_NON_COPYABLE_NOR_MOVABLE (cell_t)
    };

    //  Each spare list keeps at most one ypipe chunk's worth of entries.
    enum
    {
        spare_cap = 64
    };
    typedef ypipe_t<node_t *, spare_cap> node_pipe_t;

    //  Returns the cell of a topic record, or NULL for a control record,
    //  which keeps its FIFO position and is never replaced.
    cell_t *find_or_insert_cell (record_t &record_)
    {
        msg_t &first = record_.frames ()[0];
        const unsigned char control =
          msg_t::command | msg_t::routing_id | msg_t::credential;
        if (first.is_delimiter () || (first.flags () & control)
            || first.is_subscribe () || first.is_cancel ())
            return NULL;
        // A multipart record is keyed by its first frame; all single-frame
        // records share one key.
        const bool topic = (first.flags () & msg_t::more) != 0;
        const unsigned char *const key =
          topic ? static_cast<const unsigned char *> (first.data ()) : NULL;
        const size_t key_size = topic ? first.size () : 0;
        uint64_t hash = topic ? 14695981039346656037ULL : 1099511628211ULL;
        for (size_t i = 0; i != key_size; ++i)
            hash = (hash ^ key[i]) * 1099511628211ULL;

        size_t slot = find_slot (hash, topic, key, key_size);
        if (_cells[slot])
            return _cells[slot];
        if ((_cell_count + 1) * 2 > _cells.size ()) {
            rehash (_cells.empty () ? 8 : _cells.size () * 2);
            slot = find_slot (hash, topic, key, key_size);
        }
        cell_t *const cell = take_cell (key_size);
        cell->hash = hash;
        cell->topic = topic;
        cell->key_size = key_size;
        if (key_size)
            memcpy (cell->key (), key, key_size);
        _cells[slot] = cell;
        ++_cell_count;
        return cell;
    }

    //  Linear probing: the slot holding the key, or the empty slot for it.
    size_t find_slot (uint64_t hash_, bool topic_, const unsigned char *key_,
                      size_t key_size_)
    {
        if (_cells.empty ())
            rehash (8);
        const size_t mask = _cells.size () - 1;
        size_t slot = static_cast<size_t> (hash_) & mask;
        for (cell_t *cell; (cell = _cells[slot]) != NULL;
             slot = (slot + 1) & mask)
            if (cell->hash == hash_ && cell->topic == topic_
                && cell->key_size == key_size_
                && (key_size_ == 0
                    || memcmp (cell->key (), key_, key_size_) == 0))
                return slot;
        return slot;
    }

    void rehash (size_t capacity_)
    {
        std::vector<cell_t *> cells (capacity_, static_cast<cell_t *> (NULL));
        cells.swap (_cells);
        const size_t mask = capacity_ - 1;
        for (size_t i = 0; i != cells.size (); ++i) {
            if (!cells[i])
                continue;
            size_t slot = static_cast<size_t> (cells[i]->hash) & mask;
            while (_cells[slot])
                slot = (slot + 1) & mask;
            _cells[slot] = cells[i];
        }
    }

    void erase_cell (cell_t *cell_)
    {
        zlink_assert (!cell_->latest.load (std::memory_order_relaxed));
        const size_t mask = _cells.size () - 1;
        size_t hole = static_cast<size_t> (cell_->hash) & mask;
        while (_cells[hole] != cell_)
            hole = (hole + 1) & mask;
        // Backward-shift deletion keeps every probe chain unbroken.
        _cells[hole] = NULL;
        for (size_t next = (hole + 1) & mask; _cells[next];
             next = (next + 1) & mask) {
            const size_t home = static_cast<size_t> (_cells[next]->hash) & mask;
            if (((next - home) & mask) >= ((next - hole) & mask)) {
                _cells[hole] = _cells[next];
                _cells[next] = NULL;
                hole = next;
            }
        }
        --_cell_count;
        zlink_assert (cell_->refs == 0 && cell_->stale == 0);
        spare_cell (cell_);
        // The table never shrinks below the size that holds spare_cap cells,
        // so cells cycling through the spare list do not rehash.
        if (_cells.size () > 2 * spare_cap && _cell_count * 8 < _cells.size ())
            rehash (_cells.size () / 2);
    }

    bool probe_front (void (*fn_) (const msg_t &, void *), void *userdata_)
    {
        if (_started) {
            (*fn_) (_started->frames ()[_started->read_pos], userdata_);
            return true;
        }
        if (!advance (false))
            return false;
        record_t *const record = claim_front ();
        (*fn_) (record->frames ()[0], userdata_);
        unclaim_front (record);
        return true;
    }

    ypipe_read_result_t
    read_front (msg_t *value_, bool (*fn_) (const msg_t &, void *),
                void *userdata_, bool *batch_exhausted_)
    {
        if (batch_exhausted_)
            *batch_exhausted_ = false;
        if (!value_)
            return ypipe_read_rejected;
        record_t *record = _started;
        if (record) {
            if (fn_ && !(*fn_) (record->frames ()[record->read_pos], userdata_))
                return ypipe_read_rejected;
        } else {
            if (!advance (true))
                return ypipe_read_empty;
            record = claim_front ();
            if (fn_ && !(*fn_) (record->frames ()[0], userdata_)) {
                unclaim_front (record);
                return ypipe_read_rejected;
            }
            if (_front->cell)
                return_node (_front);
            _front = NULL;
            _started = record;
        }
        msg_t &front = record->frames ()[record->read_pos];
        *value_ = front;
        front.init (); // Ownership moves to the reader.
        if (++record->read_pos == record->size) {
            return_node (record);
            _started = NULL;
            if (batch_exhausted_)
                *batch_exhausted_ = true;
        }
        return ypipe_read_consumed;
    }

    //  Cells are reclaimed through a second SPSC queue in which the reader
    //  returns each queue entry and record it no longer uses, so the writer
    //  alone owns the topic index and frees a cell whose entries have all
    //  come back.
    void return_node (node_t *node_) { _returns.write (node_, false); }

    //  Records and cells are allocated only when their spare list is empty,
    //  and a spare list holds at most spare_cap entries; beyond that a
    //  finished record or cell is freed. The spares therefore add at most
    //  spare_cap records and spare_cap cells to those unread or queued for
    //  return. A spare too small for the new record or key is freed and
    //  replaced by one of the needed size.
    cell_t *take_cell (size_t key_size_)
    {
        cell_t *const spare = _spare_cells;
        if (spare) {
            _spare_cells = spare->next_spare;
            --_spare_cell_count;
            if (spare->key_capacity >= key_size_)
                return spare;
            cell_t::deallocate (spare);
        }
        return cell_t::allocate (key_size_);
    }

    record_t *take_record (size_t size_)
    {
        record_t *const spare = _spare;
        if (spare) {
            _spare = spare->next_spare;
            --_spare_count;
            if (spare->capacity >= size_)
                return spare;
            record_t::deallocate (spare);
        }
        return record_t::allocate (size_);
    }

    void spare_record (record_t *record_)
    {
        record_->release ();
        if (_spare_count == spare_cap) {
            record_t::deallocate (record_);
            return;
        }
        record_->next_spare = _spare;
        _spare = record_;
        ++_spare_count;
    }

    void spare_cell (cell_t *cell_)
    {
        if (_spare_cell_count == spare_cap) {
            cell_t::deallocate (cell_);
            return;
        }
        cell_->next_spare = _spare_cells;
        _spare_cells = cell_;
        ++_spare_cell_count;
    }

    //  Writer side: the frames a returned record still holds were superseded
    //  and are charged as a replacement; a cell whose last entry came back
    //  leaves the index.
    void collect_returns (uint64_t (*accounted_bytes_) (const msg_t &),
                          bool (*counted_message_) (const msg_t &),
                          uint64_t *bytes_, uint64_t *messages_)
    {
        node_t *node;
        while (_returns.probe_if_published (&ignore_node, NULL)
               && _returns.read (&node)) {
            if (!node->cell) {
                record_t *const record = static_cast<record_t *> (node);
                record->account (accounted_bytes_, counted_message_, bytes_,
                                 messages_);
                spare_record (record);
                continue;
            }
            cell_t *const cell = static_cast<cell_t *> (node);
            zlink_assert (cell->refs > 0);
            if (--cell->refs == 0)
                erase_cell (cell);
        }
    }

    static void ignore_node (node_t *const &, void *) {}

    //  Reader side: makes the next receive-order node the front. Only a
    //  sleeping advance may publish the ypipe sleep marker.
    bool advance (bool may_sleep_)
    {
        while (!_front) {
            if (!may_sleep_ && !_queue.probe_if_published (&ignore_node, NULL))
                return false;
            node_t *node;
            if (!_queue.read (&node))
                return false;
            if (node->cell && static_cast<cell_t *> (node)->stale) {
                --static_cast<cell_t *> (node)->stale;
                return_node (node);
                continue;
            }
            _front = node;
        }
        return true;
    }

    //  The front cell always holds a record: only the reader clears
    //  `latest`, and it keeps a cell as front only while `latest` is set.
    record_t *claim_front ()
    {
        if (!_front->cell)
            return static_cast<record_t *> (_front);
        record_t *const record = static_cast<cell_t *> (_front)->latest.exchange (
          NULL, std::memory_order_acq_rel);
        zlink_assert (record);
        return record;
    }

    //  Returns a claimed, unstarted record to the front cell. If the writer
    //  published a newer record meanwhile, the newer record replaces this
    //  one at the front, and the writer's queue entry for it is stale.
    void unclaim_front (record_t *record_)
    {
        if (!_front->cell)
            return;
        cell_t *const cell = static_cast<cell_t *> (_front);
        record_t *expected = NULL;
        if (cell->latest.compare_exchange_strong (expected, record_,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed))
            return;
        ++cell->stale;
        return_node (record_);
    }

    void discard (uint64_t (*accounted_bytes_) (const msg_t &),
                  bool (*counted_message_) (const msg_t &), uint64_t *bytes_,
                  uint64_t *messages_)
    {
        _returns.flush ();
        collect_returns (accounted_bytes_, counted_message_, bytes_,
                         messages_);
        if (_started) {
            _started->account (accounted_bytes_, counted_message_, bytes_,
                               messages_);
            spare_record (_started);
            _started = NULL;
        }
        // Cells are released through the index below.
        _queue.flush ();
        for (node_t *node = _front; node || _queue.read (&node); node = NULL) {
            if (node->cell)
                continue;
            record_t *const record = static_cast<record_t *> (node);
            record->account (accounted_bytes_, counted_message_, bytes_,
                             messages_);
            spare_record (record);
        }
        _front = NULL;
        for (size_t i = 0; i != _cells.size (); ++i) {
            cell_t *const cell = _cells[i];
            if (!cell)
                continue;
            record_t *const record =
              cell->latest.exchange (NULL, std::memory_order_acq_rel);
            if (record) {
                record->account (accounted_bytes_, counted_message_, bytes_,
                                 messages_);
                spare_record (record);
            }
            cell_t::deallocate (cell);
        }
        _cells.clear ();
        _cell_count = 0;
        while (_spare_cells) {
            cell_t *const spare = _spare_cells;
            _spare_cells = spare->next_spare;
            cell_t::deallocate (spare);
        }
        _spare_cell_count = 0;
        while (_spare) {
            record_t *const spare = _spare;
            _spare = spare->next_spare;
            record_t::deallocate (spare);
        }
        _spare_count = 0;
    }

    //  Writer-owned.
    std::vector<msg_t> _pending;
    std::vector<cell_t *> _cells;
    size_t _cell_count;
    record_t *_spare;
    cell_t *_spare_cells;
    size_t _spare_count;
    size_t _spare_cell_count;

    //  Reader-owned.
    node_t *_front;
    record_t *_started;

    node_pipe_t _queue;
    node_pipe_t _returns;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (dbuffer_t)
};
}

#endif
