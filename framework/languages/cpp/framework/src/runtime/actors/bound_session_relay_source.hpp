/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/Contracts/Core/routing_id.hpp>

#include <cstdint>
#include <memory>

namespace zlink::framework::detail
{

class stream_reply_submission_t;

struct bound_session_relay_source_t
{
    zlink::routing_id_t session_rid;
    std::uint64_t binding_generation = 0;
    std::uint64_t session_sequence = 0;
    std::shared_ptr<stream_reply_submission_t> reply_submission;
};

} // namespace zlink::framework::detail
