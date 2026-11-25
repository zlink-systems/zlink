/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6b_stateful_fixture.hpp"
#include <cassert>

namespace zlink::framework::test::m6b
{
stateful::object_ref_t create_ready (stateful::stateful_object_runtime_t &runtime,
                                     stateful::create_request_t request)
{
    const auto reserved = runtime.begin_create (request);
    assert (reserved.status == stateful::create_status_t::reserved);
    assert (reserved.factory_owner);
    assert (runtime.commit_create (reserved.attempt) == stateful::stateful_error_t::none);
    const auto ready = runtime.find (request.kind, request.key);
    assert (ready);
    return *ready;
}
}
