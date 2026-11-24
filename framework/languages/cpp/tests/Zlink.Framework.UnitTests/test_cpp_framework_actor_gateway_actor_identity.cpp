/* SPDX-License-Identifier: Elastic-2.0 */

#include <zlink/framework/contracts/actors/actor.hpp>
#include "../support/actor_gateway_cases.hpp"
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace zlink::framework::test::actor_gateway
{
int actor_identity_validation_is_bounded_and_utf8_exact ()
{
    using namespace zlink::framework;

    const actor_id_t unicode_id ("사용자");
    if (unicode_id.value () != "사용자")
        return 1;

    const auto rejects = [] (std::string value) {
        try {
            actor_id_t invalid (std::move (value));
            static_cast<void> (invalid);
        }
        catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    if (!rejects ({}) || !rejects (std::string ("\xc3\x28")) || !rejects (std::string ("\xc0\x80"))
        || !rejects (std::string ("\xed\xa0\x80")) || !rejects (std::string ("\xf4\x90\x80\x80"))
        || !rejects (std::string (256, 'a'))) {
        return 2;
    }

    const auto node = node_rid_t::from_string ("actor-node");
    const auto max_generation =
      static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ());
    const actor_ref_t maximum (actor_id_t ("actor"), max_generation, "game", node);
    if (maximum.object_generation () != max_generation)
        return 3;

    const auto rejects_generation = [&] (std::uint64_t generation) {
        try {
            actor_ref_t invalid (actor_id_t ("actor"), generation, "game", node);
            static_cast<void> (invalid);
        }
        catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    if (!rejects_generation (0) || !rejects_generation (max_generation + 1)) {
        return 4;
    }
    return 0;
}

class recording_actor_client_t final : public zlink::framework::actor_client_t
{
  public:
    std::atomic_int attempts{0};

  protected:
    zlink::framework::task_t<void>
    send_erased (zlink::framework::actor_id_t,
                 std::string,
                 zlink::framework::message_t,
                 const zlink::framework::actor_send_call_t::metadata_map_t &) override
    {
        ++attempts;
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::request_task_t<zlink::framework::message_t>
    request_erased (zlink::framework::actor_id_t,
                    std::string,
                    zlink::framework::message_t,
                    std::optional<std::chrono::milliseconds>,
                    const zlink::framework::actor_request_call_t::metadata_map_t &) override
    {
        return zlink::framework::request_task_t<zlink::framework::message_t> (
          zlink::framework::result_t<zlink::framework::message_t>::success (
            zlink::framework::message_t{}));
    }

    zlink::framework::serializer_registry_t &actor_client_serializers () override
    {
        return serializers;
    }

  private:
    zlink::framework::serializer_registry_t serializers;
};

int actor_send_is_one_shot ()
{
    using namespace zlink::framework;
    recording_actor_client_t client;
    actor_send_call_t call (client, actor_id_t ("actor-2"), "message", message_t{});
    auto copied = call;
    call.async ().result ().value ();
    bool rejected = false;
    try {
        (void) copied.async ().result ().value ();
    }
    catch (const framework_exception_t &error) {
        rejected = error.kind () == framework_error_kind_t::protocol_error;
    }
    return rejected && client.attempts.load () == 1 ? 0 : 2;
}
} // namespace zlink::framework::test::actor_gateway
