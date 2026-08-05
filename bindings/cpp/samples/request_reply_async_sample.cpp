/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>

int main ()
{
    // --8<-- [start:doc]
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_monitor = dealer.monitor_open ();

    const zlink::routing_id_t client_id = zlink::routing_id_t::from (std::string ("REQ-CLIENT"));
    dealer.set_routing_id (client_id);
    router.bind ("tcp://127.0.0.1:0");
    const std::string endpoint = router.options ().last_endpoint ();
    dealer.connect (endpoint);
    assert (detail::wait_connected (router_monitor, dealer_monitor));

    // 응답이 도착하면 callback이 IO 스레드에서 실행된다. 결과를 main으로 넘기려면
    // 직접 동기화한다 — callback 안에서 소켓을 다시 호출하지 않는다.
    std::mutex mutex;
    std::condition_variable arrived;
    bool completed = false;
    zlink::request_result_t outcome = zlink::request_result_t::ok;
    std::string reply_payload;

    zlink::message_t request = detail::make_message (detail::k_dealer_router_request);
    const bool accepted =
      dealer.request ()
        .message (request)
        .timeout (std::chrono::milliseconds (2000)) // 응답이 없으면 timeout으로 완료된다.
        .submit ([&] (zlink::request_result_t result_, std::vector<zlink::message_t> parts_) {
            {
                std::lock_guard<std::mutex> lock (mutex);
                outcome = result_;
                if (result_ == zlink::request_result_t::ok && !parts_.empty ())
                    reply_payload = parts_[0].to_string ();
                completed = true;
            }
            for (auto &part : parts_)
                part.close (); // 응답 part의 소유권은 callback이 받는다.
            arrived.notify_one ();
        });
    assert (accepted);

    // 응답하는 쪽. 요청을 받아 같은 routing_id와 request_seq로 되돌려준다.
    std::thread responder ([&] {
        zlink::received_t inbound;
        assert (router.recv (inbound) == 0);
        assert (inbound.routing_id ().has_value ());
        assert (inbound.request_seq ().has_value ());
        zlink::message_t reply = detail::make_message (detail::k_dealer_router_reply);
        router.reply (*inbound.routing_id (), *inbound.request_seq ())
          .message (reply)
          .submit ();
        inbound.close ();
    });

    {
        std::unique_lock<std::mutex> lock (mutex);
        assert (arrived.wait_for (lock, std::chrono::seconds (5), [&] { return completed; }));
    }
    responder.join ();

    assert (outcome == zlink::request_result_t::ok);
    assert (reply_payload == detail::k_dealer_router_reply);
    // --8<-- [end:doc]

    std::printf ("[dealer-router/request-reply/callback] send: \"%s\" -> recv: \"%s\"\n",
                 detail::k_dealer_router_request,
                 reply_payload.c_str ());
    return 0;
}
