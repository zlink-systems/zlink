/* SPDX-License-Identifier: MPL-2.0 */
// Sequential retry keeps both threads alive to prevent thread-id reuse.
#include <zlink.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "api/socket/socket_api_internal.hpp"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::printf ("CHECK failed %s:%d %s\n", __FILE__, __LINE__, #x);                       \
            std::exit (2);                                                                         \
        }                                                                                          \
    } while (0)

static const int receive_timeout_ms = 2000;
static const size_t retry_record_capacity = 3;
static const int first_concurrent_record_id = 10;
static const int second_concurrent_record_id = 11;
static const size_t whole_record_capacity = 128;
static const int stress_record_part_count = 64;
static const int stress_round_count = 200;
static int failures = 0;
static void verdict (const char *name_, bool ok_, const std::string &detail_)
{
    std::printf ("%s %s %s\n", ok_ ? "PASS" : "FAIL", name_, detail_.c_str ());
    if (!ok_)
        ++failures;
}

static void set_int (void *s_, zlink_option_t opt_, int v_)
{
    CHECK (zlink_set_option (s_, opt_, &v_, sizeof (v_)) == 0);
}

static void make_part (zlink_msg_t *m_, const std::string &s_)
{
    CHECK (zlink_msg_init_size (m_, s_.size ()) == 0);
    std::memcpy (zlink_msg_data (m_), s_.data (), s_.size ());
}

static void send_record (void *s_, int id_, int parts_)
{
    std::vector<zlink_msg_t> p (parts_);
    for (int i = 0; i < parts_; ++i)
        make_part (&p[i], "r" + std::to_string (id_) + "-" + std::to_string (i));
    CHECK (zlink_send (s_, p.data (), p.size (), ZLINK_SEND_FLAGS_NONE, NULL, NULL) == 0);
    zlink_multipart_close (p.data (), p.size ());
}

static std::string part_str (zlink_msg_t *m_)
{
    return std::string (static_cast<const char *> (zlink_msg_data (m_)), zlink_msg_size (m_));
}

// -1 when parts are not one contiguous record r<id>-0..n-1, else id.
static int record_id (zlink_msg_t *p_, size_t n_)
{
    int id = -1;
    for (size_t i = 0; i < n_; ++i) {
        int rid = -1, idx = -1;
        if (std::sscanf (part_str (&p_[i]).c_str (), "r%d-%d", &rid, &idx) != 2)
            return -1;
        if (i == 0)
            id = rid;
        if (rid != id || idx != static_cast<int> (i))
            return -1;
    }
    return id;
}

static void pair_sockets (void *ctx_, const char *ep_, void **rx_, void **tx_)
{
    *rx_ = zlink_socket (ctx_, ZLINK_SOCKET_PAIR);
    *tx_ = zlink_socket (ctx_, ZLINK_SOCKET_PAIR);
    CHECK (*rx_ && *tx_);
    set_int (*rx_, ZLINK_OPT_LINGER, 0);
    set_int (*tx_, ZLINK_OPT_LINGER, 0);
    CHECK (zlink_bind (*rx_, ep_) == 0);
    CHECK (zlink_connect (*tx_, ep_) == 0);
}

// ---- sequential retry from another thread -------------------------------

// Thread A makes the first call and stays alive until thread B's call
// returns, so B never reuses A's std::thread::id (a joined thread's id and
// pthread_t are reused by glibc, which would hide an owner-thread check).
template <typename First, typename Second>
static void first_then_other_live_thread (First first_, Second second_)
{
    std::promise<void> first_done, other_done;
    std::future<void> first_ready = first_done.get_future ();
    std::future<void> other_ready = other_done.get_future ();
    std::thread::id a_id, b_id;
    std::thread a ([&] {
        a_id = std::this_thread::get_id ();
        first_ ();
        first_done.set_value ();
        other_ready.wait ();
    });
    std::thread b ([&] {
        first_ready.wait ();
        b_id = std::this_thread::get_id ();
        second_ ();
        other_done.set_value ();
    });
    a.join ();
    b.join ();
    CHECK (a_id != b_id);
}

static void case_pair_retry_other_thread (void *ctx_, zlink_socket_type_t type_, const char *name_)
{
    void *rx, *tx;
    rx = zlink_socket (ctx_, type_);
    tx = zlink_socket (ctx_, type_ == ZLINK_SOCKET_STREAM ? ZLINK_SOCKET_PAIR : type_);
    CHECK (rx && tx);
    set_int (rx, ZLINK_OPT_LINGER, 0);
    set_int (tx, ZLINK_OPT_LINGER, 0);
    if (type_ == ZLINK_SOCKET_STREAM) {
        const zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_RAW;
        CHECK (zlink_set_stream_option (rx, ZLINK_STREAM_OPT_RECV_MODE, &mode, sizeof (mode)) == 0);
    }
    const std::string endpoint = std::string ("inproc://recv-admission-") + name_;
    CHECK (zlink_bind (rx, endpoint.c_str ()) == 0);
    CHECK (zlink_connect (tx, endpoint.c_str ()) == 0);
    const bool raw = type_ == ZLINK_SOCKET_STREAM;
    const size_t expected_count = raw ? 1 : retry_record_capacity;
    set_int (rx, ZLINK_OPT_RCVTIMEO, receive_timeout_ms);
    send_record (tx, 1, static_cast<int> (expected_count));
    zlink_msg_t small[1], large[retry_record_capacity];
    size_t n_small = 0, n_large = 0;
    int first = -1, other = -1;
    first_then_other_live_thread (
      [&] { first = zlink_recv (rx, NULL, small, raw ? 0 : 1, &n_small, ZLINK_RECV_FLAGS_NONE); },
      [&] {
          other =
            zlink_recv (rx, NULL, large, retry_record_capacity, &n_large, ZLINK_RECV_FLAGS_NONE);
      });
    const int id = other == ZLINK_RECV_OK ? record_id (large, n_large) : -2;
    if (other == ZLINK_RECV_OK)
        zlink_multipart_close (large, n_large);
    zlink_msg_t tail[retry_record_capacity];
    size_t n_tail = 0;
    const int after =
      zlink_recv (rx, NULL, tail, retry_record_capacity, &n_tail, ZLINK_RECV_FLAGS_DONTWAIT);
    if (after == ZLINK_RECV_OK)
        zlink_multipart_close (tail, n_tail);
    std::ostringstream d;
    d << "first=" << first << " needed=" << n_small << " other=" << other << " id=" << id
      << " after=" << after;
    verdict (name_,
             first == ZLINK_RECV_BUFFER_TOO_SMALL && other == ZLINK_RECV_OK && id == 1
               && n_small == expected_count && n_large == expected_count
               && after == ZLINK_RECV_NO_DATA,
             d.str ());
    zlink_close (tx);
    zlink_close (rx);
}

static void case_router_retry_other_thread (void *ctx_)
{
    void *router = zlink_socket (ctx_, ZLINK_SOCKET_ROUTER);
    void *dealer = zlink_socket (ctx_, ZLINK_SOCKET_DEALER);
    CHECK (router && dealer);
    set_int (router, ZLINK_OPT_LINGER, 0);
    set_int (dealer, ZLINK_OPT_LINGER, 0);
    set_int (router, ZLINK_OPT_RCVTIMEO, receive_timeout_ms);
    CHECK (zlink_bind (router, "inproc://recv-admission-router") == 0);
    CHECK (zlink_connect (dealer, "inproc://recv-admission-router") == 0);
    send_record (dealer, 2, static_cast<int> (retry_record_capacity));
    const zlink_routing_id_t *rid = NULL;
    zlink_reply_token_t token = 0;
    zlink_msg_t small[1], large[retry_record_capacity];
    size_t n_small = 0, n_large = 0;
    int first = -1, other = -1;
    first_then_other_live_thread (
      [&] {
          first =
            zlink_router_recv (router, &rid, &token, small, 1, &n_small, ZLINK_RECV_FLAGS_NONE);
      },
      [&] {
          other = zlink_router_recv (router, &rid, &token, large, retry_record_capacity, &n_large,
                                     ZLINK_RECV_FLAGS_NONE);
      });
    const int id = other == ZLINK_RECV_OK ? record_id (large, n_large) : -2;
    if (other == ZLINK_RECV_OK)
        zlink_multipart_close (large, n_large);
    zlink_msg_t tail[retry_record_capacity];
    size_t n_tail = 0;
    const int after = zlink_router_recv (router, &rid, &token, tail, retry_record_capacity, &n_tail,
                                         ZLINK_RECV_FLAGS_DONTWAIT);
    if (after == ZLINK_RECV_OK)
        zlink_multipart_close (tail, n_tail);
    std::ostringstream d;
    d << "first=" << first << " needed=" << n_small << " other=" << other << " id=" << id
      << " after=" << after;
    verdict ("router_retry_other_thread",
             first == ZLINK_RECV_BUFFER_TOO_SMALL && other == ZLINK_RECV_OK && id == 2
               && after == ZLINK_RECV_NO_DATA,
             d.str ());
    zlink_close (dealer);
    zlink_close (router);
}

static void case_xpub_and_sub_retry_other_thread (void *ctx_, zlink_socket_type_t type_)
{
    void *xpub = zlink_socket (ctx_, ZLINK_SOCKET_XPUB);
    void *sub = zlink_socket (ctx_, type_);
    CHECK (xpub && sub);
    set_int (xpub, ZLINK_OPT_LINGER, 0);
    set_int (sub, ZLINK_OPT_LINGER, 0);
    set_int (xpub, ZLINK_OPT_RCVTIMEO, receive_timeout_ms);
    set_int (sub, ZLINK_OPT_RCVTIMEO, receive_timeout_ms);
    const char *endpoint =
      type_ == ZLINK_SOCKET_SUB ? "inproc://recv-admission-sub" : "inproc://recv-admission-xsub";
    CHECK (zlink_bind (xpub, endpoint) == 0);
    CHECK (zlink_connect (sub, endpoint) == 0);
    const std::string topic = "topic-abcdef";
    CHECK (zlink_set_subscription (sub, topic.c_str ()) == 0);

    // XPUB event: small topic buffer on thread A, sufficient on thread B.
    const zlink_routing_id_t *rid = NULL;
    int subscribed = -1;
    char small[1];
    char large[64];
    size_t need = 0, len = 0;
    int first = -1, other = -1;
    first_then_other_live_thread (
      [&] {
          first = zlink_xpub_recv (xpub, &rid, &subscribed, small, sizeof (small), &need,
                                   ZLINK_RECV_FLAGS_NONE);
      },
      [&] {
          other = zlink_xpub_recv (xpub, &rid, &subscribed, large, sizeof (large), &len,
                                   ZLINK_RECV_FLAGS_NONE);
      });
    bool match = other == ZLINK_RECV_OK && len == topic.size ()
                 && std::memcmp (large, topic.data (), len) == 0;
    // A failed retry is recorded before the independent SUB contract is tested.
    if (other != ZLINK_RECV_OK) {
        const int own = zlink_xpub_recv (xpub, &rid, &subscribed, large, sizeof (large), &len,
                                         ZLINK_RECV_FLAGS_NONE);
        (void) own;
    }
    const int after = zlink_xpub_recv (xpub, &rid, &subscribed, large, sizeof (large), &len,
                                       ZLINK_RECV_FLAGS_DONTWAIT);
    std::ostringstream d;
    d << "first=" << first << " needed=" << need << " other=" << other << " topic_match=" << match
      << " after=" << after;
    verdict ("xpub_retry_other_thread",
             first == ZLINK_RECV_BUFFER_TOO_SMALL && other == ZLINK_RECV_OK && match
               && after == ZLINK_RECV_NO_DATA,
             d.str ());

    // SUB record: small parts buffer on thread A, sufficient on thread B.
    zlink_msg_t pub[retry_record_capacity];
    for (int i = 0; i < retry_record_capacity; ++i)
        make_part (&pub[i], "r3-" + std::to_string (i));
    CHECK (zlink_publish (xpub, topic.c_str (), pub, retry_record_capacity, ZLINK_SEND_FLAGS_NONE)
           == 0);
    char tbuf[64];
    size_t tlen = 0;
    zlink_msg_t sp_small[1], sp_large[retry_record_capacity];
    size_t n_small = 0, n_large = 0;
    int sfirst = -1, sother = -1;
    first_then_other_live_thread (
      [&] {
          sfirst = zlink_subscribe (sub, NULL, tbuf, sizeof (tbuf), &tlen, sp_small, 1, &n_small,
                                    ZLINK_RECV_FLAGS_NONE);
      },
      [&] {
          sother = zlink_subscribe (sub, NULL, tbuf, sizeof (tbuf), &tlen, sp_large,
                                    retry_record_capacity, &n_large, ZLINK_RECV_FLAGS_NONE);
      });
    const int id = sother == ZLINK_RECV_OK ? record_id (sp_large, n_large) : -2;
    if (sother == ZLINK_RECV_OK)
        zlink_multipart_close (sp_large, n_large);
    zlink_msg_t tail[retry_record_capacity];
    size_t n_tail = 0;
    const int safter = zlink_subscribe (sub, NULL, tbuf, sizeof (tbuf), &tlen, tail,
                                        retry_record_capacity, &n_tail, ZLINK_RECV_FLAGS_DONTWAIT);
    if (safter == ZLINK_RECV_OK)
        zlink_multipart_close (tail, n_tail);
    std::ostringstream ds;
    ds << "first=" << sfirst << " needed=" << n_small << " other=" << sother << " id=" << id
       << " after=" << safter;
    verdict (type_ == ZLINK_SOCKET_SUB ? "sub_retry_other_thread" : "xsub_retry_other_thread",
             sfirst == ZLINK_RECV_BUFFER_TOO_SMALL && sother == ZLINK_RECV_OK && id == 3
               && safter == ZLINK_RECV_NO_DATA,
             ds.str ());
    zlink_close (sub);
    zlink_close (xpub);
}


struct wait_observer_t
{
    std::promise<void> first_wait;
    std::promise<void> second_progress;
    std::atomic<bool> first_signaled{false};
    std::thread::id first_thread;
    std::atomic<bool> second_signaled{false};
    void signal_second ()
    {
        if (!second_signaled.exchange (true))
            second_progress.set_value ();
    }
    static void hook (void *userdata_)
    {
        wait_observer_t *self = static_cast<wait_observer_t *> (userdata_);
        if (std::this_thread::get_id () == self->first_thread) {
            if (!self->first_signaled.exchange (true))
                self->first_wait.set_value ();
        } else
            self->signal_second ();
    }
};

static void case_concurrent (void *ctx_, size_t capacity_, int parts_, int rounds_)
{
    int torn = 0, missing = 0, busy = 0, invalid = 0;
    for (int round = 0; round < rounds_; ++round) {
        void *rx, *tx;
        const std::string ep =
          "inproc://recv-admission-" + std::to_string (capacity_) + "-" + std::to_string (round);
        pair_sockets (ctx_, ep.c_str (), &rx, &tx);
        set_int (rx, ZLINK_OPT_RCVTIMEO, receive_timeout_ms);
        wait_observer_t observer;
        auto first_wait = observer.first_wait.get_future ();
        auto second_progress = observer.second_progress.get_future ();
        socket_handle_t handle = as_socket_handle (rx);
        CHECK (handle.socket->ensure_async_command_processing (true) == 0);
        handle.socket->test_set_receive_wait_hook (wait_observer_t::hook, &observer);
        int results[2] = {-1, -1}, errors[2] = {0, 0}, ids[2] = {-1, -1};
        size_t counts[2] = {0, 0};
        std::thread first ([&] {
            observer.first_thread = std::this_thread::get_id ();
            std::vector<zlink_msg_t> output (capacity_);
            results[0] = zlink_recv (rx, NULL, output.data (), output.size (), &counts[0],
                                     ZLINK_RECV_FLAGS_NONE);
            errors[0] = errno;
            if (results[0] == ZLINK_RECV_OK) {
                ids[0] = record_id (output.data (), counts[0]);
                zlink_multipart_close (output.data (), counts[0]);
            }
        });
        first_wait.wait ();
        std::thread second ([&] {
            std::vector<zlink_msg_t> output (capacity_);
            results[1] = zlink_recv (rx, NULL, output.data (), output.size (), &counts[1],
                                     ZLINK_RECV_FLAGS_NONE);
            errors[1] = errno;
            if (results[1] == ZLINK_RECV_OK) {
                ids[1] = record_id (output.data (), counts[1]);
                zlink_multipart_close (output.data (), counts[1]);
            }
            observer.signal_second ();
        });
        second_progress.wait ();
        send_record (tx, first_concurrent_record_id, parts_);
        send_record (tx, second_concurrent_record_id, parts_);
        first.join ();
        second.join ();
        handle.socket->test_set_receive_wait_hook (NULL, NULL);
        if (results[1] == ZLINK_RECV_BUSY && errors[1] == EBUSY)
            ++busy;
        int got = 0;
        std::vector<int> order;
        std::vector<int> all_ids;
        for (int w = 0; w < 2; ++w) {
            if (results[w] == ZLINK_RECV_OK) {
                ++got;
                all_ids.push_back (ids[w]);
                if (ids[w] < 0 || counts[w] != static_cast<size_t> (parts_))
                    ++torn;
            } else if (results[w] != ZLINK_RECV_BUSY && results[w] != ZLINK_RECV_BUFFER_TOO_SMALL)
                ++invalid;
        }
        zlink_msg_t drained[whole_record_capacity];
        size_t count = 0;
        int rc;
        while ((rc = zlink_recv (rx, NULL, drained, whole_record_capacity, &count,
                                 ZLINK_RECV_FLAGS_DONTWAIT))
               == ZLINK_RECV_OK) {
            const int id = record_id (drained, count);
            order.push_back (id);
            all_ids.push_back (id);
            if (id < 0 || count != static_cast<size_t> (parts_))
                ++torn;
            ++got;
            zlink_multipart_close (drained, count);
        }
        if (got != 2 || rc != ZLINK_RECV_NO_DATA || all_ids.size () != 2
            || !((all_ids[0] == first_concurrent_record_id
                  && all_ids[1] == second_concurrent_record_id)
                 || (all_ids[0] == second_concurrent_record_id
                     && all_ids[1] == first_concurrent_record_id)))
            ++missing;
        if (capacity_ < static_cast<size_t> (parts_)
            && (order.size () != 2 || order[0] != first_concurrent_record_id
                || order[1] != second_concurrent_record_id))
            ++invalid;
        handle = socket_handle_t ();
        zlink_close (tx);
        zlink_close (rx);
    }
    std::ostringstream detail;
    detail << "rounds=" << rounds_ << " busy=" << busy << " torn=" << torn << " missing=" << missing
           << " invalid=" << invalid;
    verdict (capacity_ == 1 ? "concurrent_small_buffer" : "concurrent_whole_record",
             busy == rounds_ && torn == 0 && missing == 0 && invalid == 0, detail.str ());
}


static void case_entry_busy (zlink_socket_type_t type_, const char *name_)
{
    void *ctx = zlink_ctx_new ();
    CHECK (ctx);
    void *rx = zlink_socket (ctx, type_);
    CHECK (rx);
    set_int (rx, ZLINK_OPT_LINGER, 0);
    set_int (rx, ZLINK_OPT_RCVTIMEO, receive_timeout_ms);
    void *tx = NULL;
    if (type_ == ZLINK_SOCKET_STREAM) {
        const zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
        CHECK (zlink_set_stream_option (rx, ZLINK_STREAM_OPT_RECV_MODE, &mode, sizeof (mode)) == 0);
    } else {
        const zlink_socket_type_t peer = type_ == ZLINK_SOCKET_ROUTER ? ZLINK_SOCKET_DEALER
                                         : type_ == ZLINK_SOCKET_SUB  ? ZLINK_SOCKET_XPUB
                                         : type_ == ZLINK_SOCKET_XPUB ? ZLINK_SOCKET_SUB
                                                                      : ZLINK_SOCKET_PAIR;
        tx = zlink_socket (ctx, peer);
        CHECK (tx);
        set_int (tx, ZLINK_OPT_LINGER, 0);
        const char rid[] = "recv-admission-dealer";
        if (type_ == ZLINK_SOCKET_ROUTER)
            CHECK (zlink_set_routing_id (tx, rid, sizeof (rid) - 1) == 0);
        const std::string ep = std::string ("inproc://recv-admission-busy-") + name_;
        CHECK (zlink_bind (type_ == ZLINK_SOCKET_SUB ? tx : rx, ep.c_str ()) == 0);
        CHECK (zlink_connect (type_ == ZLINK_SOCKET_SUB ? rx : tx, ep.c_str ()) == 0);
        if (type_ == ZLINK_SOCKET_SUB) {
            CHECK (zlink_set_subscription (rx, "busy-topic") == 0);
            char topic[64];
            size_t len = 0;
            int subscribed = 0;
            CHECK (zlink_xpub_recv (tx, NULL, &subscribed, topic, sizeof (topic), &len,
                                    ZLINK_RECV_FLAGS_NONE)
                   == ZLINK_RECV_OK);
        }
    }
    socket_handle_t handle = as_socket_handle (rx);
    CHECK (handle.socket->ensure_async_command_processing (true) == 0);
    wait_observer_t observer;
    auto wait = observer.first_wait.get_future ();
    handle.socket->test_set_receive_wait_hook (wait_observer_t::hook, &observer);
    auto receive = [=] (zlink_msg_t *parts_, const zlink_routing_id_t **rid_, uint64_t *token_,
                        size_t *count_, char *topic_, size_t *len_, int *subscribed_,
                        zlink_recv_flags_t flags_) {
        switch (type_) {
            case ZLINK_SOCKET_ROUTER:
                return zlink_router_recv (rx, rid_, token_, parts_, retry_record_capacity, count_,
                                          flags_);
            case ZLINK_SOCKET_SUB:
                return zlink_subscribe (rx, rid_, topic_, 64, len_, parts_, retry_record_capacity,
                                        count_, flags_);
            case ZLINK_SOCKET_XPUB:
                return zlink_xpub_recv (rx, rid_, subscribed_, topic_, 64, len_, flags_);
            case ZLINK_SOCKET_STREAM:
                return zlink_stream_recv_packet (rx, rid_, &parts_[0], &parts_[1], flags_);
            default:
                return zlink_recv (rx, rid_, parts_, retry_record_capacity, count_, flags_);
        }
    };
    int first_rc = -1;
    bool first_rid_ok = true;
    std::thread first ([&] {
        observer.first_thread = std::this_thread::get_id ();
        zlink_msg_t parts[retry_record_capacity];
        for (size_t i = 0; i < retry_record_capacity; ++i)
            CHECK (zlink_msg_init (&parts[i]) == 0);
        const zlink_routing_id_t *rid = NULL;
        uint64_t token = 0;
        size_t count = 0, len = 0;
        int subscribed = 0;
        char topic[64];
        first_rc =
          receive (parts, &rid, &token, &count, topic, &len, &subscribed, ZLINK_RECV_FLAGS_NONE);
        if (first_rc == ZLINK_RECV_OK && type_ == ZLINK_SOCKET_ROUTER) {
            const char expected[] = "recv-admission-dealer";
            first_rid_ok = rid && rid->size == sizeof (expected) - 1
                           && std::memcmp (rid->data, expected, sizeof (expected) - 1) == 0;
        }
        for (size_t i = 0; i < retry_record_capacity; ++i)
            CHECK (zlink_msg_close (&parts[i]) == 0);
    });
    wait.wait ();
    zlink_msg_t output[retry_record_capacity];
    for (size_t i = 0; i < retry_record_capacity; ++i)
        CHECK (zlink_msg_init (&output[i]) == 0);
    unsigned char saved[sizeof (output)];
    std::memcpy (saved, output, sizeof (output));
    zlink_routing_id_t sentinel = {};
    sentinel.size = 1;
    sentinel.data[0] = 's';
    const zlink_routing_id_t *rid = &sentinel;
    handle.socket->store_last_recv_source_rid (&sentinel);
    const zlink_routing_id_t *borrowed = handle.socket->last_recv_source_rid_view ();
    CHECK (borrowed);
    const zlink_routing_id_t borrowed_snapshot = *borrowed;
    const uint64_t sentinel_token = 917;
    const size_t sentinel_count = 919, sentinel_len = 921;
    const int sentinel_subscribed = 923;
    uint64_t token = sentinel_token;
    size_t count = sentinel_count, len = sentinel_len;
    int subscribed = sentinel_subscribed;
    char topic[64];
    std::memset (topic, 't', sizeof (topic));
    const int result =
      receive (output, &rid, &token, &count, topic, &len, &subscribed, ZLINK_RECV_FLAGS_DONTWAIT);
    const int error = errno;
    const bool unchanged =
      rid == &sentinel && token == sentinel_token && count == sentinel_count && len == sentinel_len
      && subscribed == sentinel_subscribed
      && handle.socket->last_recv_source_rid_view () == borrowed
      && std::memcmp (borrowed, &borrowed_snapshot, sizeof (*borrowed)) == 0
      && std::memcmp (saved, output, sizeof (output)) == 0
      && std::string (topic, sizeof (topic)) == std::string (sizeof (topic), 't');
    if (type_ == ZLINK_SOCKET_STREAM)
        CHECK (zlink_ctx_shutdown (ctx) == 0);
    else if (type_ == ZLINK_SOCKET_SUB) {
        zlink_msg_t part;
        make_part (&part, "record");
        CHECK (zlink_publish (tx, "busy-topic", &part, 1, ZLINK_SEND_FLAGS_NONE) == 0);
        CHECK (zlink_msg_close (&part) == 0);
    } else if (type_ == ZLINK_SOCKET_XPUB)
        CHECK (zlink_set_subscription (tx, "busy-topic") == 0);
    else
        send_record (tx, 31, static_cast<int> (retry_record_capacity));
    first.join ();
    handle.socket->test_set_receive_wait_hook (NULL, NULL);
    const int expected_first = type_ == ZLINK_SOCKET_STREAM ? ZLINK_RECV_TERMINATED : ZLINK_RECV_OK;
    std::ostringstream detail;
    detail << "result=" << result << " errno=" << error << " unchanged=" << unchanged
           << " first=" << first_rc << " rid=" << first_rid_ok;
    verdict (name_,
             result == ZLINK_RECV_BUSY && error == EBUSY && unchanged && first_rc == expected_first
               && first_rid_ok,
             detail.str ());
    for (size_t i = 0; i < retry_record_capacity; ++i)
        CHECK (zlink_msg_close (&output[i]) == 0);
    handle = socket_handle_t ();
    if (tx)
        CHECK (zlink_close (tx) == 0);
    CHECK (zlink_close (rx) == 0);
    CHECK (zlink_ctx_term (ctx) == 0);
}

static void case_handle_receive_pin ()
{
    void *ctx = zlink_ctx_new ();
    void *socket = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    CHECK (ctx && socket);
    socket_handle_t owner = as_socket_receive_handle (socket);
    CHECK (owner.socket);
    {
        socket_handle_t copy (owner);
        socket_handle_t assigned;
        assigned = owner;
        CHECK (copy.socket == owner.socket && assigned.socket == owner.socket);
    }
    CHECK (!as_socket_receive_handle (socket).socket && errno == EBUSY);
    socket_handle_t moved (std::move (owner));
    CHECK (!owner.socket && moved.socket);
    CHECK (!as_socket_receive_handle (socket).socket && errno == EBUSY);
    socket_handle_t destination;
    destination = std::move (moved);
    CHECK (!moved.socket && destination.socket);
    CHECK (!as_socket_receive_handle (socket).socket && errno == EBUSY);
    destination = socket_handle_t ();
    {
        socket_handle_t next = as_socket_receive_handle (socket);
        CHECK (next.socket);
    }
    socket_handle_t lifetime_owner = as_socket_receive_handle (socket);
    socket_handle_t lifetime_copy (lifetime_owner);
    CHECK (lifetime_owner.socket);
    CHECK (zlink_close (socket) == 0);
    CHECK (!as_socket_receive_handle (socket).socket && errno == ESHUTDOWN);
    CHECK (lifetime_copy.socket->check_tag ());
    lifetime_owner = socket_handle_t ();
    CHECK (lifetime_copy.socket->check_tag ());
    lifetime_copy = socket_handle_t ();
    CHECK (zlink_ctx_term (ctx) == 0);
    verdict ("receive_pin_copy_move_release", true,
             "copy=ordinary move=receive release=1 close=deferred-to-last-pin");
}

int main ()
{
    case_handle_receive_pin ();
    case_entry_busy (ZLINK_SOCKET_PAIR, "recv_entry_busy");
    case_entry_busy (ZLINK_SOCKET_ROUTER, "router_entry_busy_rid");
    case_entry_busy (ZLINK_SOCKET_SUB, "subscribe_entry_busy");
    case_entry_busy (ZLINK_SOCKET_XPUB, "xpub_entry_busy");
    case_entry_busy (ZLINK_SOCKET_STREAM, "stream_packet_entry_busy");
    void *ctx = zlink_ctx_new ();
    CHECK (ctx);
    case_pair_retry_other_thread (ctx, ZLINK_SOCKET_PAIR, "pair_retry_other_thread");
    case_pair_retry_other_thread (ctx, ZLINK_SOCKET_DEALER, "dealer_retry_other_thread");
    case_pair_retry_other_thread (ctx, ZLINK_SOCKET_STREAM, "stream_raw_retry_other_thread");
    case_router_retry_other_thread (ctx);
    case_xpub_and_sub_retry_other_thread (ctx, ZLINK_SOCKET_SUB);
    case_xpub_and_sub_retry_other_thread (ctx, ZLINK_SOCKET_XSUB);
    case_concurrent (ctx, 1, static_cast<int> (retry_record_capacity), 1);
    case_concurrent (ctx, whole_record_capacity, stress_record_part_count, stress_round_count);
    zlink_ctx_term (ctx);
    std::printf ("failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
