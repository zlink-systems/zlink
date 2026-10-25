/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_runtime.hpp"
#include "core/control_runtime.hpp"

namespace
{
const uint32_t monitor_retry_interval_ms = 10;

std::string make_monitor_ready_key (const zlink::endpoint_uri_pair_t &endpoint_uri_pair_,
                                     const unsigned char *routing_id_,
                                     size_t routing_id_size_,
                                     uint64_t transport_pair_id_,
                                     uint64_t transport_pair_generation_)
{
    std::string key = endpoint_uri_pair_.identifier ();
    if (key.empty ())
        key = endpoint_uri_pair_.remote;
    key.push_back ('\0');
    if (transport_pair_id_ != 0) {
        // A paired transport has one public-ready identity whether it owns one
        // Application lane or a ROUTER-ROUTER Application/Completion pair.
        // The two ROUTER lanes can learn the peer RID at different times, so
        // the ready key must not split when one lane reports an empty RID.
        key.append (reinterpret_cast<const char *> (&transport_pair_id_),
                    sizeof (transport_pair_id_));
        key.append (reinterpret_cast<const char *> (&transport_pair_generation_),
                    sizeof (transport_pair_generation_));
    } else {
        if (routing_id_ && routing_id_size_ > 0)
            key.append (reinterpret_cast<const char *> (routing_id_),
                        routing_id_size_);
        key.push_back ('\0');
        const uint64_t connection_id = endpoint_uri_pair_.connection_id.load ();
        key.append (reinterpret_cast<const char *> (&connection_id),
                    sizeof (connection_id));
    }
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

// A count-two ROUTER-ROUTER pair is identified by its pair id and generation.
// The peer routing id is deliberately left out: the two lanes can learn the
// peer identity at different moments, and a key that included it would split
// the pair into two half-ready entries that never complete. Count-one pairs do
// not enter this accumulator.
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
  uint32_t *ready_count_out_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_)
{
    const bool inserted =
      ready_connections
        .insert (make_monitor_ready_key (endpoint_uri_pair_, routing_id_, routing_id_size_,
                                         transport_pair_id_, transport_pair_generation_))
        .second;
    if (inserted && ready_count_out_)
        *ready_count_out_ = ready_count ();
    return inserted;
}

bool zlink::socket_monitor_runtime_t::erase_ready_connection (
  const endpoint_uri_pair_t &endpoint_uri_pair_,
  const unsigned char *routing_id_,
  size_t routing_id_size_,
  uint32_t *ready_count_out_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_)
{
    const bool erased = ready_connections.erase (make_monitor_ready_key (
                          endpoint_uri_pair_, routing_id_, routing_id_size_,
                          transport_pair_id_, transport_pair_generation_))
                        != 0;
    if (erased && ready_count_out_)
        *ready_count_out_ = ready_count ();
    return erased;
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

bool zlink::socket_monitor_runtime_t::erase_transport_pair_readiness (
  const endpoint_uri_pair_t &endpoint_uri_pair_,
  uint64_t pair_id_,
  uint64_t generation_)
{
    if (pair_id_ == 0 || generation_ == 0)
        return false;
    return transport_pair_ready_lanes.erase (make_transport_pair_ready_key (
             endpoint_uri_pair_, pair_id_, generation_))
           != 0;
}


void zlink::socket_monitor_runtime_t::reset_worker_state (
  uint64_t hwm_bytes_, uint64_t event_accounted_bytes_)
{
    scoped_lock_t lock (queue_sync);
    queue.clear ();
    queue_hwm_bytes = hwm_bytes_;
    queue_accounted_bytes = 0;
    event_accounted_bytes = event_accounted_bytes_;
    queue_stop = false;
    task_id = 0;
}

void zlink::socket_monitor_runtime_t::start_task (uint64_t task_id_,
                                                control_runtime_t *control_)
{
    scoped_lock_t lock (queue_sync);
    task_id = task_id_;
    // Publish the task identity and any earlier queue admission under the
    // same mutex. A dormant task cannot run before this publication.
    if (control_ && !queue.empty ())
        (void) control_->schedule_task_after (task_id, 0);
}

const zlink::socket_monitor_event_record_t *
zlink::socket_monitor_runtime_t::front_worker_event_nowait ()
{
    scoped_lock_t lock (queue_sync);
    return queue_stop || queue.empty () ? NULL : &queue.front ();
}

void zlink::socket_monitor_runtime_t::retry_worker_event (
  control_runtime_t *control_)
{
    scoped_lock_t lock (queue_sync);
    // The failed record remains at the front; only its existing task slot
    // needs another deadline. No copy or queue insertion occurs on failure.
    if (!queue_stop && control_)
        (void) control_->schedule_task_after (task_id, monitor_retry_interval_ms);
}

void zlink::socket_monitor_runtime_t::complete_worker_event ()
{
    scoped_lock_t lock (queue_sync);
    zlink_assert (queue_accounted_bytes >= event_accounted_bytes);
    queue.pop_front ();
    queue_accounted_bytes -= event_accounted_bytes;
    queue_cv.broadcast ();
}

void zlink::socket_monitor_runtime_t::enqueue_worker_event (
  const socket_monitor_event_record_t &record_, control_runtime_t *control_)
{
    scoped_lock_t lock (queue_sync);
    bool has_capacity = queue_accounted_bytes == 0
                        || (event_accounted_bytes <= queue_hwm_bytes
                            && queue_accounted_bytes
                                 <= queue_hwm_bytes - event_accounted_bytes);
    while (!queue_stop && !lossy.load (std::memory_order_acquire)
           && !has_capacity) {
        (void) queue_cv.wait (&queue_sync, -1);
        has_capacity = queue_accounted_bytes == 0
                       || (event_accounted_bytes <= queue_hwm_bytes
                           && queue_accounted_bytes
                                <= queue_hwm_bytes - event_accounted_bytes);
    }
    if (!queue_stop && has_capacity) {
        queue.push_back (record_);
        queue_accounted_bytes += event_accounted_bytes;
        if (control_)
            (void) control_->schedule_task_after (task_id, 0);
    }
}

void zlink::socket_monitor_runtime_t::stop_task ()
{
    scoped_lock_t lock (queue_sync);
    queue_stop = true;
    queue.clear ();
    queue_accounted_bytes = 0;
    queue_cv.broadcast ();
    task_id = 0;
}
