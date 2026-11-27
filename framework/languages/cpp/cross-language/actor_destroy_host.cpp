/* SPDX-License-Identifier: Elastic-2.0 */

#include "actor_destroy_host.hpp"
#include "location_store_observation.hpp"

#include <zlink/framework.hpp>
#include <zlink/json.hpp>
#include <zlink/locations/redis.hpp>

#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/provider_location_repository.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace actor_destroy
{
struct probe_request_t
{
    static constexpr auto packet_name = "ActorDestroyProbe";
    std::string label;
};
}

template <> struct zlink::json::dto_traits<actor_destroy::probe_request_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("label", &actor_destroy::probe_request_t::label)};
};

namespace actor_destroy
{
namespace fw = zlink::framework;
namespace runtime = fw::runtime;
using namespace std::chrono_literals;

namespace
{
constexpr auto actor_type = "cross-lang-actor-destroy-type";
constexpr auto request_timeout = 5s;

std::string sanitize (std::string_view value)
{
    std::string escaped;
    for (const auto ch : value) {
        switch (ch) {
            case '%':
                escaped += "%25";
                break;
            case '|':
                escaped += "%7C";
                break;
            case '\r':
                escaped += "%0D";
                break;
            case '\n':
                escaped += "%0A";
                break;
            default:
                escaped += ch;
                break;
        }
    }
    return escaped;
}

using time_point_t = std::chrono::system_clock::time_point;

class events_t
{
  public:
    events_t (std::string run, std::string path) : _run (std::move (run)), _path (std::move (path))
    {
        if (!std::ofstream (_path, std::ios::app))
            throw std::runtime_error ("cannot open Actor destroy events");
    }

    void emit (std::string_view event,
               std::initializer_list<std::pair<std::string_view, std::string>> fields = {},
               time_point_t time = std::chrono::system_clock::now ())
    {
        std::string line =
          "actor-destroy|run=" + _run + "|event=" + std::string (event) + "|timeUs="
          + std::to_string (
            std::chrono::duration_cast<std::chrono::microseconds> (time.time_since_epoch ())
              .count ());
        for (const auto &[key, value] : fields)
            line += "|" + std::string (key) + "=" + sanitize (value);
        const std::lock_guard lock (_mutex);
        std::ofstream output (_path, std::ios::app);
        output << line << '\n';
    }

  private:
    std::string _run;
    std::string _path;
    std::mutex _mutex;
};

const char *name_of (fw::framework_error_kind_t kind)
{
    switch (kind) {
        case fw::framework_error_kind_t::invalid_operation:
            return "InvalidOperation";
        case fw::framework_error_kind_t::unavailable:
            return "Unavailable";
        case fw::framework_error_kind_t::not_found:
            return "NotFound";
        case fw::framework_error_kind_t::deadline_exceeded:
            return "deadline";
        default:
            return "error";
    }
}

std::string key_domain (const fw::store_key_t &key)
{
    return key.value.substr (0, key.value.find ('\0'));
}

std::int64_t active_actors (const std::vector<std::byte> &bytes)
{
    const auto *data = reinterpret_cast<const char *> (bytes.data ());
    const auto value = nlohmann::json::parse (data, data + bytes.size ());
    if (value.contains ("active"))
        return value.at ("active").at ("actors").get<std::int64_t> ();
    return value.at ("actorsActive").get<std::int64_t> ();
}

struct release_evidence_t
{
    std::string version;
    bool invalid_lease = false;
    bool descriptor_condition = false;
    bool capacity_condition = false;
    bool membership_removed = false;
    std::int64_t capacity_delta = 0;
    bool relocation_policy_condition = false;

    void emit (events_t &events, time_point_t time) const
    {
        const auto boolean = [] (bool value) { return value ? "true" : "false"; };
        events.emit ("releaseCommitted",
                     {{"version", version},
                      {"invalidLease", boolean (invalid_lease)},
                      {"descriptorCondition", boolean (descriptor_condition)},
                      {"capacityCondition", boolean (capacity_condition)},
                      {"membershipRemoved", boolean (membership_removed)},
                      {"capacityDelta", std::to_string (capacity_delta)},
                      {"relocationPolicyCondition", boolean (relocation_policy_condition)}},
                     time);
    }
};

class store_t final : public fw::location_store_t
{
  public:
    store_t (std::shared_ptr<fw::location_store_t> backend,
             const std::string &actor,
             bool owner,
             std::string scenario,
             std::string peer_rid,
             events_t &events) :
        backend (std::move (backend)),
        repository (*this->backend),
        actor_key (runtime::actor_authority_key (actor)),
        owner (owner),
        scenario (std::move (scenario)),
        peer_rid (std::move (peer_rid)),
        events (events)
    {
        cross_language::location_store_observation_t observer;
        runtime::provider_location_repository_t codecs (observer);
        codecs.read_authority_core (actor_key).result ().value ();
        key = observer.observed_key;
    }

    fw::authority_read_result_t authority ()
    {
        return repository.read_authority_core (actor_key).result ().value ();
    }

    static const char *lease_kind (const fw::owner_lease_read_result_t &read,
                                   const fw::location_owner_token_t &token)
    {
        if (const auto *lease = std::get_if<fw::owner_lease_found_t> (&read)) {
            if (lease->token.lease_generation != token.lease_generation)
                return "replaced";
            return lease->lease_expires_at > lease->store_now ? "valid" : "expired";
        }
        return "missing";
    }

    fw::task_t<fw::store_read_result_t> read (fw::store_key_t requested) override
    {
        const bool observe =
          requested.value == key.value && (observe_create.load () || (scenario == "f" && !owner));
        auto result = co_await backend->read (std::move (requested));
        if (observe && std::holds_alternative<fw::store_found_t> (result)) {
            cross_language::location_store_observation_t exact (key, result);
            runtime::provider_location_repository_t codec (exact);
            const auto observed = co_await codec.read_authority_core (actor_key);
            const auto *row = std::get_if<fw::authority_snapshot_t> (&observed);
            const auto payload =
              row ? runtime::decode_direct_actor_authority_payload (row->payload) : std::nullopt;
            if (payload && payload->state == runtime::actor_authority_state_t::closing) {
                if (observe_create.exchange (false))
                    events.emit ("createObservedClosing");
                if (scenario == "f" && !owner) {
                    const auto lease = co_await repository.read_owner_lease (row->owner.owner_id);
                    if (std::string_view (lease_kind (lease, row->owner)) == "valid") {
                        if (!valid_closing.exchange (true))
                            events.emit ("callerObservedClosingWithValidLease");
                    } else if (!invalid_closing.exchange (true)) {
                        events.emit ("ownerLeaseEnded");
                        events.emit ("callerObservedClosingForRelease");
                    }
                }
            }
        }
        co_return result;
    }

    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        return backend->scan (std::move (request));
    }

    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        bool authority_put = false;
        bool authority_delete = false;
        const auto suspended_lease = blocked_lease.load ();
        for (const auto &mutation : request.mutations) {
            if (const auto *put = std::get_if<fw::store_put_t> (&mutation)) {
                if (suspended_lease && put->key.value == suspended_lease->value)
                    co_return fw::store_write_conflict_t{};
                if (put->key.value == key.value)
                    authority_put = true;
            }
            const auto *remove = std::get_if<fw::store_delete_t> (&mutation);
            if (remove && remove->key.value == key.value)
                authority_delete = true;
        }

        // The owner arms this after the initial Actor's Entry handoff has completed.
        if (authority_put && hold_cas.exchange (false)) {
            events.emit ("closingCasHeld");
            co_await cas_gate.task ();
            events.emit ("closingCasFailed");
            throw fw::framework_exception_t (fw::framework_error_kind_t::data_lost,
                                             "injectedClosingCasFailure");
        }
        if (authority_delete && hold_delete.exchange (false)) {
            events.emit ("ownerDeleteHeld");
            co_await delete_gate.task ();
        }

        std::optional<fw::authority_snapshot_t> candidate;
        if (authority_put)
            candidate = co_await inspect_put (request);
        std::optional<release_evidence_t> release;
        if (authority_delete && !owner && (scenario == "d" || scenario == "f"))
            release = co_await inspect_release (request);

        auto result = co_await backend->write (std::move (request));
        if (const auto *applied = std::get_if<fw::store_write_applied_t> (&result)) {
            if (authority_put || authority_delete)
                events.emit ("authorityWritten", {}, applied->store_now);
            if (authority_delete && owner)
                events.emit ("ownerDeleteCommitted", {}, applied->store_now);
            if (release)
                release->emit (events, std::chrono::system_clock::now ());
            if (candidate) {
                const auto payload =
                  runtime::decode_direct_actor_authority_payload (candidate->payload);
                if (payload && payload->state == runtime::actor_authority_state_t::closing)
                    events.emit ("closingCommitted", {}, applied->store_now);
                if (payload && payload->state == runtime::actor_authority_state_t::ready && !owner)
                    events.emit ("newReadyCommitted", {}, applied->store_now);
            }
        }
        co_return result;
    }

    void end_lease (const fw::authority_snapshot_t &row)
    {
        cross_language::location_store_observation_t observer;
        runtime::provider_location_repository_t codecs (observer);
        codecs.read_owner_lease (row.owner.owner_id).result ().value ();
        blocked_lease.store (std::make_shared<const fw::store_key_t> (observer.observed_key));
        const auto released = repository.release_owner_lease (row.owner).result ().value ();
        if (!std::holds_alternative<fw::owner_lease_released_t> (released))
            throw std::runtime_error ("could not end the original owner lease");
    }

    std::shared_ptr<fw::location_store_t> backend;
    runtime::provider_location_repository_t repository;
    fw::authority_key_t actor_key;
    std::atomic_bool hold_cas{false}, hold_delete{false}, observe_create{false};
    fw::task_completion_source_t<void> cas_gate, delete_gate;

  private:
    fw::task_t<std::optional<fw::authority_snapshot_t>>
    inspect_put (const fw::store_write_request_t &request)
    {
        cross_language::location_store_observation_t observed (nullptr, &request);
        runtime::provider_location_repository_t codecs (observed);
        const auto result = co_await codecs.read_authority_core (actor_key);
        if (const auto *row = std::get_if<fw::authority_snapshot_t> (&result))
            co_return *row;
        co_return std::nullopt;
    }

    fw::task_t<std::optional<release_evidence_t>>
    inspect_release (const fw::store_write_request_t &request)
    {
        const auto observed = co_await repository.read_authority_core (actor_key);
        const auto *row = std::get_if<fw::authority_snapshot_t> (&observed);
        if (!row || row->allocation.target.node_rid.value () != peer_rid)
            co_return std::nullopt;
        const auto payload = runtime::decode_direct_actor_authority_payload (row->payload).value ();
        const auto lease = co_await repository.read_owner_lease (row->owner.owner_id);

        std::string version;
        bool lease_condition = false;
        int descriptor_conditions = 0;
        std::optional<fw::store_key_t> capacity;
        for (const auto &condition : request.conditions) {
            const fw::store_key_t *condition_key = nullptr;
            if (const auto *exact = std::get_if<fw::store_version_condition_t> (&condition)) {
                condition_key = &exact->key;
                if (exact->key.value == key.value)
                    version = exact->expected.value;
            } else if (const auto *missing =
                         std::get_if<fw::store_missing_condition_t> (&condition)) {
                condition_key = &missing->key;
            }
            if (!condition_key)
                continue;
            const auto domain = key_domain (*condition_key);
            lease_condition |= domain == "owner-lease";
            if (domain == "mesh-node")
                ++descriptor_conditions;
            if (domain.starts_with ("zlink:v11:capacity:"))
                capacity = *condition_key;
        }
        std::optional<std::int64_t> delta;
        if (capacity) {
            const auto before = co_await backend->read (*capacity);
            if (const auto *found = std::get_if<fw::store_found_t> (&before))
                for (const auto &mutation : request.mutations) {
                    const auto *put = std::get_if<fw::store_put_t> (&mutation);
                    if (put && put->key.value == capacity->value)
                        delta = active_actors (put->bytes) - active_actors (found->value.bytes);
                }
        }
        // Entry membership is encoded in the Actor authority row itself.
        co_return release_evidence_t{
          .version = std::move (version),
          .invalid_lease =
            lease_condition && std::string_view (lease_kind (lease, row->owner)) != "valid",
          .descriptor_condition = descriptor_conditions >= 1,
          .capacity_condition = capacity.has_value (),
          .membership_removed =
            payload.current_spot_kind == runtime::actor_authority_spot_kind_t::entry,
          .capacity_delta = delta.value_or (0),
          .relocation_policy_condition = descriptor_conditions > 1};
    }

    bool owner;
    std::string scenario;
    std::string peer_rid;
    events_t &events;
    fw::store_key_t key;
    std::atomic_bool valid_closing{false}, invalid_closing{false};
    std::atomic<std::shared_ptr<const fw::store_key_t>> blocked_lease;
};

class actor_t final : public fw::actor_t
{
  public:
    actor_t (fw::actor_context_t context, std::shared_ptr<events_t> events) :
        _context (std::move (context)), _events (std::move (events))
    {
    }
    ~actor_t () override { _events->emit ("cleanup"); }
    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }
    events_t &events () { return *_events; }

  private:
    fw::actor_context_t _context;
    std::shared_ptr<events_t> _events;
};

class factory_t final : public fw::actor_factory_t<actor_t>
{
  public:
    factory_t (bool owner, std::shared_ptr<events_t> events) :
        _owner (owner), _events (std::move (events))
    {
    }

    fw::task_t<std::shared_ptr<actor_t>> create (fw::actor_context_t context,
                                                 std::stop_token) override
    {
        if (!_owner)
            _events->emit ("newFactoryStarted");
        co_return std::make_shared<actor_t> (std::move (context), _events);
    }

  private:
    bool _owner;
    std::shared_ptr<events_t> _events;
};

class entry_t final : public fw::entry_spot_t<actor_t>
{
  public:
    explicit entry_t (fw::entry_spot_context_t context) : _context (std::move (context)) {}
    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }

    void configure () override { _context.handlers ().add_actor_request<&entry_t::probe> (); }

    fw::task_t<void> on_actor_joined (actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (actor_t &) override { co_return; }

    std::string probe (actor_t &actor, fw::message_context_t &, const probe_request_t &request)
    {
        const auto &label = request.label;
        actor.events ().emit ("probeHandled", {{"label", label}});
        if (label == "retry")
            actor.events ().emit ("retryProbeHandled");
        return std::to_string (actor.context ().actor_ref ().object_generation ());
    }

  private:
    fw::entry_spot_context_t _context;
};

const char *destroy_event (const std::string &label)
{
    if (label == "duplicate")
        return "ownerDuplicateReturned";
    if (label == "fail")
        return "failedDestroyReturned";
    return "destroyReturned";
}

class worker_t final : public fw::hosted_service_t
{
  public:
    worker_t (std::map<std::string, std::string> args,
              std::shared_ptr<store_t> store,
              std::shared_ptr<events_t> events) :
        _args (std::move (args)), _store (std::move (store)), _events (std::move (events))
    {
    }

    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        _worker = std::jthread ([this, &services] (std::stop_token stop) {
            try {
                run (services, stop);
            }
            catch (const std::exception &error) {
                _events->emit ("error", {{"message", error.what ()}});
                release_store_gates ();
            }
        });
        co_return;
    }

    void stop () noexcept override
    {
        _worker.request_stop ();
        release_store_gates ();
        if (_worker.joinable ())
            _worker.join ();
    }

  private:
    void release_store_gates ()
    {
        _store->cas_gate.complete (fw::result_t<void>::success ());
        _store->delete_gate.complete (fw::result_t<void>::success ());
    }

    void run (fw::service_provider_t &services, std::stop_token stop)
    {
#ifdef _WIN32
        const auto pid = _getpid ();
#else
        const auto pid = getpid ();
#endif
        auto &manager = services.get_required<fw::actor_manager_t> ();
        auto &actors = services.get_required<fw::actor_client_t> ();
        const auto &mesh = _args.at ("mesh-name");
        const fw::actor_id_t actor (_args.at ("actor-id"));
        const auto &prefix = _args.at ("actor-destroy-control-prefix");
        const auto &run = _args.at ("actor-destroy-run-id");

        const auto rid = zlink::routing_id_t::from (_args.at ("node-rid"));
        auto &mesh_runtime = services.get_required<fw::route_mesh_runtime_t> ();
        wait_placement (stop, mesh, rid, 100);
        _events->emit ("hostReady", {{"pid", std::to_string (pid)}});
        std::ofstream (_args.at ("ready-file")) << "ready\n";

        std::string last;
        std::uint64_t sequence = 0;
        while (!stop.stop_requested ()) {
            collect_results ();
            std::this_thread::sleep_for (5ms);
            if (!std::filesystem::exists (_args.at ("start-file")))
                continue;
            std::ifstream input (prefix + ".command");
            std::string line, command_run, seq, verb, arg;
            if (!std::getline (input, line) || line == last)
                continue;
            last = line;
            std::istringstream fields (line);
            std::getline (fields, command_run, '\t');
            std::getline (fields, seq, '\t');
            std::getline (fields, verb, '\t');
            std::getline (fields, arg, '\t');
            if (command_run != run || seq != std::to_string (++sequence))
                throw std::runtime_error ("unexpected Actor destroy command sequence");

            if (verb == "placement") {
                const auto weight = std::stoi (arg);
                services.get_required<fw::route_mesh_runtime_options_t> ()
                  .mesh (mesh)
                  .placement_weight (weight);
                wait_placement (stop, mesh, rid, weight);
            } else if (verb == "peer-placement") {
                const auto weight = std::stoi (arg);
                const auto peer = zlink::routing_id_t::from (_args.at ("peer-rid")).to_hex ();
                auto &reader = services.get_required<runtime::live_location_reader_t> ();
                wait_until (
                  stop,
                  [&] {
                      const auto seen = reader.mesh_observation (mesh);
                      return seen && std::ranges::any_of (seen->sources, [&] (const auto &row) {
                                 return row.available () && row.descriptor.rid.to_hex () == peer
                                        && row.descriptor.placement_weight == weight;
                             });
                  },
                  "peer placement observation");
            } else if (verb == "peer") {
                const auto peer = zlink::routing_id_t::from (_args.at ("peer-rid"));
                wait_until (
                  stop,
                  [&] {
                      const auto snapshot = mesh_runtime.snapshot (mesh);
                      return std::ranges::any_of (snapshot.peers, [&] (const auto &row) {
                          return row.node_rid.to_hex () == peer.to_hex ()
                                 && row.state == fw::peer_state_t::ready;
                      });
                  },
                  "peer admission");
            } else if (verb == "create") {
                const auto result = manager.get_or_create (actor, actor_type)
                                      .in_mesh (mesh)
                                      .async ()
                                      .result ()
                                      .value ();
                _old = std::get<fw::actor_create_created_t> (result).actor;
                probe (actors, actor, "initial");
                _events->emit ("created",
                               {{"generation", std::to_string (_old->object_generation ())}});
            } else if (verb == "find") {
                const auto value = manager.find (actor).result ().value ();
                if (value && !_old)
                    _old = value;
                _events->emit (
                  "found",
                  {{"label", arg},
                   {"generation", value ? std::to_string (value->object_generation ()) : "none"}});
            } else if (verb == "authority") {
                authority (arg);
            } else if (verb == "hold-delete") {
                _store->hold_delete.store (true);
            } else if (verb == "resume-delete") {
                _events->emit ("ownerDeleteResumed");
                _store->delete_gate.complete (fw::result_t<void>::success ());
            } else if (verb == "hold-cas") {
                _store->hold_cas.store (true);
            } else if (verb == "fail-cas") {
                _store->cas_gate.complete (fw::result_t<void>::success ());
            } else if (verb == "destroy") {
                const auto started = std::chrono::system_clock::now ();
                _destroy.emplace (arg, manager.destroy (_old.value ()));
                if (arg == "admission")
                    _events->emit ("destroyDeadline", {}, started + request_timeout);
            } else if (verb == "pending-destroy") {
                const auto pending = _destroy.find (arg);
                if (pending == _destroy.end () || pending->second.await_ready ())
                    throw std::runtime_error ("Destroy completed while Delete was held");
                if (arg == "duplicate")
                    _events->emit ("ownerDuplicatePending");
            } else if (verb == "get-or-create") {
                _store->observe_create.store (true);
                _create = manager.get_or_create (actor, actor_type).in_mesh (mesh).async ();
            } else if (verb == "pending-create") {
                if (!_create || _create->await_ready ())
                    throw std::runtime_error ("GetOrCreate completed while Delete was held");
                _events->emit ("createPending");
            } else if (verb == "probe") {
                probe (actors, actor, arg);
            } else if (verb == "lease") {
                const auto lease = read_lease ();
                _events->emit (
                  "lease",
                  {{"label", arg}, {"kind", store_t::lease_kind (lease, _old_owner.value ())}});
            } else if (verb == "wait-lease-expired") {
                wait_until (
                  stop,
                  [&] {
                      const auto lease = read_lease ();
                      return std::string_view (store_t::lease_kind (lease, *_old_owner))
                               == "expired"
                             || (std::holds_alternative<fw::owner_lease_missing_t> (lease)
                                 && _original_expiry
                                 && observed_store_time () >= *_original_expiry);
                  },
                  "original owner lease expiration");
                _events->emit ("ownerLeaseExpired");
            } else if (verb == "end-lease") {
                _store->end_lease (std::get<fw::authority_snapshot_t> (_store->authority ()));
            } else if (verb == "allow-admission") {
                std::ofstream (prefix + ".admission") << "allow\n";
            } else if (verb == "finish") {
                if (!_destroy.empty () || _create)
                    throw std::runtime_error ("Actor destroy API still pending at finish");
            } else {
                throw std::runtime_error ("unexpected Actor destroy command " + verb);
            }
            _events->emit ("ack", {{"seq", seq}});
        }
    }

    template <typename Predicate>
    void wait_until (std::stop_token stop, Predicate predicate, const char *description)
    {
        const auto deadline = std::chrono::steady_clock::now () + 60s;
        while (!stop.stop_requested () && std::chrono::steady_clock::now () < deadline) {
            if (predicate ())
                return;
            std::this_thread::sleep_for (20ms);
        }
        throw std::runtime_error (std::string ("could not observe ") + description);
    }

    void wait_placement (std::stop_token stop,
                         const std::string &mesh,
                         const zlink::routing_id_t &rid,
                         int weight)
    {
        const auto local = rid.to_hex ();
        wait_until (
          stop,
          [&] {
              const auto rows = _store->repository.list_mesh_nodes (mesh).result ().value ();
              return std::ranges::any_of (rows.items, [&] (const auto &row) {
                  return row.rid.to_hex () == local && row.entry_spot_id
                         && row.placement_weight == weight
                         && row.state == fw::framework_runtime_state_t::serving;
              });
          },
          "Entry Spot and placement publication");
    }

    void authority (const std::string &label)
    {
        const auto observed = _store->authority ();
        if (const auto *row = std::get_if<fw::authority_snapshot_t> (&observed)) {
            const auto payload =
              runtime::decode_direct_actor_authority_payload (row->payload).value ();
            const auto canonical =
              runtime::decode_canonical_authority_payload (row->payload).value ();
            if (row->allocation.state != fw::placement_allocation_state_t::active
                || row->allocation.object_kind != fw::placement_object_kind_t::actor
                || row->allocation.stable_type != payload.stable_type || row->pending_creation
                || row->aggregate || payload.has_relocation_state
                || row->allocation.target.node_rid.value () != payload.node_rid.value ()
                || row->allocation.target.node_lifecycle_generation != payload.node_generation
                || row->owner.owner_id != payload.owner_id
                || static_cast<std::uint64_t> (row->owner.lease_generation)
                     != payload.owner_lease_generation)
                throw std::runtime_error ("Actor authority allocation and payload disagree");
            _events->emit (
              "authority",
              {{"label", label},
               {"read", "Found"},
               {"version", row->store_version},
               {"generation", std::to_string (row->object_generation)},
               {"state", std::to_string (static_cast<int> (payload.state))},
               {"operationKind", std::to_string (std::to_integer<int> (canonical.body.front ()))},
               {"owner", std::string (payload.node_rid.value ())},
               {"ownerId", row->owner.owner_id},
               {"ownerGeneration", std::to_string (row->authority_owner_generation)},
               {"leaseGeneration", std::to_string (row->owner.lease_generation)},
               {"nodeGeneration", std::to_string (payload.node_generation)},
               {"actorId", payload.actor_id},
               {"actorType", payload.stable_type},
               {"spot", payload.current_spot_id},
               {"spotKind", payload.current_spot_kind == runtime::actor_authority_spot_kind_t::entry
                              ? "entry"
                              : "user"}});
        } else {
            _events->emit ("authority", {{"label", label}, {"read", "Missing"}});
        }
    }

    void probe (fw::actor_client_t &actors, const fw::actor_id_t &actor, const std::string &label)
    {
        const auto result =
          actors.request (actor, probe_request_t{label}).async<std::string> ().result ();
        if (label == "initial" || (!result && !result.error ()))
            (void) result.value ();
        if (result)
            _events->emit ("probeReturned",
                           {{"label", label}, {"kind", "return"}, {"generation", result.value ()}});
        else
            _events->emit (
              "probeReturned",
              {{"label", label},
               {"kind", name_of (result.error_kind ())},
               {"retryable",
                result.error_kind () == fw::framework_error_kind_t::unavailable ? "true" : "false"},
               {"message", result.error ()->what ()}});
        if (label == "pending-cas")
            _events->emit ("pendingCasProbeReturned");
        if (label == "post-commit")
            _events->emit ("postCommitProbeReturned");
    }

    fw::owner_lease_read_result_t read_lease ()
    {
        if (!_old_owner)
            _old_owner = std::get<fw::authority_snapshot_t> (_store->authority ()).owner;
        auto current =
          _store->repository.read_owner_lease (_old_owner->owner_id).result ().value ();
        if (const auto *found = std::get_if<fw::owner_lease_found_t> (&current);
            found && found->token.lease_generation == _old_owner->lease_generation)
            _original_expiry = found->lease_expires_at;
        return current;
    }

    time_point_t observed_store_time ()
    {
        return std::get<fw::authority_snapshot_t> (_store->authority ()).store_now;
    }

    void collect_results ()
    {
        for (auto it = _destroy.begin (); it != _destroy.end ();) {
            if (!it->second.await_ready ()) {
                ++it;
                continue;
            }
            const auto result = it->second.result ();
            const auto &label = it->first;
            if (result) {
                _events->emit (destroy_event (label),
                               {{"label", label}, {"kind", result.value () ? "true" : "false"}});
            } else {
                if (!result.error ())
                    (void) result.value ();
                const std::string message = result.error ()->what ();
                _events->emit (destroy_event (label), {{"label", label},
                                                       {"kind", name_of (result.error_kind ())},
                                                       {"message", message}});
                if (label == "admission" && message == "Actor owner is not RouteMesh-admitted")
                    _events->emit ("submissionRejected");
            }
            it = _destroy.erase (it);
        }
        if (_create && _create->await_ready ()) {
            const auto result = _create->result ().value ();
            const auto *created = std::get_if<fw::actor_create_created_t> (&result);
            const auto *existing = std::get_if<fw::actor_create_existing_t> (&result);
            _events->emit (
              "getOrCreateReturned",
              {{"kind", created    ? "Created"
                        : existing ? "Existing"
                                   : "error"},
               {"generation", created    ? std::to_string (created->actor.object_generation ())
                              : existing ? std::to_string (existing->actor.object_generation ())
                                         : "none"}});
            _create.reset ();
        }
    }

    std::map<std::string, std::string> _args;
    std::shared_ptr<store_t> _store;
    std::shared_ptr<events_t> _events;
    std::optional<fw::actor_ref_t> _old;
    std::optional<fw::location_owner_token_t> _old_owner;
    std::optional<time_point_t> _original_expiry;
    std::map<std::string, fw::task_t<bool>> _destroy;
    std::optional<fw::task_t<fw::actor_create_result_t>> _create;
    std::jthread _worker;
};
} // namespace

int run_host (int argc, char **argv, bool owner, const std::map<std::string, std::string> &args)
{
    const auto &prefix = args.at ("redis-key-prefix");
    const auto &endpoint = args.at ("redis-endpoint");
    const auto events =
      std::make_shared<events_t> (args.at ("actor-destroy-run-id"), args.at ("event-file"));
    const auto redis =
      std::make_shared<fw::redis::redis_location_store_t> (fw::redis::redis_location_options_t{
        .connection_string = endpoint, .key_prefix = prefix + ":location"});
    const auto store =
      std::make_shared<store_t> (redis, args.at ("actor-id"), owner,
                                 args.at ("actor-destroy-scenario"), args.at ("peer-rid"), *events);

    auto app = fw::app_t::create ();
    app.logging ().use_file (args.at ("event-file") + ".flow");
    app.add_zlink_framework ([&] (fw::zlink_framework_options_t &options) {
        options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::normal);
        options.add_location_store (store);
        options.add_relocation_store (std::make_shared<fw::redis::redis_relocation_store_t> (
          fw::redis::redis_relocation_options_t{.connection_string = endpoint,
                                                .key_prefix = prefix + ":relocation"}));
        auto mesh = options.add_route_mesh (args.at ("mesh-name"));
        mesh.listen (args.at ("bind-endpoint"))
          .set_advertise_host (args.at ("advertise-host"))
          .set_routing_id (zlink::routing_id_t::from (args.at ("node-rid")))
          .set_default_request_timeout (request_timeout)
          .set_placement_weight (100);
        mesh.channel_name (args.at ("mesh-name")).server ();
        mesh.objects ().server ();
        mesh.add_entry_spot<entry_t> ([] (fw::entry_spot_context_t context) {
            return std::make_shared<entry_t> (std::move (context));
        });
        mesh.add_actor_factory<actor_t, factory_t> (
          actor_type, std::make_shared<factory_t> (owner, events),
          [] (auto &factory) { factory.recreate_on_relocation (); });
    });
    app.add_hosted_service (std::make_unique<worker_t> (args, store, events));
    return app.run (argc, argv);
}
} // namespace actor_destroy
