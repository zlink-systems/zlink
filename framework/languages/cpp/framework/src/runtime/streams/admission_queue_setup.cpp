/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/streams/admission_queue_setup.hpp"

#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/admission_queue_state.hpp"

#include <limits>

namespace zlink::framework::runtime::admission_queue
{
namespace
{

[[noreturn]] void configuration_error (const char *message)
{
    throw framework_exception_t (framework_error_kind_t::not_configured, message);
}

task_t<std::set<owner_t>> ended_owners (live_location_reader_t &locations, std::set<owner_t> owners)
{
    std::set<owner_t> ended;
    for (const auto &owner : owners) {
        location_owner_token_t token{owner.id, static_cast<std::int64_t> (owner.generation)};
        if (!co_await locations.owner_available (std::move (token)))
            ended.insert (owner);
    }
    co_return ended;
}

class queue_spot_t final : public instance_spot_t
{
  public:
    queue_spot_t (instance_spot_context_t context,
                  const settings_t &settings,
                  live_location_reader_t &locations) :
        _context (std::move (context)),
        _queue (settings.tickets, settings.global_limit),
        _locations (locations)
    {
    }

    instance_spot_context_t &context () noexcept override { return _context; }
    const instance_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ()
          .add_handler<&queue_spot_t::enqueue> (enqueue_packet)
          .add_handler<&queue_spot_t::report> (report_packet)
          .add_handler<&queue_spot_t::admit> (admit_packet)
          .add_handler<&queue_spot_t::release> (release_packet);
    }

    json_t enqueue (const json_t &request)
    {
        return write_admission_message (
          _queue.enqueue (read_enqueue (request), queue_time_t::now ()));
    }

    task_t<json_t> report (const json_t &request)
    {
        auto report = read_report (request);
        const auto now = queue_time_t::now ();
        auto owners = _queue.accept_report (report, now);
        if (owners.empty ())
            return task_t<json_t> (result_t<json_t>::success (
              write_admission_message (_queue.finish_report (report.node, now))));
        return report_after_lease_check (std::move (report.node), std::move (owners));
    }

    task_t<json_t> admit (const json_t &request)
    {
        auto permit = read_admit (request);
        const auto now = queue_time_t::now ();
        auto owners = _queue.permit_owners_at_limit (permit, now);
        if (owners.empty ())
            return task_t<json_t> (
              result_t<json_t>::success (write_admit_reply (_queue.admit (permit, now))));
        return admit_after_lease_check (std::move (permit), std::move (owners));
    }

    void release (const json_t &message) { _queue.release (read_release (message)); }

  private:
    task_t<json_t> report_after_lease_check (node_t node, std::set<owner_t> owners)
    {
        const auto ended = co_await check_owners (std::move (owners));
        co_return write_admission_message (
          _queue.finish_report (node, queue_time_t::now (), ended));
    }

    task_t<json_t> admit_after_lease_check (admit_request_t permit, std::set<owner_t> owners)
    {
        const auto ended = co_await check_owners (std::move (owners));
        for (const auto &owner : ended)
            _queue.owner_lease_ended (owner);
        co_return write_admit_reply (_queue.admit (permit, queue_time_t::now ()));
    }

    task_t<std::set<owner_t>> check_owners (std::set<owner_t> owners)
    {
        auto worker = _context.run_io_worker (
          [&locations = _locations, owners = std::move (owners)] () mutable {
              return ended_owners (locations, std::move (owners));
          });
        co_return co_await worker.yield ();
    }

    instance_spot_context_t _context;
    queue_state_t _queue;
    live_location_reader_t &_locations;
};

} // namespace

std::optional<settings_t>
configure (const std::vector<stream_snapshot_t> &streams,
           const std::vector<std::shared_ptr<detail::mesh_node_builder_state_t>> &meshes,
           live_location_reader_t *locations)
{
    if (streams.empty ())
        return {};
    const auto &first = streams.front ().authentication;
    for (const auto &stream : streams) {
        const auto &settings = stream.authentication;
        if (settings.queue_enabled != first.queue_enabled
            || (first.queue_enabled
                && (settings.queue_key != first.queue_key
                    || settings.queue_previous_key != first.queue_previous_key
                    || settings.global_limit != first.global_limit
                    || settings.queue_mesh_name != first.queue_mesh_name)))
            configuration_error ("STREAM admission queue settings must agree on every listener");
        if (first.queue_enabled && !valid_stream_name (stream.name))
            configuration_error ("STREAM admission queue listener name must be 1..255 UTF-8 bytes");
    }
    if (!first.queue_enabled)
        return {};
    if (!first.packet_name || !first.queue_key)
        configuration_error (
          "STREAM admission queue requires an authentication packet name and key");
    if (first.global_limit
        && (*first.global_limit < 1
            || *first.global_limit > std::numeric_limits<std::int32_t>::max ()))
        configuration_error ("STREAM admission global limit must be in 1..2147483647");
    if (!first.queue_mesh_name && meshes.size () != 1)
        configuration_error ("Mesh selection is required");
    const auto mesh = std::ranges::find_if (meshes, [&] (const auto &candidate) {
        return !first.queue_mesh_name || candidate->mesh_name == *first.queue_mesh_name;
    });
    if (mesh == meshes.end ())
        configuration_error ("STREAM admission queue mesh is not registered");
    if (!locations || (*mesh)->object_role == object_role_t::none)
        configuration_error (
          "STREAM admission queue requires an Object MeshNode and Location Store");
    settings_t settings{(*mesh)->mesh_name, {}, first.global_limit};
    if (!detail::valid_spot_id (spot_prefix + settings.mesh_name))
        configuration_error ("STREAM admission queue SpotId exceeds the Instance Spot ID limit");
    try {
        settings.tickets =
          std::make_shared<const ticket_signer_t> (*first.queue_key, first.queue_previous_key);
    }
    catch (const std::invalid_argument &) {
        configuration_error ("Invalid STREAM admission queue key");
    }
    if ((*mesh)->object_role == object_role_t::server) {
        (*mesh)->spot_builder.add_instance_spot_factory<queue_spot_t> (
          spot_type,
          [settings, locations] (instance_spot_context_t context) {
              return std::make_shared<queue_spot_t> (std::move (context), settings, *locations);
          },
          [] (auto &factory) { factory.recreate_on_relocation (); });
    }
    return settings;
}

} // namespace zlink::framework::runtime::admission_queue
