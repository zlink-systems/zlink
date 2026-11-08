/* SPDX-License-Identifier: MPL-2.0 */

#include "../contract/support.hpp"

#include <array>
#include <barrier>
#include <iostream>
#include <map>
#include <thread>
#include <type_traits>

namespace
{
constexpr int workers = 4;
struct counts_t
{
    int ok = 0;
    int terminated = 0;
    int close_ok = 0;
    int close_busy = 0;
    int ownership_failures = 0;
    std::map<std::pair<int, int>, int> errors;
};

// Build before releasing close. The builder must not use its cached handle
// when close wins between construction and native admission.
template <typename Factory>
void race (zlink::socket_t &socket_, Factory factory_, counts_t &counts_)
{
    std::barrier start (workers + 1);
    std::array<int, workers> results{};
    std::array<int, workers> errors{};
    std::array<bool, workers> ownership{};
    std::array<std::thread, workers> threads;
    for (int i = 0; i != workers; ++i) {
        threads[i] = std::thread ([&, i] {
            auto message = zlink_cpp_contract::make_message ("race");
            auto operation = factory_ (message);
            start.arrive_and_wait ();
            try {
                if constexpr (std::is_same_v<decltype (operation),
                                             zlink::request_submit_operation_t>) {
                    auto submitted = std::move (operation).async ();
                    results[i] = static_cast<int> (submitted.result);
                } else {
                    std::move (operation).submit ();
                }
                ownership[i] = results[i] == 0 ? !message.valid () : message.valid ();
            }
            catch (const zlink::submit_error_t &error) {
                results[i] = static_cast<int> (error.result ());
                errors[i] = error.internal_errno ();
                ownership[i] = message.valid ();
            }
            catch (const zlink::binding_error_t &error) {
                results[i] = -1;
                errors[i] = error.internal_errno ();
            }
        });
    }
    start.arrive_and_wait ();
    try {
        socket_.close ();
        ++counts_.close_ok;
    }
    catch (const zlink::close_error_t &error) {
        assert (error.result () == zlink::close_result_t::busy);
        assert (error.internal_errno () == EBUSY);
        ++counts_.close_busy;
    }
    for (auto &thread : threads)
        thread.join ();
    socket_.close ();
    for (int i = 0; i != workers; ++i) {
        if (results[i] == 0)
            ++counts_.ok;
        else if (results[i] == static_cast<int> (zlink::submit_result_t::terminated)
                 && errors[i] == ESHUTDOWN)
            ++counts_.terminated;
        else
            ++counts_.errors[{results[i], errors[i]}];
        if (!ownership[i])
            ++counts_.ownership_failures;
    }
}

void round (const std::string &kind_, counts_t &counts_)
{
    zlink::context_t context;
    if (kind_ == "publish") {
        zlink::pub_socket_t publisher (context);
        race (
          publisher, [&] (auto &message) { return publisher.publish ("topic").message (message); },
          counts_);
        return;
    }
    zlink::router_socket_t server (context);
    zlink::dealer_socket_t client (context);
    server.options ().recv_timeout (std::chrono::seconds (5));
    client.options ().send_timeout (std::chrono::seconds (5));
    const auto endpoint = zlink_cpp_contract::unique_inproc ("submit-close-race");
    server.bind (endpoint);
    client.connect (endpoint);
    auto probe = zlink_cpp_contract::make_message ("ready");
    client.send ().message (probe).submit ();
    zlink::received_t ready;
    assert (server.recv (ready) == 0);
    if (kind_ == "send") {
        race (client, [&] (auto &message) { return client.send ().message (message); }, counts_);
    } else if (kind_ == "request") {
        zlink::poller_t poller;
        poller.add (client, zlink::poll_event_flag_t::pollcompletion, 1);
        race (client, [&] (auto &message) { return client.request ().message (message); }, counts_);
    } else {
        zlink::poller_t poller;
        poller.add (client, zlink::poll_event_flag_t::pollcompletion, 1);
        std::array<zlink::received_t, workers> received;
        std::vector<zlink::request_submission_t> requests;
        for (int i = 0; i != workers; ++i) {
            auto message = zlink_cpp_contract::make_message ("request");
            requests.push_back (client.request ().message (message).async ());
            assert (server.recv (received[i]) == 0);
        }
        std::atomic<int> next{0};
        race (
          server,
          [&] (auto &message) { return received[next.fetch_add (1)].reply ().message (message); },
          counts_);
    }
}
} // namespace

int main (int argc_, char **argv_)
{
    std::cout << std::unitbuf;
    const std::string kind = argc_ > 1 ? argv_[1] : "all";
    const int repetitions = argc_ > 2 ? std::stoi (argv_[2]) : 1000;
    int unexpected = 0;
    for (const std::string path : {"send", "request", "reply", "publish"}) {
        if (kind != "all" && kind != path)
            continue;
        counts_t counts;
        for (int i = 0; i != repetitions; ++i)
            round (path, counts);
        std::cout << path << " repetitions=" << repetitions << " ok=" << counts.ok
                  << " terminated=" << counts.terminated << " close_ok=" << counts.close_ok
                  << " close_busy=" << counts.close_busy
                  << " ownership_failures=" << counts.ownership_failures << '\n';
        for (const auto &[error, count] : counts.errors) {
            std::cout << "unexpected result=" << error.first << " errno=" << error.second
                      << " count=" << count << '\n';
            unexpected += count;
        }
        unexpected += counts.ownership_failures;
    }
    return unexpected == 0 ? 0 : 1;
}
