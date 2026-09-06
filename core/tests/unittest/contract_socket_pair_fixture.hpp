/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_TEST_CONTRACT_SOCKET_PAIR_FIXTURE_HPP
#define ZLINK_TEST_CONTRACT_SOCKET_PAIR_FIXTURE_HPP

#include "../testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "core/command.hpp"
#include "core/pipe.hpp"

// A transport-free pair of real socket owners. The fixture supplies the
// handshake result through the existing bind-command boundary, and executes
// mailbox commands synchronously. The caller owns and closes both sockets.
struct contract_socket_pair_t
{
    contract_socket_pair_t (void *first_, void *second_, uint64_t pair_id_ = 1,
                            uint64_t generation_ = 1, bool attach_ = true,
                            uint64_t hwm_ = 0) :
        pair_id (pair_id_), generation (generation_), attached (false)
    {
        sockets[0] = first_;
        sockets[1] = second_;
        for (size_t i = 0; i != 2; ++i) {
            cores[i] = as_socket_handle (sockets[i]).socket;
            TEST_ASSERT_NOT_NULL (cores[i]);
            memset (&rids[i], 0, sizeof (rids[i]));
            if (paired_type (cores[i]->socket_type ()))
                TEST_ASSERT_EQUAL_INT (
                  ZLINK_CONFIG_OK, zlink_get_routing_id (sockets[i], &rids[i]));
            application[i] = NULL;
            completion[i] = NULL;
        }
        lane_count = pair_id == 0 || !paired_type (cores[0]->socket_type ())
                         || !paired_type (cores[1]->socket_type ())
                       ? 0
                       : cores[0]->socket_type () == ZLINK_CORE_SOCKET_ROUTER
                             && cores[1]->socket_type () == ZLINK_CORE_SOCKET_ROUTER
                           ? 2 : 1;
        create_lane (application, zlink::transport_lane_application, hwm_);
        if (lane_count == 2)
            create_lane (completion, zlink::transport_lane_completion, 0);
        if (attach_)
            attach ();
    }

    void attach ()
    {
        TEST_ASSERT_FALSE (attached);
        for (size_t i = 0; i != 2; ++i)
            bind_pipe (cores[i], application[i]);
        if (lane_count == 2)
            for (size_t i = 0; i != 2; ++i)
                bind_pipe (cores[i], completion[i]);
        attached = true;
        pump ();
    }

    static size_t pump_owner (zlink::socket_base_t *owner_)
    {
        const bool pending =
          static_cast<zlink::mailbox_t *> (owner_->get_mailbox ())
            ->has_command_pending_hint ();
        // Use the socket's command owner so deferred route retirement runs
        // after its receive lock is released, exactly as in normal dispatch.
        TEST_ASSERT_SUCCESS_ERRNO (owner_->test_process_commands_only ());
        return pending ? 1 : 0;
    }

    void pump ()
    {
        size_t count;
        do {
            count = pump_owner (cores[0]);
            count += pump_owner (cores[1]);
        } while (count != 0);
    }

    void *sockets[2];
    zlink::socket_base_t *cores[2];
    zlink::pipe_t *application[2];
    zlink::pipe_t *completion[2];
    zlink_routing_id_t rids[2];
    uint64_t pair_id;
    uint64_t generation;
    unsigned char lane_count;
    bool attached;

  private:
    static bool paired_type (int type_)
    {
        return type_ == ZLINK_CORE_SOCKET_DEALER
               || type_ == ZLINK_CORE_SOCKET_ROUTER;
    }

    static void bind_pipe (zlink::socket_base_t *owner_, zlink::pipe_t *pipe_)
    {
        zlink::command_t command;
        memset (&command, 0, sizeof (command));
        command.destination = owner_;
        command.type = zlink::command_t::bind;
        command.args.bind.pipe = pipe_;
        TEST_ASSERT_TRUE (pipe_->retain_lifetime_ref ());
        owner_->inc_seqnum ();
        owner_->process_command (command);
    }

    void create_lane (zlink::pipe_t **pipes_, zlink::transport_lane_t lane_,
                       uint64_t hwm_)
    {
        zlink::object_t *parents[] = {cores[0], cores[1]};
        const uint64_t hwms[] = {hwm_, hwm_};
        const bool conflates[] = {false, false};
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink::pipepair (parents, pipes_, hwms, conflates, false, lane_));
        char endpoint[96];
        snprintf (endpoint, sizeof (endpoint), "inproc://unit-pair-%llu-%llu",
                  static_cast<unsigned long long> (pair_id),
                  static_cast<unsigned long long> (generation));
        uint64_t connection_id = 0;
        for (size_t i = 0; i != 2; ++i) {
            const size_t peer = 1 - i;
            pipes_[i]->set_nodelay ();
            pipes_[i]->set_transport_pair (lane_, lane_count ? pair_id : 0,
                                           lane_count ? generation : 0);
            if (lane_count)
                pipes_[i]->set_transport_lane_count (lane_count);
            pipes_[i]->set_peer_socket_type (cores[peer]->socket_type ());
            pipes_[i]->set_peer_routing_id (rids[peer].data, rids[peer].size);
            pipes_[i]->set_transport_peer_identity (rids[peer].data,
                                                     rids[peer].size);
            zlink::endpoint_uri_pair_t endpoint_pair =
              zlink::make_unconnected_bind_endpoint_pair (endpoint);
            if (i == 0)
                connection_id = endpoint_pair.connection_id.load ();
            else
                endpoint_pair.connection_id = connection_id;
            pipes_[i]->set_endpoint_pair (ZLINK_MOVE (endpoint_pair));
            // A validated connector already carries the peer RID; ROUTER
            // adopts it without a transport routing preamble.
            pipes_[i]->set_locally_initiated (true);
            const zlink::blob_t rid (rids[peer].data, rids[peer].size);
            pipes_[i]->set_router_socket_routing_id (rid);
        }
    }
};

#endif
