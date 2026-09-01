/* SPDX-License-Identifier: MPL-2.0 */

#include "../../perf/multi/common/perf_common.hpp"
#include "../../perf/multi/common/perf_multi_reqrep.hpp"
#include "support.hpp"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
void require (bool condition_, const std::string &message_)
{
    if (!condition_)
        throw std::runtime_error (message_);
}

struct scheduler_probe_t
{
    std::mutex mutex;
    std::thread::id handoff_thread;
    size_t handoff_count = 0;
    std::atomic<bool> continuation_resumed{false};
};

class scheduler_probe_task_t
{
  public:
    struct promise_type
    {
        scheduler_probe_task_t get_return_object ()
        {
            return scheduler_probe_task_t (
              std::coroutine_handle<promise_type>::from_promise (*this));
        }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_always final_suspend () noexcept { return {}; }
        void return_void () noexcept {}
        void unhandled_exception () noexcept { failure = std::current_exception (); }

        void zlink_bind_continuation_scheduler (
          perf::application_ready_queue_t &queue_) noexcept
        {
            ready_queue = &queue_;
        }

        std::function<void (std::function<void ()>)>
        zlink_continuation_scheduler ()
        {
            if (!ready_queue || !probe)
                return {};

            auto enqueue = ready_queue->continuation_scheduler ();
            scheduler_probe_t *const bound_probe = probe;
            return [enqueue = std::move (enqueue), bound_probe] (
                     std::function<void ()> work_) mutable {
                {
                    std::lock_guard<std::mutex> lock (bound_probe->mutex);
                    bound_probe->handoff_thread = std::this_thread::get_id ();
                    ++bound_probe->handoff_count;
                }
                enqueue (std::move (work_));
            };
        }

        perf::application_ready_queue_t *ready_queue = nullptr;
        scheduler_probe_t *probe = nullptr;
        std::exception_ptr failure;
    };

    scheduler_probe_task_t (scheduler_probe_task_t &&other_) noexcept :
        _handle (std::exchange (other_._handle, {}))
    {
    }
    scheduler_probe_task_t (const scheduler_probe_task_t &) = delete;
    ~scheduler_probe_task_t ()
    {
        if (_handle)
            _handle.destroy ();
    }

    bool done () const noexcept { return !_handle || _handle.done (); }

    void get ()
    {
        require (done (), "scheduler probe task did not complete");
        if (_handle.promise ().failure)
            std::rethrow_exception (_handle.promise ().failure);
    }

  private:
    explicit scheduler_probe_task_t (
      std::coroutine_handle<promise_type> handle_) noexcept :
        _handle (handle_)
    {
    }

    std::coroutine_handle<promise_type> _handle;
};

struct bind_scheduler_probe_t
{
    scheduler_probe_t &probe;

    bool await_ready () const noexcept { return false; }

    template <typename TPromise>
    bool await_suspend (std::coroutine_handle<TPromise> continuation_) noexcept
    {
        continuation_.promise ().probe = &probe;
        return false;
    }

    void await_resume () const noexcept {}
};

struct small_hwm_pair_fixture_t
{
    zlink::context_t ctx;
    std::unique_ptr<zlink::pair_socket_t> sender;
    std::unique_ptr<zlink::pair_socket_t> receiver;

    explicit small_hwm_pair_fixture_t (const char *name_)
    {
        ctx.options ().auto_hwm_enabled (false);
        sender = std::make_unique<zlink::pair_socket_t> (ctx);
        receiver = std::make_unique<zlink::pair_socket_t> (ctx);

        // The public option is a byte budget. Four test payloads are enough to
        // create deterministic pressure without guessing Core's private
        // message-record layout.
        const uint64_t hwm = UINT64_C (256);
        sender->options ().send_hwm (zlink::byte_count_t::bytes (hwm));
        sender->options ().send_timeout (std::chrono::seconds (2));
        sender->options ().linger (std::chrono::milliseconds::zero ());
        receiver->options ().recv_hwm (zlink::byte_count_t::bytes (hwm));
        receiver->options ().recv_timeout (std::chrono::seconds (2));
        receiver->options ().linger (std::chrono::milliseconds::zero ());

        const std::string endpoint = zlink_cpp_contract::unique_inproc (name_);
        receiver->bind (endpoint);
        sender->connect (endpoint);

        zlink::message_t warmup = zlink_cpp_contract::make_message ("warmup");
        require (sender->send ().message (warmup).submit (),
                 "small-HWM pair failed its connection warmup send");
        zlink::message_t received;
        require (receiver->recv (received) == 0
                   && received.to_string () == "warmup",
                 "small-HWM pair failed its connection warmup receive");
    }

    void fill_until_backpressured ()
    {
        const std::string filler_text (64, 'h');
        for (int attempt = 0; attempt < 64; ++attempt) {
            zlink::message_t filler =
              zlink_cpp_contract::make_message (filler_text);
            if (!sender->send ()
                   .message (filler)
                   .flags (zlink::send_flags_t::dontwait)
                   .submit ())
                return;
        }
        throw std::runtime_error ("small HWM did not become backpressured");
    }

    void drain_one ()
    {
        zlink::message_t drained;
        require (receiver->recv (drained) == 0,
                 "small-HWM receiver could not release one send credit");
    }
};

struct pending_completion_t
{
    explicit pending_completion_t (bool &abandoned_) noexcept : abandoned (&abandoned_) {}
    pending_completion_t (pending_completion_t &&other_) noexcept :
        abandoned (std::exchange (other_.abandoned, nullptr))
    {
    }
    pending_completion_t (const pending_completion_t &) = delete;
    ~pending_completion_t ()
    {
        if (abandoned)
            *abandoned = true;
    }

    bool await_ready () const noexcept { return false; }
    bool await_suspend (std::coroutine_handle<>) const noexcept { return true; }
    void await_resume () const noexcept {}

    bool *abandoned;
};

perf::async_task_t<void> run_two_fair_turns (perf::application_ready_queue_t &ready_queue_,
                                             int socket_,
                                             std::vector<int> &order_)
{
    co_await ready_queue_.schedule ();
    order_.push_back (socket_);
    co_await ready_queue_.schedule ();
    order_.push_back (socket_);
}

scheduler_probe_task_t run_public_send (
  perf::application_ready_queue_t &ready_queue_,
  zlink::pair_socket_t &sender_,
  scheduler_probe_t &probe_)
{
    co_await bind_scheduler_probe_t{probe_};
    co_await ready_queue_.schedule ();
    zlink::message_t payload =
      zlink_cpp_contract::make_message (std::string (64, 'p'));
    co_await std::move (sender_.send ()).message (payload).async ();
    probe_.continuation_resumed.store (true, std::memory_order_release);
}

perf::async_task_t<void> run_pending_completion (
  perf::application_ready_queue_t &ready_queue_, bool &abandoned_)
{
    co_await ready_queue_.schedule ();
    co_await pending_completion_t (abandoned_);
}

struct withheld_reply_close_probe_t
{
    explicit withheld_reply_close_probe_t (
      perf::application_ready_queue_t &ready_queue_,
      std::shared_ptr<perf::multi::reqrep::client_completion_state_t> completion_) :
        ready_queue (&ready_queue_),
        completion (std::move (completion_))
    {
    }

    bool valid () const noexcept { return open; }

    void close ()
    {
        ++close_attempts;
        if (close_attempts == 1)
            throw zlink::close_error_t (zlink::close_result_t::busy);

        open = false;
        ready_queue->continuation_scheduler () ([completion = completion] {
            completion->outstanding.fetch_sub (1, std::memory_order_release);
        });
    }

    perf::application_ready_queue_t *ready_queue;
    std::shared_ptr<perf::multi::reqrep::client_completion_state_t> completion;
    unsigned int close_attempts = 0;
    bool open = true;
};

void test_reqrep_withheld_reply_close_quiesces_before_teardown ()
{
    perf::application_ready_queue_t ready_queue;
    auto completion =
      std::make_shared<perf::multi::reqrep::client_completion_state_t> (64);
    completion->outstanding.store (1, std::memory_order_release);

    std::vector<std::unique_ptr<perf::multi::reqrep::client_slot_t<
      withheld_reply_close_probe_t>>>
      slots;
    auto slot = std::make_unique<perf::multi::reqrep::client_slot_t<
      withheld_reply_close_probe_t>> ();
    withheld_reply_close_probe_t *const probe =
      (slot->socket = std::make_unique<withheld_reply_close_probe_t> (
         ready_queue, completion)).get ();
    slots.emplace_back (std::move (slot));

    perf::multi::reqrep::close_requesters_and_drain (slots, ready_queue, completion);

    require (probe->close_attempts == 2,
             "reqrep exceptional close did not retry an in-flight callback");
    require (completion->outstanding.load (std::memory_order_acquire) == 0,
             "withheld reply remained live after close quiescence");
    require (ready_queue.run_ready_round () == 0,
             "reqrep close quiescence left a continuation queued for teardown");
}

void test_public_send_handoff_precedes_poller_return ()
{
    small_hwm_pair_fixture_t fixture ("cpp-perf-pollcompletion-handoff");
    zlink::poller_t poller;
    poller.add (*fixture.sender, zlink::poll_event_flag_t::pollcompletion, 17);
    fixture.fill_until_backpressured ();

    perf::application_ready_queue_t ready_queue;
    scheduler_probe_t probe;
    scheduler_probe_task_t send =
      run_public_send (ready_queue, *fixture.sender, probe);
    require (ready_queue.run_ready_round (1) == 1,
             "pending public send did not enter its application admission round");
    require (!send.done (),
             "backpressured public send completed before receiver credit was released");

    fixture.drain_one ();
    zlink::poll_event_t event;
    const std::thread::id application_thread = std::this_thread::get_id ();
    require (poller.wait (&event, 1, std::chrono::seconds (2)) == 1,
             "pending public send did not wake its completion poller");
    require (event.slot == 17
               && perf::poll_event_has (event.revents,
                                        zlink::poll_event_flag_t::pollcompletion),
             "public send wake did not report POLLCOMPLETION");

    {
        std::lock_guard<std::mutex> lock (probe.mutex);
        require (probe.handoff_count == 1,
                 "poller returned before the continuation scheduler accepted work");
        require (probe.handoff_thread == application_thread,
                 "completion scheduler handoff left the poller/application thread");
    }
    require (!probe.continuation_resumed.load (std::memory_order_acquire),
             "completion resumed inline instead of entering the application queue");
    require (ready_queue.run_ready_round (1) == 1,
             "poller-owned completion was not visible to the next ready round");
    require (probe.continuation_resumed.load (std::memory_order_acquire),
             "queued public-send continuation did not resume");
    require (send.done (), "public-send coroutine did not reach final suspend");
    send.get ();
}

void test_stop_token_wait_ignores_active_drain_deadline ()
{
    small_hwm_pair_fixture_t fixture ("cpp-perf-stop-backpressure");
    fixture.fill_until_backpressured ();

    perf::application_ready_queue_t ready_queue;
    std::atomic<bool> stop_requested{false};
    perf::async_task_t<bool> stop =
      perf::multi::send_stop_token_until_admitted (
        ready_queue, fixture.sender.get (), stop_requested);
    require (ready_queue.run_ready_round (1) == 1,
             "stop-token task did not enter its application admission round");
    require (!stop.done (),
             "backpressured stop token unexpectedly completed immediately");

    std::this_thread::sleep_for (std::chrono::milliseconds (1100));
    require (!stop.done (),
             "stop-token admission inherited the one-second active drain deadline");

    fixture.drain_one ();
    const auto test_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!stop.done ()) {
        require (ready_queue.wait_and_run_ready_round_until (test_deadline, 1) != 0,
                 "stop-token completion did not reach the application queue");
    }
    require (stop.get (), "stop token did not complete after backpressure release");

    bool received_stop = false;
    for (int attempt = 0; attempt < 64 && !received_stop; ++attempt) {
        zlink::message_t received;
        require (fixture.receiver->recv (received) == 0,
                 "stop token was admitted but not delivered");
        received_stop = received.to_string () == perf::multi::k_stop_token;
    }
    require (received_stop,
             "per-socket blocking stop task did not publish the stop token");
}

void test_active_senders_keep_core_default_admission_timeout ()
{
    const std::filesystem::path repo_root =
      std::filesystem::path (__FILE__).parent_path ().parent_path ().parent_path ()
        .parent_path ().parent_path ();
    const std::filesystem::path sender_dir =
      repo_root / "bindings/cpp/perf/multi/src";
    const char *const sender_sources[] = {
      "perf_dealer_dealer_client.cpp",
      "perf_dealer_router_client.cpp",
      "perf_router_router_client.cpp",
    };

    for (const char *const sender_source : sender_sources) {
        std::ifstream input (sender_dir / sender_source);
        require (input.good (), "missing C++ multi sender source");
        const std::string source ((std::istreambuf_iterator<char> (input)),
                                  std::istreambuf_iterator<char> ());
        require (source.find (".timeout (") == std::string::npos,
                 "multi sender overrode Core's default async admission timeout");
        require (source.find ("send_drain_timeout_ms") != std::string::npos,
                 "multi sender lost its outer post-active drain bound");
        if (std::string (sender_source) == "perf_dealer_router_client.cpp")
            require (source.find ("submit_result_t::not_connected")
                       != std::string::npos,
                     "dealer-router sender did not retry pre-route async admission");
    }
}

void test_multi_runner_owns_io_threads_alias ()
{
    const std::filesystem::path repo_root =
      std::filesystem::path (__FILE__).parent_path ().parent_path ().parent_path ()
        .parent_path ().parent_path ();
    std::ifstream input (
      repo_root / "bindings/cpp/perf/run_binding_multi.sh");
    require (input.good (), "missing C++ multi runner");
    const std::string source ((std::istreambuf_iterator<char> (input)),
                              std::istreambuf_iterator<char> ());
    const size_t alias_begin = source.find ("    --io-threads)");
    const size_t alias_end = source.find ("    --server-io-threads)", alias_begin);
    require (alias_begin != std::string::npos && alias_end != std::string::npos,
             "multi runner lost the --io-threads alias");
    require (source.find ("SCRIPT_ARGS", alias_begin) >= alias_end,
             "multi runner forwarded --io-threads to the comparison runner");
    require (source.find (
               "SERVER_IO_THREADS=\"${SERVER_IO_THREADS:-${COMMON_IO_THREADS}}\"")
               != std::string::npos
               && source.find (
                    "CLIENT_IO_THREADS=\"${CLIENT_IO_THREADS:-${COMMON_IO_THREADS}}\"")
                    != std::string::npos,
             "multi runner did not map --io-threads to both benchmark roles");
}

void test_remote_snapshot_defers_reentrant_enqueue ()
{
    perf::application_ready_queue_t ready_queue;
    std::vector<int> order;
    const auto schedule = ready_queue.continuation_scheduler ();
    schedule ([&] {
        order.push_back (1);
        schedule ([&] { order.push_back (3); });
    });
    schedule ([&] { order.push_back (2); });

    require (ready_queue.run_ready_round () == 2,
             "remote snapshot did not run its initial callbacks");
    require (order == std::vector<int> ({1, 2}),
             "reentrant remote enqueue ran in the same fairness round");
    require (ready_queue.run_ready_round () == 1,
             "deferred remote callback did not reach the next round");
    require (order == std::vector<int> ({1, 2, 3}),
             "remote callback order did not preserve the snapshot boundary");
}

void test_dealer_dealer_counter_stays_application_owned ()
{
    const std::filesystem::path repo_root =
      std::filesystem::path (__FILE__).parent_path ().parent_path ().parent_path ()
        .parent_path ().parent_path ();
    std::ifstream input (
      repo_root / "bindings/cpp/perf/multi/src/perf_dealer_dealer_client.cpp");
    require (input.good (), "missing C++ dealer-dealer sender source");
    const std::string source ((std::istreambuf_iterator<char> (input)),
                              std::istreambuf_iterator<char> ());
    const size_t sender = source.find ("perf::async_task_t<bool> run_sender");
    const size_t loop = source.find (
      "while (std::chrono::steady_clock::now () < deadline)", sender);
    const size_t sender_end = source.find (
      "perf::async_task_t<bool> run_phase", loop);
    require (source.find ("unsigned long long &count") != std::string::npos
               && source.find ("std::atomic<unsigned long long> count") == std::string::npos,
             "dealer-dealer counter is not single-owner application state");
    require (sender != std::string::npos && loop != std::string::npos
               && sender_end != std::string::npos
               && source.find ("co_await ready_queue.schedule ();", sender) < loop
               && source.find ("co_await ready_queue.schedule ();", loop) >= sender_end,
             "dealer-dealer sender yields between immediately admitted sends");
}

void test_reqrep_completion_owner_is_the_application_queue ()
{
    const std::filesystem::path repo_root =
      std::filesystem::path (__FILE__).parent_path ().parent_path ().parent_path ()
        .parent_path ().parent_path ();
    std::ifstream input (
      repo_root / "bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp");
    require (input.good (), "missing C++ multi reqrep source");
    const std::string source ((std::istreambuf_iterator<char> (input)),
                              std::istreambuf_iterator<char> ());
    require (source.find ("poll_event_flag_t::pollcompletion") == std::string::npos
               && source.find ("bind_application_ready_queue_t") != std::string::npos
               && source.find ("wait_and_run_ready_round_until") != std::string::npos,
             "reqrep completion did not stay on the application ready queue");

}

void test_reqrep_reuses_only_pre_admission_payload ()
{
    const std::filesystem::path repo_root =
      std::filesystem::path (__FILE__).parent_path ().parent_path ().parent_path ()
        .parent_path ().parent_path ();
    std::ifstream input (
      repo_root / "bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp");
    require (input.good (), "missing C++ multi reqrep source");
    const std::string source ((std::istreambuf_iterator<char> (input)),
                              std::istreambuf_iterator<char> ());
    const size_t post_admission = source.find ("// Post-admission:");
    const size_t post_completion = source.find (
      "completion_->outstanding.fetch_sub", post_admission);
    const size_t post_logical = source.find ("logical_->", post_admission);
    require (source.find ("std::make_shared<logical_request_t>") == std::string::npos
               && source.find ("slot_.logical.payload") != std::string::npos
               && post_admission != std::string::npos
               && post_completion != std::string::npos
               && (post_logical == std::string::npos || post_logical >= post_completion),
             "reqrep reusable logical payload crossed the admission boundary");
}
} // namespace

int main ()
{
    perf::application_ready_queue_t ready_queue;
    std::vector<int> order;
    std::vector<perf::async_task_t<void>> senders;
    senders.emplace_back (run_two_fair_turns (ready_queue, 1, order));
    senders.emplace_back (run_two_fair_turns (ready_queue, 2, order));

    require (order.empty (), "eager sender ran before the first admission round");
    require (ready_queue.run_ready_round () == 2,
             "first admission round did not run both sockets");
    require (order == std::vector<int> ({1, 2}),
             "a socket advanced twice in one admission round");
    require (ready_queue.run_ready_round () == 2,
             "second admission round did not run both sockets");
    require (order == std::vector<int> ({1, 2, 1, 2}),
             "socket admission order was not round-robin fair");
    require (senders[0].done () && senders[1].done (),
             "fair sender tasks did not reach their terminal state");
    senders[0].get ();
    senders[1].get ();

    test_public_send_handoff_precedes_poller_return ();
    test_stop_token_wait_ignores_active_drain_deadline ();
    test_active_senders_keep_core_default_admission_timeout ();
    test_multi_runner_owns_io_threads_alias ();
    test_remote_snapshot_defers_reentrant_enqueue ();
    test_dealer_dealer_counter_stays_application_owned ();
    test_reqrep_completion_owner_is_the_application_queue ();
    test_reqrep_reuses_only_pre_admission_payload ();
    test_reqrep_withheld_reply_close_quiesces_before_teardown ();

    require (!perf::poll_event_has (zlink::poll_event_flag_t::pollcompletion,
                                    zlink::poll_event_flag_t::pollin),
             "completion-only wake was classified as payload readiness");
    require (perf::poll_event_has (
               zlink::poll_event_flag_t::pollin
                 | zlink::poll_event_flag_t::pollcompletion,
               zlink::poll_event_flag_t::pollin),
             "combined completion/read wake lost payload readiness");

    const auto expired = std::chrono::steady_clock::now ()
                         - std::chrono::milliseconds (1);
    require (perf::multi::remaining_bounded_timeout (expired)
               == std::chrono::milliseconds::zero (),
             "expired drain deadline did not produce a zero wait");
    static_assert (perf::multi::default_send_drain_timeout_ms == 1000);

    bool pending_abandoned = false;
    {
        perf::async_task_t<void> pending =
          run_pending_completion (ready_queue, pending_abandoned);
        require (ready_queue.run_ready_round () == 1,
                 "pending task did not consume its local ready snapshot");
        require (!pending.done (),
                 "pending public operation reached terminal state unexpectedly");
        // The task is destroyed only after its local ready handle was removed.
        // Its suspended awaiter models public-operation cancellation at the
        // bounded drain deadline.
    }
    require (pending_abandoned,
             "bounded drain teardown did not abandon the pending operation");
    require (ready_queue.run_ready_round () == 0,
             "bounded drain teardown left a raw local coroutine handle queued");
    return 0;
}
