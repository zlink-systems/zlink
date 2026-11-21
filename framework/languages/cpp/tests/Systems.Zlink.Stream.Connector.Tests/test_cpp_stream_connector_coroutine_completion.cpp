/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>
#include "runtime/connector_runtime.hpp"
#include "runtime/protocol/framing.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/metadata_codec.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <zlink/json.hpp>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>
#include "../support/stream_connector_cases.hpp"

#include "../support/stream_connector_wire_frames.hpp"
#include "../support/stream_connector_typed_messages.hpp"
#include "../support/stream_connector_callback_sync.hpp"
#include "../support/stream_connector_failure.hpp"

using namespace stream_connector_fixture;

namespace
{
zlink::stream_e2e_client::task_t<void>
send_with_coroutine_submit (zlink::stream_e2e_client::coroutine_connector_t &connector)
{
    connector.send (login_request_t{}).packet_name ("coroutine.send").submit ();
    co_return;
}

zlink::stream_e2e_client::task_t<login_reply_t>
request_with_coroutine_submit (zlink::stream_e2e_client::coroutine_connector_t &connector)
{
    auto reply = co_await connector.request (login_request_t{})
                   .packet_name ("coroutine.request")
                   .timeout (std::chrono::milliseconds (100))
                   .async<login_reply_t> ();
    co_return reply;
}

zlink::stream_e2e_client::task_t<int> delayed_coroutine_child ()
{
    auto value = co_await zlink::stream_e2e_client::task_t<int> (
      [] (std::function<void (zlink::stream_connector::result_t<int>)> callback) {
          std::thread ([callback = std::move (callback)] () mutable {
              callback (zlink::stream_connector::result_t<int>::success (41));
          }).detach ();
      });
    co_return value;
}

zlink::stream_e2e_client::task_t<int> await_delayed_coroutine_child ()
{
    auto value = co_await delayed_coroutine_child ();
    co_return value + 1;
}

struct continuation_probe_t
{
    struct promise_type;
    using handle_t = std::coroutine_handle<promise_type>;

    struct promise_type
    {
        continuation_probe_t get_return_object () noexcept
        {
            return continuation_probe_t{handle_t::from_promise (*this)};
        }

        std::suspend_always initial_suspend () const noexcept { return {}; }
        std::suspend_always final_suspend () const noexcept { return {}; }
        void return_void () const noexcept {}
        void unhandled_exception () const noexcept { std::terminate (); }
    };

    explicit continuation_probe_t (handle_t handle) : handle (handle) {}
    continuation_probe_t (continuation_probe_t &&other) noexcept : handle (other.handle)
    {
        other.handle = {};
    }
    continuation_probe_t (const continuation_probe_t &) = delete;
    continuation_probe_t &operator= (const continuation_probe_t &) = delete;
    ~continuation_probe_t ()
    {
        if (handle) {
            handle.destroy ();
        }
    }

    handle_t handle;
};

continuation_probe_t make_continuation_probe (std::atomic_bool &resumed)
{
    resumed.store (true);
    co_return;
}

// The awaiter contract: a completed task reports `false` from await_suspend so
// the caller resumes at once instead of parking a continuation nobody will
// wake.
template <typename Task>
bool resume_if_await_suspend_reports_ready (Task &task, std::coroutine_handle<> continuation)
{
    if (task.await_suspend (continuation)) {
        return false;
    }
    continuation.resume ();
    return true;
}

bool task_completion_before_suspend_resumes_value_task ()
{
    using task_t = zlink::stream_e2e_client::task_t<int>;
    task_t::callback_t complete;
    task_t task ([&complete] (task_t::callback_t callback) { complete = std::move (callback); });
    if (task.await_ready () || !complete) {
        return false;
    }

    complete (zlink::stream_connector::result_t<int>::success (7));
    std::atomic_bool resumed{false};
    auto probe = make_continuation_probe (resumed);
    return resume_if_await_suspend_reports_ready (task, probe.handle) && resumed.load ();
}

bool task_completion_before_suspend_resumes_void_task ()
{
    using task_t = zlink::stream_e2e_client::task_t<void>;
    task_t::callback_t complete;
    task_t task ([&complete] (task_t::callback_t callback) { complete = std::move (callback); });
    if (task.await_ready () || !complete) {
        return false;
    }

    complete (zlink::stream_connector::result_t<void>::success ());
    std::atomic_bool resumed{false};
    auto probe = make_continuation_probe (resumed);
    return resume_if_await_suspend_reports_ready (task, probe.handle) && resumed.load ();
}

zlink::stream_e2e_client::task_t<bool>
result_waits_for_coroutine_frame_cleanup (std::atomic_bool &cleaned)
{
    co_await zlink::stream_e2e_client::task_t<void> (
      [] (std::function<void (zlink::stream_connector::result_t<void>)> callback) {
          std::thread ([callback = std::move (callback)] () mutable {
              callback (zlink::stream_connector::result_t<void>::success ());
          }).detach ();
      });
    struct cleanup_marker_t
    {
        std::atomic_bool *cleaned;
        ~cleanup_marker_t ()
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
            cleaned->store (true);
        }
    } cleanup_marker{&cleaned};
    co_return true;
}

/* Issue #630: a coroutine frame is owned by the frame itself, never by the
 * `task_t` that carries its result.
 *
 * On Windows under load, `connector_t::close` completes a pending operation on
 * the connector's own IOCP thread and that resumes the client coroutine there.
 * The coroutine therefore runs its last steps - result production, frame-local
 * cleanup, final suspend - on a thread the task's owner does not control. If
 * `~task_t` may destroy the frame, the owner can free it while the connector
 * thread is still inside it.
 *
 * The probe below forces exactly that interleaving with semaphores instead of
 * timing: the coroutine parks on `owner_done` after producing its result and
 * before the frame is finished, the owner drops the task and then reclaims the
 * heap, and the coroutine checks whether its own frame survived. */
struct frame_lifetime_probe_t
{
    std::binary_semaphore resume_frame{0}; /* owner  -> frame: finish the awaited operation */
    std::binary_semaphore frame_parked{0}; /* frame  -> owner: result produced, frame still live */
    std::binary_semaphore owner_done{0};   /* owner  -> frame: the owner has dropped the task */
    std::binary_semaphore frame_left{0};   /* frame  -> owner: the frame-local guard is done */
    std::atomic_bool frame_overwritten{false};
    std::atomic_int guard_destructions{0};
};

zlink::stream_e2e_client::task_t<bool>
coroutine_frame_outlives_its_task (frame_lifetime_probe_t &probe)
{
    /* Resumed from a foreign thread, the way the connector's close path resumes
   * a client coroutine. */
    co_await zlink::stream_e2e_client::task_t<void> (
      [&probe] (std::function<void (zlink::stream_connector::result_t<void>)> callback) {
          std::thread ([&probe, callback = std::move (callback)] () mutable {
              probe.resume_frame.acquire ();
              callback (zlink::stream_connector::result_t<void>::success ());
          }).detach ();
      });

    struct frame_guard_t
    {
        frame_lifetime_probe_t *probe;
        unsigned char canary[4096];

        explicit frame_guard_t (frame_lifetime_probe_t *owner) : probe (owner)
        {
            std::memset (canary, 0xa5, sizeof (canary));
        }
        frame_guard_t (const frame_guard_t &) = delete;
        frame_guard_t &operator= (const frame_guard_t &) = delete;
        ~frame_guard_t ()
        {
            /* Frame-local destructors run after the coroutine has produced its
       * result and before final suspend, so this is the point where a
       * result-carrying owner could believe the frame is finished. */
            auto *watcher = probe; /* off the frame, so parking never reads it */
            watcher->guard_destructions.fetch_add (1);
            watcher->frame_parked.release ();
            watcher->owner_done.acquire ();
            for (const unsigned char byte : canary) {
                if (byte != 0xa5) {
                    watcher->frame_overwritten.store (true);
                    break;
                }
            }
            watcher->frame_left.release ();
        }
    } guard{&probe};

    co_return true;
}
} // namespace

namespace stream_connector_tests
{
int verify_completion_before_suspend ()
{
    if (!task_completion_before_suspend_resumes_value_task ()) {
        return fail (261);
    }
    if (!task_completion_before_suspend_resumes_void_task ()) {
        return fail (262);
    }

    return 0;
}

int verify_coroutine_frame_lifetime ()
{
    {
        auto nested = await_delayed_coroutine_child ().result ();
        if (!nested || nested.value () != 42) {
            return fail (76);
        }
    }
    {
        std::atomic_bool coroutine_frame_cleaned{false};
        auto cleanup_result =
          result_waits_for_coroutine_frame_cleanup (coroutine_frame_cleaned).result ();
        if (!cleanup_result || !cleanup_result.value () || !coroutine_frame_cleaned.load ()) {
            return fail (108);
        }
    }
    /* Issue #630: dropping the task must not free a coroutine frame that is
   * still running on another thread. */
    {
        frame_lifetime_probe_t probe;
        std::optional<zlink::stream_e2e_client::task_t<bool>> task (
          coroutine_frame_outlives_its_task (probe));

        probe.resume_frame.release ();
        probe.frame_parked.acquire ();

        /* The coroutine has produced its result and is parked inside its own
     * frame. This is where the owner used to conclude the task was done. */
        task.reset ();

        /* Reclaim the heap: if the frame was freed, these blocks take its bytes. */
        std::vector<std::vector<unsigned char>> reclaimed;
        reclaimed.reserve (512);
        for (int index = 0; index < 512; ++index) {
            reclaimed.emplace_back (8192, 0x5a);
        }

        probe.owner_done.release ();
        probe.frame_left.acquire ();

        if (probe.frame_overwritten.load ()) {
            return fail (204);
        }
        if (probe.guard_destructions.load () != 1) {
            return fail (205);
        }
    }

    return 0;
}

int verify_coroutine_operations (
  zlink::context_t &context,
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request,
  zlink::stream_connector::connector_t &immediate)
{
    zlink::stream_socket_t coroutine_server (context);
    coroutine_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    coroutine_server.options ().notify (false);
    coroutine_server.bind ("tcp://127.0.0.1:0");
    const auto coroutine_endpoint = coroutine_server.options ().last_endpoint ();
    std::atomic_bool coroutine_send_seen{false};
    std::atomic_bool coroutine_request_seen{false};
    joining_thread_t coroutine_thread ([&coroutine_server, &coroutine_send_seen,
                                        &coroutine_request_seen] {
        std::string buffer;
        while (!coroutine_send_seen || !coroutine_request_seen) {
            zlink::received_t inbound;
            if (coroutine_server.recv (inbound) != 0) {
                return;
            }
            buffer += inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
            while (auto frame = try_read_server_frame (buffer)) {
                if (frame->header.kind == zlink::stream_connector::message_kind_t::send
                    && frame->header.name == "coroutine.send") {
                    coroutine_send_seen = true;
                }
                if (frame->header.kind == zlink::stream_connector::message_kind_t::request
                    && frame->header.name == "coroutine.request") {
                    coroutine_request_seen = true;
                    auto reply = make_server_frame (
                      zlink::stream_connector::message_kind_t::response,
                      frame->header.request_seq.value (), frame->header.name, "ok");
                    inbound.send ().message (reply).submit ();
                }
            }
            inbound.close ();
        }
    });
    zlink::stream_connector::connector_options_t coroutine_options;
    coroutine_options.endpoint = coroutine_endpoint;
    coroutine_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto coroutine_connector =
      zlink::stream_connector::connector_factory_t::create (coroutine_options);
    if (!coroutine_connector.connect ()) {
        return fail (73);
    }
    auto coroutine_adapter = zlink::stream_e2e_client::use (coroutine_connector);
    auto coroutine_send = send_with_coroutine_submit (coroutine_adapter).result ();
    if (!coroutine_send) {
        return fail (74);
    }
    auto coroutine_request = request_with_coroutine_submit (coroutine_adapter).result ();
    coroutine_thread.join ();
    if (!coroutine_request || !coroutine_send_seen || !coroutine_request_seen) {
        return fail (75);
    }
    coroutine_connector.close ();

    if (const int failure = verify_read_terminal_order (); failure != 0)
        return failure;
    if (const int failure = verify_receive_count_and_close_retention (); failure != 0)
        return failure;
    if (const int failure = verify_close_during_error_callback (immediate); failure != 0)
        return failure;
    if (const int failure = verify_completion_from_callback (); failure != 0)
        return failure;
    if (const int failure = verify_write_failure_and_close (); failure != 0)
        return failure;
    if (const int failure = verify_immediate_receive_queue (); failure != 0)
        return failure;
    if (const int failure = verify_protocol_close (); failure != 0)
        return failure;
    if (const int failure = verify_late_receive_handler (); failure != 0)
        return failure;
    if (const int failure = verify_inbound_decompression (options, connector); failure != 0)
        return failure;
    if (const int failure = verify_actor_wait_and_dispatch_order (options, connector); failure != 0)
        return failure;
    if (const int failure = verify_repeated_close_reason (connector); failure != 0)
        return failure;
    if (const int failure = verify_late_manual_handler (options, connector); failure != 0)
        return failure;
    if (const int failure = verify_decompression_failure (); failure != 0)
        return failure;
    return verify_close_from_callback (context, connector, runtime, request, immediate);
}
} // namespace stream_connector_tests
