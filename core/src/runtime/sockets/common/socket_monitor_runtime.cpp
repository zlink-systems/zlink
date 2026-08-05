/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_runtime.hpp"

namespace
{
std::string make_monitor_ready_key (const zlink::endpoint_uri_pair_t &endpoint_uri_pair_,
                                    const unsigned char *routing_id_,
                                    size_t routing_id_size_)
{
    std::string key = endpoint_uri_pair_.identifier ();
    if (key.empty ())
        key = endpoint_uri_pair_.remote;
    key.push_back ('\0');
    if (routing_id_ && routing_id_size_ > 0)
        key.append (reinterpret_cast<const char *> (routing_id_), routing_id_size_);
    key.push_back ('\0');
    key.append (reinterpret_cast<const char *> (
                  &endpoint_uri_pair_.connection_id),
                sizeof (endpoint_uri_pair_.connection_id));
    return key;
}

std::string
make_monitor_ready_endpoint_prefix (const zlink::endpoint_uri_pair_t &endpoint_uri_pair_)
{
    std::string prefix = endpoint_uri_pair_.identifier ();
    if (prefix.empty ())
        prefix = endpoint_uri_pair_.remote;
    prefix.push_back ('\0');
    return prefix;
}

//  A transport pair is identified by its pair id and generation. The peer
//  routing id is deliberately left out: the two lanes of one pair can learn
//  the peer identity at different moments, and a key that included it would
//  split the pair into two half-ready entries that never complete.
std::string make_transport_pair_ready_key (
  const zlink::endpoint_uri_pair_t &endpoint_uri_pair_,
  uint64_t pair_id_,
  uint64_t generation_)
{
    std::string key = make_monitor_ready_endpoint_prefix (endpoint_uri_pair_);
    key.push_back ('\0');
    key.append (reinterpret_cast<const char *> (&pair_id_), sizeof (pair_id_));
    key.append (reinterpret_cast<const char *> (&generation_), sizeof (generation_));
    return key;
}

}

uint32_t zlink::socket_monitor_runtime_t::ready_count () const
{
    return static_cast<uint32_t> (ready_connections.size ());
}

bool zlink::socket_monitor_runtime_t::mark_ready_connection (
  const endpoint_uri_pair_t &endpoint_uri_pair_,
  const unsigned char *routing_id_,
  size_t routing_id_size_,
  uint32_t *ready_count_out_)
{
    const bool inserted =
      ready_connections
        .insert (make_monitor_ready_key (endpoint_uri_pair_, routing_id_, routing_id_size_))
        .second;
    if (inserted && ready_count_out_)
        *ready_count_out_ = ready_count ();
    return inserted;
}

bool zlink::socket_monitor_runtime_t::erase_ready_connection (
  const endpoint_uri_pair_t &endpoint_uri_pair_,
  const unsigned char *routing_id_,
  size_t routing_id_size_,
  uint32_t *ready_count_out_)
{
    const bool erased = ready_connections.erase (make_monitor_ready_key (
                          endpoint_uri_pair_, routing_id_, routing_id_size_))
                        != 0;
    if (erased && ready_count_out_)
        *ready_count_out_ = ready_count ();
    return erased;
}

bool zlink::socket_monitor_runtime_t::erase_ready_connection_for_endpoint (
  const endpoint_uri_pair_t &endpoint_uri_pair_, uint32_t *ready_count_out_)
{
    const std::string prefix = make_monitor_ready_endpoint_prefix (endpoint_uri_pair_);
    for (std::set<std::string>::iterator it = ready_connections.begin ();
         it != ready_connections.end (); ++it) {
        if (it->compare (0, prefix.size (), prefix) != 0)
            continue;

        ready_connections.erase (it);
        if (ready_count_out_)
            *ready_count_out_ = ready_count ();
        return true;
    }

    return false;
}

bool zlink::socket_monitor_runtime_t::mark_transport_pair_lane_ready (
  const endpoint_uri_pair_t &endpoint_uri_pair_,
  transport_lane_t lane_,
  uint64_t pair_id_,
  uint64_t generation_)
{
    const std::string key =
      make_transport_pair_ready_key (endpoint_uri_pair_, pair_id_, generation_);
    uint8_t &lanes = transport_pair_ready_lanes[key];
    lanes |= lane_ == transport_lane_completion ? 0x02 : 0x01;
    if (lanes != 0x03)
        return false;

    transport_pair_ready_lanes.erase (key);
    return true;
}

void zlink::socket_monitor_runtime_t::erase_transport_pair_readiness_for_endpoint (
  const endpoint_uri_pair_t &endpoint_uri_pair_)
{
    const std::string prefix =
      make_monitor_ready_endpoint_prefix (endpoint_uri_pair_);
    for (std::map<std::string, uint8_t>::iterator it =
           transport_pair_ready_lanes.begin ();
         it != transport_pair_ready_lanes.end ();) {
        if (it->first.compare (0, prefix.size (), prefix) == 0)
            it = transport_pair_ready_lanes.erase (it);
        else
            ++it;
    }
}


void zlink::socket_monitor_runtime_t::reset_worker_state ()
{
    queue_sync.lock ();
    queue.clear ();
    queue_stop = false;
    task_running = false;
    queue_sync.unlock ();
}

void zlink::socket_monitor_runtime_t::start_task (uint64_t task_id_)
{
    queue_sync.lock ();
    task_id = task_id_;
    task_running = task_id_ != 0;
    queue_sync.unlock ();
}

bool zlink::socket_monitor_runtime_t::dequeue_worker_event_nowait (
  socket_monitor_event_record_t *out_)
{
    if (!out_)
        return false;

    queue_sync.lock ();
    if (queue_stop || queue.empty ()) {
        queue_sync.unlock ();
        return false;
    }

    *out_ = queue.front ();
    queue.pop_front ();
    queue_cv.broadcast ();
    queue_sync.unlock ();
    return true;
}

void zlink::socket_monitor_runtime_t::requeue_worker_event_front (
  const socket_monitor_event_record_t &record_)
{
    queue_sync.lock ();
    if (!queue_stop) {
        queue.push_front (record_);
        queue_cv.broadcast ();
    }
    queue_sync.unlock ();
}

void zlink::socket_monitor_runtime_t::enqueue_worker_event (
  const socket_monitor_event_record_t &record_, size_t hwm_)
{
    queue_sync.lock ();
    while (!queue_stop && !lossy && queue.size () >= hwm_)
        (void) queue_cv.wait (&queue_sync, -1);
    if (!queue_stop && queue.size () < hwm_) {
        queue.push_back (record_);
        queue_cv.broadcast ();
    }
    queue_sync.unlock ();
}

void zlink::socket_monitor_runtime_t::stop_task ()
{
    queue_sync.lock ();
    queue_stop = true;
    queue.clear ();
    queue_cv.broadcast ();
    task_running = false;
    task_id = 0;
    queue_sync.unlock ();
}
