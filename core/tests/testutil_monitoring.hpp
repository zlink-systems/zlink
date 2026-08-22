/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __TESTUTIL_MONITORING_HPP_INCLUDED__
#define __TESTUTIL_MONITORING_HPP_INCLUDED__

#include "../include/zlink.h"
#include "../src/runtime/core/internal_defs.hpp"
#include "utils/stdint.hpp"

#include <stddef.h>
#include <mutex>
#include <vector>

//  General, i.e. non-security specific, monitor utilities

int recv_monitor_event_from_socket (void *monitor_, zlink_monitor_event_t *event_, int flags_);

int get_monitor_event_with_timeout (void *monitor_, int *value_, char **address_, int timeout_);

//  Read one event off the monitor socket; return value and address
//  by reference, if not null, and event number by value. Returns -1
//  in case of error.
int get_monitor_event (void *monitor_, int *value_, char **address_);

void expect_monitor_event (void *monitor_, int expected_event_);

//  expects that one or more occurrences of the expected event are received
//  via the specified socket monitor
//  returns the number of occurrences of the expected event
//  interrupts, if a ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL with EPIPE, ECONNRESET
//  or ECONNABORTED occurs; in this case, 0 is returned
//  this should be investigated further, see
//  https://github.com/zlink/libzlink/issues/2644
int expect_monitor_event_multiple (void *server_mon_,
                                   int expected_event_,
                                   int expected_err_ = -1,
                                   bool optional_ = false);

int64_t get_monitor_event_v2 (void *monitor_,
                              uint64_t **value_,
                              char **local_address_,
                              char **remote_address_);

void expect_monitor_event_v2 (void *monitor_,
                              int64_t expected_event_,
                              const char *expected_local_address_ = NULL,
                              const char *expected_remote_address_ = NULL);

struct test_monitor_probe_t
{
    std::mutex sync;
    std::vector<uint64_t> events;
    //  Full records, parallel to events, so a test can assert the payload an
    //  operator actually receives and not only the event id.
    std::vector<zlink_monitor_event_t> records;
};

void *open_test_monitor_probe (void *socket_,
                               zlink_socket_monitor_event_mask_t events_,
                               test_monitor_probe_t *probe_);
void close_test_monitor_probe (void **monitor_p_, test_monitor_probe_t *probe_);

int test_monitor_probe_count (test_monitor_probe_t *probe_);
uint64_t test_monitor_probe_event_at (test_monitor_probe_t *probe_, int index_);
zlink_monitor_event_t test_monitor_probe_record_at (test_monitor_probe_t *probe_,
                                                    int index_);
bool test_monitor_probe_wait_count (test_monitor_probe_t *probe_, int expected_, int timeout_ms_);
bool test_monitor_probe_wait_no_additional (test_monitor_probe_t *probe_,
                                            int baseline_,
                                            int timeout_ms_);
bool test_monitor_probe_wait_event_after (test_monitor_probe_t *probe_,
                                          uint64_t expected_,
                                          int start_index_,
                                          int timeout_ms_,
                                          int *found_index_out_);

bool wait_monitor_event_routing_id (void *monitor_,
                                    void *activity_socket_,
                                    uint64_t expected_event_,
                                    unsigned char *routing_id_out_,
                                    size_t routing_id_size_,
                                    int timeout_ms_);

#endif
