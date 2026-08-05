/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/channels/channel_runtime.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/eventing/runtime_wake_pipe.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/locations/location_runtime.hpp"

#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/handlers/handler_registry.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::runtime::fanout
{

class fanout_location_runtime_t
{
  public:
    fanout_location_runtime_t (
      message_bus_t bus,
      std::vector<channel_snapshot_t> channels,
      location_runtime_t &locations,
      location_repository_t &store,
      location_repository_t &leases,
      service_provider_t &services,
      serializer_registry_t &serializers,
      const handler_registry_t &handlers);
    ~fanout_location_runtime_t () noexcept;

    fanout_location_runtime_t (
      const fanout_location_runtime_t &) = delete;
    fanout_location_runtime_t &operator= (
      const fanout_location_runtime_t &) = delete;

    void start ();
    void stop () noexcept;
    bool empty () const noexcept;

  private:
    struct publisher_entry_t;
    struct subscriber_entry_t;

    void start_publisher (
      const channel_snapshot_t &channel,
      const location_owner_token_t &owner);
    void start_subscriber (
      const channel_snapshot_t &channel);
    void run ();
    void publish_descriptors ();
    void reconcile_subscribers ();
    void reconcile_subscriber (
      subscriber_entry_t &subscriber);
    void pump ();
    void wait_for_activity (std::chrono::milliseconds timeout) noexcept;
    void stop_publishers () noexcept;
    void stop_subscribers () noexcept;
    result_t<void> publish (
      const std::string &channel_name,
      std::string topic,
      std::string packet_name,
      std::string content_type,
      zlink::message_t message,
      std::chrono::milliseconds timeout);
    bool owner_is_live (
      const fanout_publisher_descriptor_t &descriptor) const;
    static std::uint64_t make_lifecycle_generation ();

    message_bus_t _bus;
    detail::channel_runtime_t _channel_runtime;
    std::vector<channel_snapshot_t> _channels;
    location_runtime_t *_locations;
    location_repository_t *_store;
    location_repository_t *_leases;
    service_provider_t *_services;
    serializer_registry_t *_serializers;
    const handler_registry_t *_handlers;
    std::unique_ptr<zlink::poller_t> _subscriber_poller;
    eventing::runtime_wake_pipe_t _wake_pipe;
    mutable std::mutex _gate;
    std::map<std::string, std::unique_ptr<publisher_entry_t>>
      _publishers;
    std::map<std::string, std::unique_ptr<subscriber_entry_t>>
      _subscribers;
    std::size_t _subscriber_pump_cursor = 0;
    std::atomic_bool _stop{false};
    std::thread _thread;
};

} // namespace zlink::framework::runtime::fanout
