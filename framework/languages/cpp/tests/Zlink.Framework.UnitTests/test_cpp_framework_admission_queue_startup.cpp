/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/admission_queue_messages.hpp"
#include "runtime/streams/admission_queue_setup.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
namespace fw = zlink::framework;
namespace queue = fw::runtime::admission_queue;
using queue::json_t;

void check (bool condition, const json_t &fixture)
{
    if (!condition)
        throw std::runtime_error (fixture.at ("name").get<std::string> ());
}

void run (const json_t &fixture)
{
    const auto &given = fixture.at ("given");
    const auto &expected = fixture.at ("expect");
    fw::runtime::in_memory_location_repository_t store;
    fw::runtime::live_location_reader_t locations (store);
    std::vector<std::shared_ptr<fw::detail::mesh_node_builder_state_t>> meshes;
    for (const auto &name : given.at ("meshNodes")) {
        auto mesh = std::make_shared<fw::detail::mesh_node_builder_state_t> (name);
        mesh->object_role = fw::object_role_t::client;
        meshes.push_back (std::move (mesh));
    }
    const auto name_bytes = given.value ("streamNameUtf8Bytes", 5);
    std::string name;
    while (name.size () + 2 <= static_cast<std::size_t> (name_bytes))
        name += "\xc3\xa9";
    if (name.size () < static_cast<std::size_t> (name_bytes))
        name += 'a';
    fw::stream_snapshot_t stream{.name = std::move (name)};
    stream.authentication.packet_name = "login";
    stream.authentication.queue_key = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";
    stream.authentication.queue_enabled = given.at ("queueEnabled");
    if (!given.at ("queueMeshName").is_null ())
        stream.authentication.queue_mesh_name = given.at ("queueMeshName").get<std::string> ();
    try {
        const auto config = queue::configure ({stream}, meshes, &locations);
        check (expected.at ("started") == true, fixture);
        if (config) {
            check (config->mesh_name == expected.at ("queueMesh").get<std::string> (), fixture);
            check (queue::spot_prefix + config->mesh_name
                     == expected.at ("queueSpotId").get<std::string> (),
                   fixture);
        } else {
            check (expected.at ("queueMesh").is_null (), fixture);
        }
    }
    catch (const fw::framework_exception_t &error) {
        check (expected.at ("started") == false, fixture);
        check (error.kind () == fw::framework_error_kind_t::not_configured, fixture);
        if (expected.at ("configurationError").is_string ())
            check (expected.at ("configurationError").get<std::string> () == error.what (),
                   fixture);
    }
}
} // namespace

int main ()
{
    try {
        const auto fixture = json_t::parse (std::ifstream (ZLINK_ADMISSION_QUEUE_PATH));
        const auto &scenarios = fixture.at ("startupScenarios");
        if (scenarios.size () != 7)
            throw std::runtime_error ("Expected seven admission startup scenarios");
        for (const auto &scenario : scenarios) {
            run (scenario);
            std::cout << "PASS " << scenario.at ("name").get<std::string> () << '\n';
        }
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what () << '\n';
        return 1;
    }
}
