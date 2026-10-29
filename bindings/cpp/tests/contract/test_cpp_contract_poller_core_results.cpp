/* SPDX-License-Identifier: MPL-2.0 */
#include "support.hpp"

#include <zlink.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <stdexcept>
#include <algorithm>
#include <unistd.h>

extern "C" void *__real_zlink_ctx_new ();

extern "C" zlink_recv_result_t __real_zlink_completion_recv (
  void *, zlink_completion_t *, zlink_recv_flags_t);

extern "C" zlink_config_result_t __real_zlink_socket_connection_attempts_snapshot (
  void *, zlink_connection_attempt_t *, size_t, size_t *, char *, size_t, size_t *);

extern "C" int
__real_zlink_poller_wait (void *, zlink_poller_event_t *, int, long, zlink_config_result_t *);

namespace
{
#define CHECK(condition_)                                                                          \
    do {                                                                                           \
        if (!(condition_)) {                                                                       \
            std::fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition_);                 \
            std::exit (1);                                                                         \
        }                                                                                          \
    } while (false)

std::atomic_bool inject_wait_error{false};

template <typename Action> bool has_config_result (Action &&action_, zlink::config_result_t result_)
{
    try {
        action_ ();
    }
    catch (const zlink::config_error_t &error_) {
        return error_.result () == result_;
    }
    return false;
}

void test_duplicate_source_uses_core_conflict ()
{
    zlink::poller_t poller;
    int fds[2];
    CHECK (pipe (fds) == 0);
    poller.add_fd (fds[0], zlink::poll_event_flag_t::pollin, 0);
    CHECK (has_config_result ([&] { poller.add_fd (fds[0], zlink::poll_event_flag_t::pollin, 0); },
                              zlink::config_result_t::conflict));
    CHECK (poller.size () == 1);
    CHECK (poller.remove_fd (fds[0]));
    CHECK (
      has_config_result ([&] { poller.remove_fd (fds[0]); }, zlink::config_result_t::not_found));
    CHECK (has_config_result ([&] { poller.modify_fd (fds[0], zlink::poll_event_flag_t::pollin); },
                              zlink::config_result_t::not_found));
    close (fds[0]);
    close (fds[1]);

    zlink::context_t context;
    zlink::pair_socket_t socket (context);
    poller.add (socket, zlink::poll_event_flag_t::pollin, 1);
    CHECK (has_config_result ([&] { poller.add (socket, zlink::poll_event_flag_t::pollin, 2); },
                              zlink::config_result_t::conflict));
    CHECK (poller.size () == 1);
    CHECK (poller.remove (socket));

    zlink::timer_t timer;
    poller.add (timer, 3);
    CHECK (has_config_result ([&] { poller.add (timer, 4); }, zlink::config_result_t::conflict));
    CHECK (poller.size () == 1);
    CHECK (poller.remove (timer));
}

void test_active_wait_uses_core_busy ()
{
    zlink::poller_t poller;
    int fds[2];
    int other[2];
    CHECK (pipe (fds) == 0 && pipe (other) == 0);
    poller.add_fd (fds[0], zlink::poll_event_flag_t::pollin, 0);
    std::thread waiter ([&] {
        zlink::poll_event_t event;
        CHECK (poller.wait (&event, 1, std::chrono::milliseconds (-1)) == 1);
    });

    // The public size result is the signal that Core has entered the wait.
    bool core_busy = false;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!core_busy && std::chrono::steady_clock::now () < deadline) {
        core_busy =
          has_config_result ([&] { (void) poller.size (); }, zlink::config_result_t::busy);
        if (!core_busy)
            std::this_thread::yield ();
    }
    bool add_busy = false;
    bool modify_busy = false;
    bool remove_busy = false;
    bool wait_busy = false;
    if (core_busy) {
        add_busy =
          has_config_result ([&] { poller.add_fd (other[0], zlink::poll_event_flag_t::pollin, 0); },
                             zlink::config_result_t::busy);
        modify_busy =
          has_config_result ([&] { poller.modify_fd (fds[0], zlink::poll_event_flag_t::pollin); },
                             zlink::config_result_t::busy);
        remove_busy =
          has_config_result ([&] { poller.remove_fd (fds[0]); }, zlink::config_result_t::busy);
        wait_busy = has_config_result (
          [&] {
              zlink::poll_event_t event;
              (void) poller.wait (&event, 1, std::chrono::milliseconds (0));
          },
          zlink::config_result_t::busy);
    }
    const char wake = 1;
    CHECK (write (fds[1], &wake, 1) == 1);
    waiter.join ();
    CHECK (core_busy && add_busy && modify_busy && remove_busy && wait_busy);
    CHECK (poller.size () == 1);
    CHECK (poller.remove_fd (fds[0]));
    close (fds[0]);
    close (fds[1]);
    close (other[0]);
    close (other[1]);
}

void test_wait_uses_error_out ()
{
    zlink::poller_t poller;
    int fds[2];
    CHECK (pipe (fds) == 0);
    poller.add_fd (fds[0], zlink::poll_event_flag_t::pollin, 0);
    inject_wait_error = true;
    bool preserved = false;
    try {
        zlink::poll_event_t event;
        (void) poller.wait (&event, 1, std::chrono::milliseconds (0));
    }
    catch (const zlink::config_error_t &error_) {
        preserved = error_.result () == zlink::config_result_t::not_found
                    && error_.internal_errno () == EBUSY;
    }
    CHECK (preserved);
    CHECK (poller.remove_fd (fds[0]));
    close (fds[0]);
    close (fds[1]);
}
// Controlled native results exercise the public conversion boundary.
struct snapshot_reply_t
{
    zlink_config_result_t result = ZLINK_CONFIG_OK;
    size_t count = 1;
    size_t bytes = 4;
    zlink_connection_attempt_t row{};
    std::string endpoint = "abc";
    bool terminate = true;
    std::vector<zlink_connection_attempt_t> rows;
};
std::vector<snapshot_reply_t> snapshot_replies;
size_t snapshot_call = 0;
bool inject_snapshot = false;
bool inject_readiness_bits = false;
zlink::socket_t *observed_socket = nullptr;
size_t real_deficits = 0;
size_t empty_completion_drains = 0;
bool inject_later_connect = false;
std::string later_endpoint;
size_t context_creations = 0;

snapshot_reply_t valid_snapshot_reply ()
{
    snapshot_reply_t reply;
    reply.row.endpoint_length = 3;
    reply.row.intent_id = UINT64_MAX;
    reply.row.revision = uint64_t{1} << 63;
    reply.row.last_failure_id = UINT64_MAX;
    reply.row.state = ZLINK_CONNECTION_CONNECTED;
    reply.row.last_failure.kind = ZLINK_CONNECTION_FAILURE_CONNECTION_LOST;
    reply.row.last_failure.stage = ZLINK_CONNECTION_STAGE_ESTABLISHED;
    reply.row.last_failure.normalized_errno = INT32_MIN;
    reply.row.last_failure.disconnect_reason = UINT32_MAX;
    return reply;
}

void replies_for (snapshot_reply_t reply_)
{
    auto size = reply_;
    size.result = ZLINK_CONFIG_BUFFER_TOO_SMALL;
    snapshot_replies = {size, reply_};
    snapshot_call = 0;
    inject_snapshot = true;
}

void expect_protocol_violation (zlink::socket_t &socket_, snapshot_reply_t reply_)
{
    replies_for (reply_);
    bool rejected = false;
    try { (void) socket_.connection_attempts_snapshot (); }
    catch (const zlink::config_error_t &) { CHECK (false); }
    catch (const std::runtime_error &error_) {
        rejected = std::strstr (error_.what (), "connection snapshot protocol violation") != nullptr;
    }
    CHECK (rejected);
    CHECK (snapshot_call == 2);
    inject_snapshot = false;
}

void test_context_requires_snapshot_capability ()
{
    const auto before = context_creations;
    size_t rejected = 0;
    for (auto result : {ZLINK_CONFIG_OK, ZLINK_CONFIG_NOT_SUPPORTED, ZLINK_CONFIG_BUSY}) {
        for (bool explicit_threads : {false, true}) {
            snapshot_reply_t reply;
            reply.result = result;
            reply.count = 0;
            reply.bytes = 0;
            snapshot_replies = {reply};
            snapshot_call = 0;
            inject_snapshot = true;
            bool violation = false;
            try {
                if (explicit_threads) {
                    zlink::context_t context (zlink::io_thread_count_t::value (1));
                } else {
                    zlink::context_t context;
                }
            }
            catch (const zlink::config_error_t &) { CHECK (false); }
            catch (const std::runtime_error &error_) {
                violation = std::strstr (error_.what (), "connection snapshot protocol violation") != nullptr;
            }
            CHECK (violation && snapshot_call == 1);
            CHECK (context_creations == before);
            inject_snapshot = false;
            ++rejected;
        }
    }
    std::printf ("context capability: unexpected_native_results_rejected=%zu before_ctx_new=1\n", rejected);
}

void test_snapshot_projection ()
{
    zlink::context_t context;
    zlink::dealer_socket_t socket (context);
    auto reply = valid_snapshot_reply ();
    size_t accepted = 0;
    for (uint32_t state = 0; state < 3; ++state)
        for (uint32_t kind = 0; kind < 10; ++kind)
            for (uint32_t stage = 0; stage < 7; ++stage) {
                reply.row.state = state;
                reply.row.last_failure.kind = kind;
                reply.row.last_failure.stage = stage;
                replies_for (reply);
                const auto rows = socket.connection_attempts_snapshot ();
                CHECK (rows.size () == 1);
                CHECK (static_cast<uint32_t> (rows[0].state) == state);
                CHECK (static_cast<uint32_t> (rows[0].last_failure.kind) == kind);
                CHECK (static_cast<uint32_t> (rows[0].last_failure.stage) == stage);
                CHECK (rows[0].intent_id == UINT64_MAX);
                CHECK (rows[0].revision == (uint64_t{1} << 63));
                CHECK (rows[0].last_failure_id == UINT64_MAX);
                CHECK (rows[0].last_failure.normalized_errno == INT32_MIN);
                CHECK (rows[0].last_failure.disconnect_reason == UINT32_MAX);
                CHECK (snapshot_call == 2);
                ++accepted;
            }
    reply = valid_snapshot_reply ();
    reply.endpoint = "ipc://\xed\x95\x9c\xea\xb8\x80";
    reply.bytes = reply.endpoint.size () + 1;
    reply.row.endpoint_length = reply.endpoint.size ();
    replies_for (reply);
    const auto owned = socket.connection_attempts_snapshot ();
    CHECK (owned[0].endpoint == reply.endpoint);
    reply.row.intent_id = uint64_t{1} << 63;
    reply.row.revision = UINT64_MAX;
    reply.row.last_failure_id = 0;
    reply.row.last_failure.normalized_errno = INT32_MAX;
    reply.row.last_failure.disconnect_reason = uint32_t{1} << 31;
    replies_for (reply);
    auto absent = socket.connection_attempts_snapshot ();
    CHECK (absent[0].intent_id == (uint64_t{1} << 63));
    CHECK (absent[0].revision == UINT64_MAX);
    CHECK (absent[0].last_failure_id == 0);
    CHECK (absent[0].last_failure.normalized_errno == INT32_MAX);
    CHECK (absent[0].last_failure.disconnect_reason == (uint32_t{1} << 31));
    CHECK (owned[0].endpoint == reply.endpoint);
    size_t rejected = 0;
    for (int field = 0; field < 3; ++field)
        for (uint32_t value : {uint32_t{10}, uint32_t{1} << 31, UINT32_MAX}) {
            auto bad = valid_snapshot_reply ();
            if (field == 0) bad.row.state = value;
            if (field == 1) bad.row.last_failure.kind = value;
            if (field == 2) bad.row.last_failure.stage = value;
            expect_protocol_violation (socket, bad);
            ++rejected;
        }
    auto bad_state = valid_snapshot_reply (); bad_state.row.state = 3;
    expect_protocol_violation (socket, bad_state); ++rejected;
    auto bad_stage = valid_snapshot_reply (); bad_stage.row.last_failure.stage = 7;
    expect_protocol_violation (socket, bad_stage); ++rejected;
    for (int fault = 0; fault < 8; ++fault) {
        auto bad = valid_snapshot_reply ();
        if (fault == 0) bad.row.intent_id = 0;
        if (fault == 1) bad.row.revision = 0;
        if (fault == 2) bad.row.endpoint_offset = SIZE_MAX;
        if (fault == 3) bad.row.endpoint_length = SIZE_MAX;
        if (fault == 4) bad.row.endpoint_offset = bad.bytes;
        if (fault == 5) bad.row.endpoint_length = bad.bytes;
        if (fault == 6) bad.terminate = false;
        if (fault == 7) { bad.row.endpoint_offset = 2; bad.row.endpoint_length = 2; }
        expect_protocol_violation (socket, bad);
        ++rejected;
    }
    // Success counts must fit the buffers allocated by the preceding deficit.
    for (bool rows : {false, true}) {
        auto size = valid_snapshot_reply ();
        size.result = ZLINK_CONFIG_BUFFER_TOO_SMALL;
        auto bad = valid_snapshot_reply ();
        if (rows) ++bad.count; else ++bad.bytes;
        snapshot_replies = {size, bad}; snapshot_call = 0; inject_snapshot = true;
        bool rejected_count = false;
        try { (void) socket.connection_attempts_snapshot (); }
        catch (const std::runtime_error &) { rejected_count = true; }
        CHECK (rejected_count); ++rejected;
    }
    snapshot_replies = {valid_snapshot_reply ()};
    snapshot_replies[0].result = ZLINK_CONFIG_BUSY;
    snapshot_call = 0; inject_snapshot = true;
    bool query_error = false;
    try { (void) socket.connection_attempts_snapshot (); }
    catch (const zlink::config_error_t &error_) {
        query_error = error_.result () == zlink::config_result_t::busy
                      && error_.internal_errno () == EBUSY;
    }
    CHECK (query_error && snapshot_call == 1);
    inject_snapshot = false;
    socket.close ();
    CHECK (owned[0].endpoint == reply.endpoint);
    std::printf ("snapshot projection: legal=%zu malformed_rejected=%zu query_errors=1 owned_utf8=1\n", accepted, rejected);
}

void test_snapshot_capacity_growth ()
{
    zlink::context_t context;
    zlink::dealer_socket_t socket (context);
    auto deficit = valid_snapshot_reply ();
    deficit.result = ZLINK_CONFIG_BUFFER_TOO_SMALL;
    deficit.count = 1; deficit.bytes = 0;
    auto arena = deficit; arena.bytes = 4;
    auto rows = arena; rows.count = 2;
    auto both = rows; both.count = 3; both.bytes = 12;
    auto final = valid_snapshot_reply (); final.count = 3; final.bytes = 12;
    final.endpoint = std::string ("abc\0def\0ghi", 11);
    for (size_t i = 0; i < 3; ++i) {
        auto row = final.row; row.endpoint_offset = i * 4; row.intent_id = i + 1;
        final.rows.push_back (row);
    }
    snapshot_replies = {deficit, arena, rows, both, final};
    snapshot_call = 0; inject_snapshot = true;
    const auto result = socket.connection_attempts_snapshot ();
    CHECK (result.size () == 3);
    CHECK (result[0].endpoint == "abc" && result[0].intent_id == 1);
    CHECK (result[1].endpoint == "def" && result[1].intent_id == 2);
    CHECK (result[2].endpoint == "ghi" && result[2].intent_id == 3);
    CHECK (snapshot_call == 5);
    inject_snapshot = false;
    std::puts ("snapshot growth: row_only=1 arena_only=1 alternating=1 both_deficient=1 full_success=1");
}

void test_snapshot_stream_upcast ()
{
    zlink::context_t context;
    zlink::stream_socket_t stream (context);
    CHECK (has_config_result ([&] {
        (void) static_cast<zlink::socket_t &> (stream).connection_attempts_snapshot ();
    }, zlink::config_result_t::not_supported));
    std::puts ("snapshot STREAM upcast: native_not_supported=1");
}

void test_pollconnect_existing_event_paths ()
{
    zlink::context_t context;
    zlink::dealer_socket_t socket (context);
    zlink::poller_t poller;
    poller.add (socket, zlink::poll_event_flag_t::pollconnect, 1710);
    socket.connect (zlink_cpp_contract::unique_inproc ("snapshot-readiness"));
    zlink::poll_event_t event;
    CHECK (poller.wait (&event, 1, std::chrono::milliseconds (0)) == 1);
    CHECK (static_cast<short> (event.revents) == ZLINK_POLLCONNECT);
    CHECK (empty_completion_drains == 0);
    observed_socket = &socket;
    CHECK (socket.connection_attempts_snapshot ().size () == 1);
    observed_socket = nullptr;
    CHECK (real_deficits == 1);
    CHECK (poller.wait (&event, 1, std::chrono::milliseconds (0)) == 0);
    observed_socket = &socket;
    later_endpoint = zlink_cpp_contract::unique_inproc ("snapshot-later-change");
    inject_later_connect = true;
    CHECK (socket.connection_attempts_snapshot ().size () == 1);
    observed_socket = nullptr;
    CHECK (!inject_later_connect);
    CHECK (poller.wait (&event, 1, std::chrono::milliseconds (0)) == 1);
    auto item = zlink::poll_item_t::from_socket (socket, zlink::poll_event_flag_t::pollconnect);
    CHECK (zlink::poll (&item, 1, std::chrono::milliseconds (0)) == 1);
    CHECK (static_cast<short> (item.revents) == ZLINK_POLLCONNECT);
    poller.modify (socket, zlink::poll_event_flag_t::pollconnect
                           | zlink::poll_event_flag_t::pollcompletion);
    inject_readiness_bits = true;
    CHECK (poller.wait (&event, 1, std::chrono::milliseconds (0)) == 1);
    inject_readiness_bits = false;
    CHECK (static_cast<short> (event.revents)
           == (ZLINK_POLLCONNECT | ZLINK_POLLIN | ZLINK_POLLROUTE));
    CHECK (empty_completion_drains == 1);
    CHECK (socket.connection_attempts_snapshot ().size () == 2);
    CHECK (poller.wait (&event, 1, std::chrono::milliseconds (0)) == 0);
    poller.close ();
    std::puts ("pollconnect: poller=1 one_shot=1 deficit_keeps_ready=1 racing_change=1 independent_bits=3 empty_completion_drains=1");
}

} // namespace

extern "C" int __wrap_zlink_poller_wait (void *poller_,
                                         zlink_poller_event_t *events_,
                                         int capacity_,
                                         long timeout_,
                                         zlink_config_result_t *error_)
{
    if (inject_wait_error.exchange (false)) {
        if (error_)
            *error_ = ZLINK_CONFIG_NOT_FOUND;
        errno = EBUSY;
        return -1;
    }
    const int rc = __real_zlink_poller_wait (poller_, events_, capacity_, timeout_, error_);
    if (inject_readiness_bits && rc > 0)
        events_[0].events |= ZLINK_POLLIN | ZLINK_POLLROUTE | ZLINK_POLLCOMPLETION;
    return rc;
}

extern "C" void *__wrap_zlink_ctx_new ()
{
    ++context_creations;
    return __real_zlink_ctx_new ();
}

extern "C" zlink_recv_result_t __wrap_zlink_completion_recv (
  void *socket_, zlink_completion_t *completion_, zlink_recv_flags_t flags_)
{
    const auto rc = __real_zlink_completion_recv (socket_, completion_, flags_);
    if (inject_readiness_bits) {
        CHECK (rc == ZLINK_RECV_NO_DATA);
        ++empty_completion_drains;
    }
    return rc;
}

extern "C" zlink_config_result_t __wrap_zlink_socket_connection_attempts_snapshot (
  void *socket_, zlink_connection_attempt_t *rows_, size_t capacity_, size_t *count_,
  char *arena_, size_t arena_capacity_, size_t *bytes_)
{
    if (!inject_snapshot) {
        short readiness_before = 0;
        if (observed_socket) {
            auto item = zlink::poll_item_t::from_socket (
              *observed_socket, zlink::poll_event_flag_t::pollconnect);
            CHECK (zlink::poll (&item, 1, std::chrono::milliseconds (0)) >= 0);
            readiness_before = static_cast<short> (item.revents);
        }
        const auto rc = __real_zlink_socket_connection_attempts_snapshot (
          socket_, rows_, capacity_, count_, arena_, arena_capacity_, bytes_);
        if (observed_socket && rc == ZLINK_CONFIG_BUFFER_TOO_SMALL) {
            auto item = zlink::poll_item_t::from_socket (
              *observed_socket, zlink::poll_event_flag_t::pollconnect);
            CHECK (zlink::poll (&item, 1, std::chrono::milliseconds (0)) >= 0);
            CHECK (static_cast<short> (item.revents) == readiness_before);
            ++real_deficits;
        }
        if (observed_socket && inject_later_connect && rc == ZLINK_CONFIG_OK) {
            // A new Core change occurs after the consistent native read and
            // before the binding has returned its owned projection.
            inject_later_connect = false;
            observed_socket->connect (later_endpoint);
        }
        return rc;
    }
    size_t expected_rows = 0, expected_bytes = 0;
    for (size_t i = 0; i < snapshot_call; ++i) {
        expected_rows = std::max (expected_rows, snapshot_replies[i].count);
        expected_bytes = std::max (expected_bytes, snapshot_replies[i].bytes);
    }
    CHECK (capacity_ == expected_rows && arena_capacity_ == expected_bytes);
    CHECK (snapshot_call < snapshot_replies.size ());
    const auto &reply = snapshot_replies[snapshot_call++];
    *count_ = reply.count;
    *bytes_ = reply.bytes;
    if (reply.result == ZLINK_CONFIG_OK) {
        // Deliberately malformed counts never cause the injector to overflow.
        for (size_t i = 0; i < std::min (capacity_, reply.count); ++i)
            rows_[i] = reply.rows.empty () ? reply.row : reply.rows.at (i);
        if (arena_capacity_ != 0) {
            std::memset (arena_, 'x', arena_capacity_);
            const auto length = std::min (arena_capacity_, reply.endpoint.size ());
            std::memcpy (arena_, reply.endpoint.data (), length);
            if (reply.terminate && length < arena_capacity_)
                arena_[length] = '\0';
        }
    }
    errno = EBUSY;
    return reply.result;
}

int main (int argc_, char **argv_)
{
    if (argc_ == 1 || std::strcmp (argv_[1], "snapshot") == 0) {
        test_context_requires_snapshot_capability ();
        test_snapshot_projection ();
        test_snapshot_capacity_growth ();
        test_snapshot_stream_upcast ();
        test_pollconnect_existing_event_paths ();
    }
    if (argc_ == 1 || std::strcmp (argv_[1], "duplicate") == 0)
        test_duplicate_source_uses_core_conflict ();
    if (argc_ == 1 || std::strcmp (argv_[1], "concurrency") == 0)
        test_active_wait_uses_core_busy ();
    if (argc_ == 1 || std::strcmp (argv_[1], "projection") == 0)
        test_wait_uses_error_out ();
}
