#ifndef ZLINK_TEST_HELPER_INTERLEAVE_FIXTURE_HPP_INCLUDED
#define ZLINK_TEST_HELPER_INTERLEAVE_FIXTURE_HPP_INCLUDED

/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string.h>
#include <thread>
#include <vector>


namespace
{
void init_part (zlink_msg_t *part_, const char *text_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, strlen (text_)));
    memcpy (zlink_msg_data (part_), text_, strlen (text_));
}

void init_tagged_part (zlink_msg_t *part_, unsigned char kind_, int round_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, 1 + sizeof (round_)));
    unsigned char *data = static_cast<unsigned char *> (zlink_msg_data (part_));
    data[0] = kind_;
    memcpy (data + 1, &round_, sizeof (round_));
}

bool pair_has_no_record_for (void *receiver_, int timeout_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t rc = zlink_recv (
          receiver_, NULL, &parts, &part_count, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_OK) {
            zlink_multipart_close (parts, part_count);
            return false;
        }
        if (rc != ZLINK_RECV_NO_DATA)
            return false;
        msleep (1);
    }
    return true;
}

bool recv_pair_record_eventually (
  void *receiver_, const std::vector<std::string> &expected_parts_,
  int timeout_ms_ = 3000)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t rc = zlink_recv (
          receiver_, NULL, &parts, &part_count, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            msleep (1);
            continue;
        }
        if (rc != ZLINK_RECV_OK)
            return false;

        bool matches = part_count == expected_parts_.size ();
        for (size_t i = 0; matches && i < part_count; ++i) {
            matches = zlink_msg_size (&parts[i]) == expected_parts_[i].size ()
                      && memcmp (zlink_msg_data (&parts[i]),
                                 expected_parts_[i].data (),
                                 expected_parts_[i].size ()) == 0;
        }
        zlink_multipart_close (parts, part_count);
        return matches;
    }
    return false;
}

bool recv_pair_record_eventually (void *receiver_,
                                  std::vector<std::string> *parts_out_,
                                  int timeout_ms_)
{
    if (!parts_out_)
        return false;
    parts_out_->clear ();
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t rc = zlink_recv (
          receiver_, NULL, &parts, &part_count, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            msleep (1);
            continue;
        }
        if (rc != ZLINK_RECV_OK)
            return false;

        for (size_t i = 0; i != part_count; ++i) {
            parts_out_->push_back (std::string (
              static_cast<const char *> (zlink_msg_data (&parts[i])),
              zlink_msg_size (&parts[i])));
        }
        zlink_multipart_close (parts, part_count);
        return true;
    }
    return false;
}

bool recv_published_record_eventually (
  void *subscriber_, const char *expected_topic_,
  const std::vector<std::string> &expected_parts_, int timeout_ms_ = 3000)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        char topic[64];
        size_t topic_len = sizeof (topic);
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t rc = zlink_subscribe (
          subscriber_, NULL, &parts, &part_count, topic, &topic_len,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            msleep (1);
            continue;
        }
        if (rc != ZLINK_RECV_OK)
            return false;

        const size_t expected_topic_len = strlen (expected_topic_);
        bool matches = topic_len == expected_topic_len
                       && memcmp (topic, expected_topic_, topic_len) == 0
                       && part_count == expected_parts_.size ();
        for (size_t i = 0; matches && i < part_count; ++i) {
            matches = zlink_msg_size (&parts[i]) == expected_parts_[i].size ()
                      && memcmp (zlink_msg_data (&parts[i]),
                                 expected_parts_[i].data (),
                                 expected_parts_[i].size ()) == 0;
        }
        zlink_multipart_close (parts, part_count);
        return matches;
    }
    return false;
}

struct close_between_parts_probe_t
{
    close_between_parts_probe_t () : ready (false), go (false), done (false),
                                      result (ZLINK_CLOSE_INTERNAL_ERROR)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool ready;
    bool go;
    bool done;
    zlink_close_result_t result;
};

struct option_query_probe_t
{
    option_query_probe_t () : ready (false), go (false), done (false),
                               result (ZLINK_CONFIG_INTERNAL_ERROR),
                               value (-1), value_size (0)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool ready;
    bool go;
    bool done;
    zlink_config_result_t result;
    int value;
    size_t value_size;
};

struct one_call_send_probe_t
{
    one_call_send_probe_t () : ready (false), go (false), calling (false),
                                done (false), result (-1), terminal_errno (0)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool ready;
    bool go;
    std::atomic<bool> calling;
    std::atomic<bool> done;
    int result;
    int terminal_errno;
};

struct paired_close_probe_t
{
    paired_close_probe_t () : ready (0), go (false), local_done (false),
                              peer_done (false),
                              local_result (ZLINK_CLOSE_INTERNAL_ERROR),
                              peer_result (ZLINK_CLOSE_INTERNAL_ERROR),
                              local_errno (0), peer_errno (0)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    int ready;
    bool go;
    bool local_done;
    bool peer_done;
    zlink_close_result_t local_result;
    zlink_close_result_t peer_result;
    int local_errno;
    int peer_errno;
};

}

#endif
