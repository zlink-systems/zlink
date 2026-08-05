/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_OWN_HPP_INCLUDED__
#define __ZLINK_OWN_HPP_INCLUDED__

#include <set>

#include "core/object.hpp"
#include "core/options.hpp"
#include "utils/atomic_counter.hpp"
#include "utils/stdint.hpp"

namespace zlink
{
class ctx_t;
class io_thread_t;
#ifdef ZLINK_BUILD_TESTS
class session_termination_test_access_t;
#endif

//  Base class for objects forming a part of ownership hierarchy.
//  It handles initialisation and destruction of such objects.

class own_t : public object_t
{
    template <typename T> friend void release_heap_owned (T *);
#ifdef ZLINK_BUILD_TESTS
    friend class session_termination_test_access_t;
#endif

  public:
    //  Note that the owner is unspecified in the constructor.
    //  It'll be supplied later on when the object is plugged in.

    //  The object is not living within an I/O thread. It has it's own
    //  thread outside of 0MQ infrastructure.
    own_t (zlink::ctx_t *parent_, uint32_t tid_);

    //  The object is living within I/O thread.
    own_t (zlink::io_thread_t *io_thread_, const options_t &options_);

    //  When another owned object wants to send command to this object
    //  it calls this function to let it know it should not shut down
    //  before the command is delivered.
    void inc_seqnum ();

    //  Use following two functions to wait for arbitrary events before
    //  terminating. Just add number of events to wait for using
    //  register_tem_acks functions. When event occurs, call
    //  remove_term_ack. When number of pending acks reaches zero
    //  object will be deallocated.
    void register_term_acks (int count_);
    void unregister_term_ack ();

  protected:
    //  Launch the supplied object and become its owner.
    void launch_child (own_t *object_);

    //  Terminate owned object
    void term_child (own_t *object_);

    //  Ask owner object to terminate this object. It may take a while
    //  while actual termination is started. This function should not be
    //  called more than once.
    void terminate ();

    //  Returns true if the object is in process of termination.
    bool is_terminating () const;

    //  Derived object destroys own_t. There's no point in allowing
    //  others to invoke the destructor. At the same time, it has to be
    //  virtual so that generic own_t deallocation mechanism destroys
    //  specific type of the owned object correctly.
    ~own_t () ZLINK_OVERRIDE;

    //  Term handler is protected rather than private so that it can
    //  be intercepted by the derived class. This is useful to add custom
    //  steps to the beginning of the termination process.
    void process_term (int linger_) ZLINK_OVERRIDE;

    //  A place to hook in when physical destruction of the object
    //  is to be delayed.
    virtual void process_destroy ();

    //  Socket options associated with this object.
    options_t options;

  private:
    //  Set owner of the object
    void set_owner (own_t *owner_);

    //  Handlers for incoming commands.
    void process_own (own_t *object_) ZLINK_OVERRIDE;
    void process_term_req (own_t *object_) ZLINK_OVERRIDE;
    void process_term_ack () ZLINK_OVERRIDE;
    void process_seqnum () ZLINK_OVERRIDE;

    //  Check whether all the pending term acks were delivered.
    //  If so, deallocate this object.
    void check_term_acks ();

    //  True if termination was already initiated. If so, we can destroy
    //  the object if there are no more child objects or pending term acks.
    bool _terminating;

    //  Sequence number of the last command sent to this object.
    atomic_counter_t _sent_seqnum;

    //  Sequence number of the last command processed by this object.
    uint64_t _processed_seqnum;

    //  Socket owning this object. It's responsible for shutting down
    //  this object.
    own_t *_owner;

    //  List of all objects owned by this socket. We are responsible
    //  for deallocating them before we quit.
    typedef std::set<own_t *> owned_t;
    owned_t _owned;

    //  Number of events we have to get before we can destroy the object.
    int _term_acks;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (own_t)
};
}

#endif
