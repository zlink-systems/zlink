/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Configuration/publisher_options.hpp"

#include <nlohmann/json.hpp>

namespace zlink::framework::e2e::pubsub::server::publisher
{

inline zlink::framework::http_response_t publish_from_query (
  zlink::framework::publisher_t &publisher,
  server::operational_evidence_store_t &evidence,
  const zlink::framework::http_request_t &request,
  const char *packet_name = nullptr)
{
    const auto topic = request.query_values.find ("topic");
    const auto value = request.query_values.find ("value");
    if (topic == request.query_values.end () || value == request.query_values.end ()) {
        zlink::framework::http_response_t response;
        response.status = 400;
        response.body = R"({"error":"topic and value are required"})";
        return response;
    }

    /* packet_name이 주어지면 handler가 없는 negative wire 변형으로 발행한다. */
    if (packet_name != nullptr) {
        publisher.publish (event_channel, topic->second, missing_event_t{value->second})
          .submit ();
    } else {
        publisher.publish (event_channel, topic->second, event_t{value->second}).submit ();
    }
    evidence.add (std::string ("published|topic=") + topic->second + "|value=" + value->second
                  + "|packet=" + (packet_name == nullptr ? "Event" : packet_name));

    zlink::framework::http_response_t response;
    response.body = nlohmann::json{
      {"status", "published"}, {"topic", topic->second}, {"value", value->second}}
                      .dump ();
    return response;
}

class publish_event_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::publisher_t,
                                          server::operational_evidence_store_t>;

    publish_event_handler_t (zlink::framework::publisher_t &publisher,
                             server::operational_evidence_store_t &evidence) :
        _publisher (publisher), _evidence (evidence)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        return publish_from_query (_publisher, _evidence, request);
    }

  private:
    zlink::framework::publisher_t &_publisher;
    server::operational_evidence_store_t &_evidence;
};

class publish_missing_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::publisher_t,
                                          server::operational_evidence_store_t>;

    publish_missing_handler_t (zlink::framework::publisher_t &publisher,
                               server::operational_evidence_store_t &evidence) :
        _publisher (publisher), _evidence (evidence)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        return publish_from_query (_publisher, _evidence, request, "MissingEvent");
    }

  private:
    zlink::framework::publisher_t &_publisher;
    server::operational_evidence_store_t &_evidence;
};

} // namespace zlink::framework::e2e::pubsub::server::publisher
