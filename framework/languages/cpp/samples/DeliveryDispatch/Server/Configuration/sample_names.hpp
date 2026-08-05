/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/Contracts/messages.hpp"

#include <string>

namespace zlink::samples::deliverydispatch
{

struct sample_names_t
{
    static constexpr const char *dispatch_route_channel = "deliverydispatch.dispatch";
    static constexpr const char *customer_actor_discovery = "delivery-customers";
    static constexpr const char *courier_actor_discovery = "delivery-couriers";
    static constexpr const char *tracking_route_channel = "deliverydispatch.tracking";
    static constexpr const char *customer_stream_node = "delivery-customer-stream";
    static constexpr const char *courier_stream_node = "delivery-courier-stream";
    static constexpr const char *customer_actor_type = "delivery-customer";
    static constexpr const char *courier_actor_instance_1 = "delivery-courier-node-1";
    static constexpr const char *courier_actor_instance_2 = "delivery-courier-node-2";
    static constexpr const char *dispatch_route_node = "delivery-dispatch-channel";
    static constexpr const char *tracking_route_node = "delivery-tracking-server";
    static constexpr const char *customer_gateway_route_node = "delivery-customer-gateway";
    static constexpr const char *courier_session_route_node = "delivery-courier-session";
    static constexpr const char *courier_actor_type = "delivery-courier";
    static constexpr const char *customer_id = "customer-1";

};

} // namespace zlink::samples::deliverydispatch
