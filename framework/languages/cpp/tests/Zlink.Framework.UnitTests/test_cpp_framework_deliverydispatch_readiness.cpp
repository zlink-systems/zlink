/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../../samples/DeliveryDispatch/Server/Configuration/sample_readiness.hpp"

#include <array>
#include <barrier>
#include <map>
#include <mutex>
#include <sstream>

namespace
{
using namespace zlink::framework;
using namespace zlink::samples::deliverydispatch;

class observed_mesh_t final : public route_mesh_runtime_t
{
  public:
    mesh_node_snapshot_t snapshot (std::string) const override { return {}; }
    bool is_ready (std::string) const override { return false; }
    std::unique_ptr<mesh_runtime_observation_t> observe (
      std::string, std::size_t,
      std::function<void (const observed_status_t<mesh_node_snapshot_t> &)> observer) override
    {
        observers.push_back (std::move (observer));
        return nullptr;
    }

    std::vector<std::function<void (const observed_status_t<mesh_node_snapshot_t> &)>> observers;
};

// Preserve each stream insertion, while allowing competing writers between
// insertions as the redirected stdout used by the sample runner does.
class captured_output_t final : public std::streambuf
{
  public:
    captured_output_t () : _previous (std::cout.rdbuf (this)) {}
    ~captured_output_t () override { std::cout.rdbuf (_previous); }
    std::string text () const { return _text; }

  protected:
    std::streamsize xsputn (const char *data, std::streamsize size) override
    {
        {
            std::lock_guard lock (_mutex);
            _text.append (data, static_cast<std::size_t> (size));
        }
        std::this_thread::yield ();
        return size;
    }
    int_type overflow (int_type ch) override
    {
        if (!traits_type::eq_int_type (ch, traits_type::eof ())) {
            const auto value = traits_type::to_char_type (ch);
            xsputn (&value, 1);
        }
        return traits_type::not_eof (ch);
    }

  private:
    std::streambuf *_previous;
    std::mutex _mutex;
    std::string _text;
};
} // namespace

int main ()
{
    for (int round = 0; round < 32; ++round) {
        service_collection_t registrations;
        auto mesh = std::make_shared<observed_mesh_t> ();
        auto &runtime = *mesh;
        registrations.add_factory<route_mesh_runtime_t> (
          [mesh] (service_provider_t &) -> std::shared_ptr<route_mesh_runtime_t> {
              return mesh;
          }, service_lifetime_t::singleton);
        auto services = registrations.build_provider ();
        route_readiness_service_t route ("dispatch", "delivery-couriers");
        actor_route_readiness_service_t first (
          "delivery-couriers", "courier-node-1", "courier-node-1");
        actor_route_readiness_service_t second (
          "delivery-couriers", "courier-node-2", "courier-node-2");
        std::string output;
        bool premature_evidence = false;
        {
            captured_output_t capture;
            route.start (services).await_resume ();
            first.start (services).await_resume ();
            second.start (services).await_resume ();
            for (const auto &observer : runtime.observers)
                observer ({});
            premature_evidence = !capture.text ().empty ();

            observed_status_t<mesh_node_snapshot_t> ready;
            ready.status.is_ready = true;
            ready.status.peers = {
              {.node_rid = zlink::routing_id_t::from ("courier-node-1"),
               .state = peer_state_t::ready},
              {.node_rid = zlink::routing_id_t::from ("courier-node-2"),
               .state = peer_state_t::ready}};
            std::barrier start (3);
            std::array<std::thread, 3> writers;
            for (std::size_t index = 0; index < writers.size (); ++index) {
                writers[index] = std::thread ([&, index] {
                    start.arrive_and_wait ();
                    runtime.observers[index] (ready);
                    runtime.observers[index] (ready);
                });
            }
            for (auto &writer : writers)
                writer.join ();
            route.stop ();
            first.stop ();
            second.stop ();
            output = capture.text ();
        }
        std::map<std::string, int> actual;
        std::istringstream lines (output);
        for (std::string line; std::getline (lines, line);)
            ++actual[line];
        const std::map<std::string, int> expected{
          {"deliverydispatch-ready kind=route node=dispatch", 1},
          {"deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-1", 1},
          {"deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-2", 1}};
        if (premature_evidence || actual != expected) {
            std::cerr << "Readiness evidence must contain each complete line exactly once:\n"
                      << output;
            return 1;
        }
    }
    return 0;
}
