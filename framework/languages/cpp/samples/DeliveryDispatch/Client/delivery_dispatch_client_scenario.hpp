/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Shared/Contracts/messages.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/http_client.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace zlink::samples::deliverydispatch
{

class delivery_dispatch_client_scenario_t
{
  public:
    bool run (const std::string &api_http_url,
              const std::string &customer_stream_endpoint,
              const std::string &courier_stream_endpoint)
    {
        try {
            zlink::stream_connector::connector_options_t connector_options;
            connector_options.endpoint = customer_stream_endpoint;
            connector_options.connect_timeout = std::chrono::seconds (5);
            connector_options.request_timeout = std::chrono::seconds (12);
            connector_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
            auto core_customer =
              zlink::stream_connector::connector_factory_t::create (connector_options);
            use_json_codec (core_customer);
            auto customer = zlink::stream_e2e_client::use (core_customer);
            auto customer_connected = customer.connect ().submit ();
            ensure (static_cast<bool> (customer_connected), "customer stream connect failed");

            connector_options.endpoint = courier_stream_endpoint;
            auto core_courier_a =
              zlink::stream_connector::connector_factory_t::create (connector_options);
            use_json_codec (core_courier_a);
            auto courier_a = zlink::stream_e2e_client::use (core_courier_a);
            auto courier_a_connected = courier_a.connect ().submit ();
            ensure (static_cast<bool> (courier_a_connected), "courier-a stream connect failed");

            auto core_courier_b =
              zlink::stream_connector::connector_factory_t::create (connector_options);
            use_json_codec (core_courier_b);
            auto courier_b = zlink::stream_e2e_client::use (core_courier_b);
            auto courier_b_connected = courier_b.connect ().submit ();
            ensure (static_cast<bool> (courier_b_connected), "courier-b stream connect failed");

            bind_courier (courier_a, "courier-a");
            bind_courier (courier_b, "courier-b");

            auto http = zlink::http_client::client_t::create (api_http_url)
                          .timeout (std::chrono::seconds (12))
                          .build ();
            run_successful_delivery (http, customer, courier_a);
            run_reassigned_delivery (http, customer, courier_a, courier_b);
            assert_server_evidence (http);
            return true;
        }
        catch (const std::exception &error) {
            std::cerr << "deliverydispatch scenario failed: " << error.what () << "\n";
            return false;
        }
    }

  private:
    using connector_t = zlink::stream_e2e_client::coroutine_connector_t;

    static void use_json_codec (zlink::stream_connector::connector_t &connector)
    {
        connector.codecs ()
          .enable_codec (zlink::stream_connector::codec_t::json)
          .use_default_codec (zlink::stream_connector::codec_t::json);
    }

    /* Actor placement는 Location Store가 결정한다. Scenario는 global CourierId와 bind 성공만
     * 검증하며 current owner NodeRid를 성공 조건으로 사용하지 않는다. */
    static void bind_courier (connector_t &courier, const std::string &courier_id)
    {
        const auto bound = courier.request (bind_courier_session_req_t{courier_id})
                             .async<bind_courier_session_res_t> ()
                             .result ();
        if (!bound) {
            throw std::runtime_error (bound.error () ? bound.error ()->message
                                                     : "courier bind failed");
        }
        ensure (bound.value ().courier_id == courier_id, "courier bind id mismatch");
    }

    static void run_successful_delivery (zlink::http_client::client_t &http,
                                         connector_t &customer,
                                         connector_t &courier)
    {
        const std::string delivery_id = "delivery-success";
        const auto subscribed =
          customer.request (subscribe_delivery_req_t{delivery_id})
            .async<subscribe_delivery_res_t> ()
            .result ();
        if (!subscribed) {
            throw std::runtime_error (subscribed.error () ? subscribed.error ()->message
                                                          : "delivery-success subscription failed");
        }
        ensure (subscribed && subscribed.value ().delivery_id == delivery_id,
                "delivery-success subscription failed");
        auto offer = wait_offer (courier, delivery_id, "courier-a");
        auto statuses = customer.wait_for_sequence<delivery_status_notify_t> ()
                          .expect ([delivery_id] (const delivery_status_notify_t &message) {
                              return message.delivery_id == delivery_id
                                     && message.status == delivery_status_t::assigned;
                          })
                          .expect ([delivery_id] (const delivery_status_notify_t &message) {
                              return message.delivery_id == delivery_id
                                     && message.status == delivery_status_t::accepted;
                          })
                          .expect ([delivery_id] (const delivery_status_notify_t &message) {
                              return message.delivery_id == delivery_id
                                     && message.status == delivery_status_t::picked_up;
                          })
                          .expect ([delivery_id] (const delivery_status_notify_t &message) {
                              return message.delivery_id == delivery_id
                                     && message.status == delivery_status_t::delivered;
                          })
                          .timeout (std::chrono::seconds (12))
                          .async ();

        auto created_future = std::async (std::launch::async, [&http, delivery_id] {
            return http.post ("/deliveries")
              .body (create_delivery_req_t{
                delivery_id, "customer-1", "Kitchen 12", "Customer Lobby"})
              .fetch<create_delivery_res_t> ();
        });
        const auto courier_offer = offer.get ();
        send_decision (courier, courier_offer.delivery_id, courier_offer.courier_id, true);
        auto created = created_future.get ();
        ensure (created.delivery_id == delivery_id, "delivery-success create failed");
        auto received = statuses.result ();
        ensure (static_cast<bool> (received), "delivery-success status sequence failed");
        ensure (received.value ()[0].courier_id == "courier-a", "assigned courier mismatch");
        ensure (received.value ()[1].courier_id == "courier-a", "accepted courier mismatch");
        ensure (received.value ()[2].courier_id == "courier-a", "picked-up courier mismatch");
        ensure (received.value ()[3].courier_id == "courier-a", "delivered courier mismatch");
    }

    static void run_reassigned_delivery (zlink::http_client::client_t &http,
                                         connector_t &customer,
                                         connector_t &courier_a,
                                         connector_t &courier_b)
    {
        const std::string delivery_id = "delivery-reassign";
        const auto subscribed =
          customer.request (subscribe_delivery_req_t{delivery_id})
            .async<subscribe_delivery_res_t> ()
            .result ();
        if (!subscribed) {
            throw std::runtime_error (subscribed.error () ? subscribed.error ()->message
                                                          : "delivery-reassign subscription failed");
        }
        ensure (subscribed && subscribed.value ().delivery_id == delivery_id,
                "delivery-reassign subscription failed");
        auto first_offer = wait_offer (courier_a, delivery_id, "courier-a");
        auto second_offer = wait_offer (courier_b, delivery_id, "courier-b");
        auto statuses = customer.wait_for_sequence<delivery_status_notify_t> ()
                          .expect ([delivery_id] (const delivery_status_notify_t &message) {
                              return message.delivery_id == delivery_id
                                     && message.status == delivery_status_t::assigned;
                          })
                          .expect ([delivery_id] (const delivery_status_notify_t &message) {
                              return message.delivery_id == delivery_id
                                     && message.status == delivery_status_t::reassigned;
                          })
                          .expect ([delivery_id] (const delivery_status_notify_t &message) {
                              return message.delivery_id == delivery_id
                                     && message.status == delivery_status_t::accepted;
                          })
                          .expect ([delivery_id] (const delivery_status_notify_t &message) {
                              return message.delivery_id == delivery_id
                                     && message.status == delivery_status_t::delivered;
                          })
                          .timeout (std::chrono::seconds (12))
                          .async ();

        auto created_future = std::async (std::launch::async, [&http, delivery_id] {
            return http.post ("/deliveries")
              .body (create_delivery_req_t{
                delivery_id, "customer-1", "Kitchen 12", "Customer Lobby"})
              .fetch<create_delivery_res_t> ();
        });
        (void) first_offer.get ();
        const auto accepted_offer = second_offer.get ();
        send_decision (courier_b, accepted_offer.delivery_id, accepted_offer.courier_id, true);
        auto created = created_future.get ();
        ensure (created.delivery_id == delivery_id, "delivery-reassign create failed");
        auto received = statuses.result ();
        ensure (static_cast<bool> (received), "delivery-reassign status sequence failed");
        ensure (received.value ()[0].courier_id == "courier-a", "assigned courier mismatch");
        ensure (received.value ()[1].courier_id == "courier-b", "reassigned courier mismatch");
        ensure (received.value ()[2].courier_id == "courier-b", "accepted courier mismatch");
        ensure (received.value ()[3].courier_id == "courier-b", "delivered courier mismatch");
        std::cout << "deliverydispatch-reassignment=completed\n";
    }

    static void assert_server_evidence (zlink::http_client::client_t &http)
    {
        auto assertion = http.post ("/self-check/assert")
                           .body (server_assertion_req_t{"delivery-success", "delivery-reassign"})
                           .fetch<server_assertion_res_t> ();
        ensure (assertion.passed, "server evidence assertion failed");
        std::cout << "deliverydispatch-server-evidence=completed\n";
    }

    static std::future<offer_delivery_notify_t>
    wait_offer (connector_t &courier, const std::string &delivery_id, const std::string &courier_id)
    {
        return courier.wait_for<offer_delivery_notify_t> ()
          .where ([delivery_id, courier_id] (const offer_delivery_notify_t &message) {
              return message.delivery_id == delivery_id && message.courier_id == courier_id;
          })
          .timeout (std::chrono::seconds (12))
          .to_future ("courier offer wait failed");
    }

    static void send_decision (connector_t &courier,
                               const std::string &delivery_id,
                               const std::string &courier_id,
                               bool accepted)
    {
        courier.send (courier_decision_msg_t{delivery_id, courier_id, accepted, ""}).submit ();
    }

    static void ensure (bool condition, const char *message)
    {
        if (!condition) {
            throw std::runtime_error (message);
        }
    }
};

} // namespace zlink::samples::deliverydispatch
