# C++ Location · Relocation Store · Redis Exact Interface

[C++ exact interface table of contents](README.en.md) ·
[Location Runtime](../../../../21-location-runtime.en.md) ·
[Redis Location Store](../../../../22-location-store-redis.en.md)

This document fixes the minimal public SPI an external provider
implements, the location option and operational query an application
uses, and the public declaration of the official Redis extension.
Authority, owner lease, reservation, capacity, aggregate, and the
relocation state machine are encoded by the Framework as opaque
records. The provider doesn't know that domain type or processing
stage.

The Store primitive and abstract Store class are owned by the opt-in
CMake target `zlink::framework_provider_abstractions`. A provider
implementation can implement the Store by linking only this target,
without depending on an Actor/Spot application target. The C++
namespace keeps the existing `zlink::framework`.

## 1. Root Option And Registration

```cpp
namespace zlink::framework {

struct location_options_t {
    std::chrono::milliseconds owner_lease_renew_interval{5000};
    std::chrono::milliseconds owner_lease_ttl{15000};
    std::chrono::milliseconds polling_interval{1000};
    std::chrono::milliseconds store_failure_grace{30000};
    std::chrono::milliseconds owner_lease_fencing_margin{5000};
    std::chrono::milliseconds owner_lease_renew_timeout{3000};
    std::chrono::milliseconds route_cache_max_age{15000};
    std::chrono::milliseconds message_follow_duration{30000};
    std::size_t max_active_outbound_relocations = 64;
    std::size_t max_active_inbound_relocations = 64;
    std::size_t max_concurrent_relocation_captures = 8;
    std::size_t max_concurrent_relocation_restores = 8;
    std::uint64_t max_relocation_payload_in_flight_bytes = 268435456;
};

} // namespace zlink::framework
```

The Store registration member is owned by
[Configuration and host](02-configuration-host.en.md)'s
`zlink_framework_options_t`. The application registers each of the two
capabilities once with the existing `add_location_store(...)` and
`add_relocation_store(...)`. A `use_*` member of the same meaning, or a
Redis-specific registration helper, isn't provided.

Once registration succeeds, the Framework takes over the Store
instance's logical lifetime. After registration, the caller doesn't
call or replace the Store directly. The `std::shared_ptr` the Framework
holds is released only after ending the dependent runtime and
in-progress operations first. If two Stores share a connection,
whether the connection closes when the last Store is released, or an
external owner keeps the connection, is managed by the provider.

## 2. Location Store

```cpp
namespace zlink::framework {

struct store_key_t {
    std::string value;
};

struct store_version_t {
    std::string value;
};

struct store_scan_cursor_t {
    std::string value;
};

struct store_value_t {
    std::vector<std::byte> bytes;
    store_version_t version;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::chrono::system_clock::time_point store_now{};
};

struct store_missing_t {
    std::chrono::system_clock::time_point store_now{};
};

struct store_found_t {
    store_value_t value;
};

using store_read_result_t = std::variant<store_missing_t, store_found_t>;

struct store_missing_condition_t {
    store_key_t key;
};

struct store_version_condition_t {
    store_key_t key;
    store_version_t expected;
};

using store_condition_t =
  std::variant<store_missing_condition_t, store_version_condition_t>;

struct store_put_t {
    store_key_t key;
    std::vector<std::byte> bytes;
    std::optional<std::chrono::milliseconds> retention;
};

struct store_delete_t {
    store_key_t key;
};

using store_mutation_t = std::variant<store_put_t, store_delete_t>;

struct store_write_request_t {
    std::vector<store_condition_t> conditions;
    std::vector<store_mutation_t> mutations;
};

struct store_put_version_t {
    store_key_t key;
    store_version_t version;
};

struct store_write_applied_t {
    std::vector<store_put_version_t> put_versions;
    std::chrono::system_clock::time_point store_now{};
};

struct store_write_conflict_t {
    std::chrono::system_clock::time_point store_now{};
};

using store_write_result_t =
  std::variant<store_write_applied_t, store_write_conflict_t>;

struct store_scan_request_t {
    std::string prefix;
    std::optional<store_scan_cursor_t> cursor;
    std::uint32_t limit = 100;
};

struct store_scan_item_t {
    store_key_t key;
    store_value_t value;
};

struct store_scan_page_t {
    std::vector<store_scan_item_t> items;
    std::optional<store_scan_cursor_t> next_cursor;
    std::chrono::system_clock::time_point store_now{};
};

struct store_scan_expired_t {};
using store_scan_result_t =
  std::variant<store_scan_page_t, store_scan_expired_t>;

class location_store_t {
public:
    virtual ~location_store_t() = default;

    virtual task_t<store_read_result_t> read(store_key_t key) = 0;
    virtual task_t<store_write_result_t> write(
      store_write_request_t request) = 0;
    virtual task_t<store_scan_result_t> scan(
      store_scan_request_t request) = 0;
};

} // namespace zlink::framework
```

Key is an opaque UTF-8 `1..1024`-byte string the Framework issues,
using case-sensitive exact match. Version and cursor are opaque UTF-8
`1..4096`-byte strings the provider issues. Value is at most 1 MiB. If
`retention` is absent, it doesn't expire, and the provider clock is
used for expiry judgment. Since `store_now` is a time obtained from the
same provider observation, the Framework doesn't use the local clock
for TTL judgment. The specified `retention` must be positive.

`write(...)` first checks every condition, and only if all are true
does it apply every mutation as one atomic commit. If even one
condition is false, both mutation and version increase are 0, and it
returns `store_write_conflict_t`. Condition only provides Missing or
exact Version comparison. The conflict result doesn't carry domain
state or the current value — the Framework does an exact read of the
needed key.

One write request allows at most 2,048 unique keys combining condition
and mutation, and at most 4 MiB of encoded size. A condition or
mutation on the same key can't be duplicated.

`scan(...)` is the required operation recovery and maintenance use to
find a bounded key set. Prefix is UTF-8 `0..1024` bytes, and limit is
`1..1000`. The snapshot the first page created is fixed through the
last page. If the provider can't keep the snapshot any longer, it
returns `store_scan_expired_t`, and the Framework discards the partial
result and reads from the first page again. A page can return fewer
items than limit once it reaches 4 MiB encoded.

## 3. Relocation Store

```cpp
namespace zlink::framework {

struct blob_reference_t {
    std::string value;
};

struct blob_stored_t {
    std::chrono::system_clock::time_point expires_at{};
    std::chrono::system_clock::time_point store_now{};
};

struct blob_already_stored_t {
    std::chrono::system_clock::time_point expires_at{};
    std::chrono::system_clock::time_point store_now{};
};

struct blob_conflict_t {
    std::chrono::system_clock::time_point store_now{};
};

using blob_put_result_t =
  std::variant<blob_stored_t, blob_already_stored_t, blob_conflict_t>;

struct blob_missing_t {
    std::chrono::system_clock::time_point store_now{};
};

struct blob_found_t {
    std::vector<std::byte> bytes;
    std::chrono::system_clock::time_point expires_at{};
    std::chrono::system_clock::time_point store_now{};
};

using blob_read_result_t = std::variant<blob_missing_t, blob_found_t>;

struct blob_renewed_t {
    std::chrono::system_clock::time_point expires_at{};
    std::chrono::system_clock::time_point store_now{};
};

using blob_renew_result_t = std::variant<blob_missing_t, blob_renewed_t>;

class relocation_store_t {
public:
    virtual ~relocation_store_t() = default;

    virtual task_t<blob_put_result_t> put(
      blob_reference_t reference,
      std::span<const std::byte> payload,
      std::chrono::milliseconds retention) = 0;

    virtual task_t<blob_read_result_t> read(
      blob_reference_t reference) = 0;

    virtual task_t<blob_renew_result_t> renew(
      blob_reference_t reference,
      std::chrono::milliseconds retention) = 0;

    // an idempotent operation that succeeds even when the reference doesn't exist.
    virtual task_t<void> erase(blob_reference_t reference) = 0;
};

} // namespace zlink::framework
```

Reference is an opaque UTF-8 `1..4096`-byte string the Framework
issues before put, using exact match. A deleted or expired reference
also isn't reused for different content. Re-putting the same reference
with the same bytes returns `blob_already_stored_t`; putting different
bytes returns `blob_conflict_t`. With this rule, the Framework can
reconcile the storage result after a timeout or connection error by
doing an exact read of the same reference. `retention` must be
positive.

One blob is at most 64 MiB. The Framework composes a logical relocation
stream of at most 256 GiB using at most 4,096 chunks and an immutable
root manifest. Checksum, root/chunk relationship, participant
inventory, and relocation phase are owned by the Framework, and the
provider doesn't interpret the payload.

The input span the provider receives is valid only until the
asynchronous operation ends. Ownership of the returned byte vector
transfers to the caller.

## 4. Call Failure And Reconciliation

The C++ interface doesn't directly port a different language's
cancellation token. If host shutdown or deadline is confirmed before
the Framework starts an operation, it doesn't call the provider. If a
timeout, transport error, or process interruption occurs after a call
has started, whether the commit was applied may be uncertain. The
Framework reconciles the result with the Location Store's exact read
and version, or the Relocation Store's Framework-issued reference.

A caller error, such as an input range violation, is validated before
starting the operation. Conflict, Missing, Expired, and AlreadyStored
are normal result variants, and aren't expressed as an exception. A
provider failure converts to the runtime's provider-failure
classification, and doesn't expose internal information, such as a
Redis key or script, to the application error.

## 5. Operational Query

The application looks up an aggregate projection instead of directly
reading a stored key or private record.

```cpp
namespace zlink::framework {

enum class location_role_t : std::uint16_t {
    invalid = 0,
    spot = 2,
    router = 3,
    dealer = 4,
    pub = 5,
    sub = 6
};

struct location_page_request_t {
    int page_size = 100;
    std::optional<std::string> continuation_token;
};

template <typename T>
struct location_page_t {
    std::vector<T> items;
    std::optional<std::string> continuation_token;
};

class location_readiness_t {
public:
    virtual ~location_readiness_t() = default;
    virtual task_t<bool> is_peer_ready(
      std::string mesh_name,
      location_role_t role,
      std::optional<zlink::routing_id_t> node_rid = std::nullopt) = 0;
};

struct location_runtime_status_t {
    bool store_healthy = false;
    std::optional<std::chrono::system_clock::time_point> last_refresh_at;
    bool owner_lease_healthy = false;
    std::optional<std::chrono::system_clock::time_point> owner_lease_renewed_at;
};

enum class location_topology_state_t {
    discovered = 1,
    connecting = 2,
    ready = 3,
    lost = 4,
    error = 5,
    stopped = 6
};

struct location_topology_filter_t {
    std::optional<std::string> mesh_name;
    std::optional<zlink::routing_id_t> node_rid;
    std::optional<location_topology_state_t> state;
};

struct location_topology_entry_t {
    std::string mesh_name;
    zlink::routing_id_t node_rid;
    std::string endpoint;
    bool draining = false;
    location_topology_state_t state = location_topology_state_t::discovered;
    std::chrono::system_clock::time_point updated_at{};
};

struct location_service_summary_filter_t {
    std::optional<std::string> mesh_name;
};

struct location_service_summary_t {
    std::string mesh_name;
    std::uint32_t total_count = 0;
    std::uint32_t ready_count = 0;
    std::uint32_t error_count = 0;
    std::uint32_t stopped_count = 0;
    std::chrono::system_clock::time_point last_updated_at{};
};

class location_runtime_query_t {
public:
    virtual ~location_runtime_query_t() = default;
    virtual task_t<location_runtime_status_t> get_status() = 0;
    virtual task_t<location_page_t<location_topology_entry_t>> list_topology(
      location_topology_filter_t filter,
      location_page_request_t page = {}) = 0;
    virtual task_t<location_page_t<location_service_summary_t>>
      list_service_summaries(
        location_service_summary_filter_t filter,
        location_page_request_t page = {}) = 0;
};

} // namespace zlink::framework
```

NodeRid is a transport routing identity, so it keeps the public
`zlink::routing_id_t`. Store version, private owner token, and provider
clock aren't exposed in the operational query.

## 6. Redis Extension

```cpp
namespace zlink::framework::redis {

struct redis_location_options_t {
    std::string connection_string;
    std::string key_prefix;
    std::chrono::milliseconds operation_timeout{5000};
};

struct redis_relocation_options_t {
    std::string connection_string;
    std::string key_prefix;
    std::chrono::milliseconds operation_timeout{5000};
};

class redis_location_store_t final : public location_store_t {
public:
    explicit redis_location_store_t(redis_location_options_t options);
    ~redis_location_store_t() override;
};

class redis_relocation_store_t final : public relocation_store_t {
public:
    explicit redis_relocation_store_t(redis_relocation_options_t options);
    ~redis_relocation_store_t() override;
};

} // namespace zlink::framework::redis
```

The provider surface the official Redis package makes public is two
options and two Store implementation classes. Redis key layout, Lua
script, private record encoding, retry, and connection lease are
implementation details. The two Stores can share the same Redis
deployment, or be physically separate. Even when using the same
deployment, they use different `key_prefix`es, and don't require a
cross-store transaction.

## 7. Types Not Made Public

The following types and operations are Framework-private records or
Redis implementation details.

- Authority/owner-lease/reservation/capacity/fence/aggregate DTO
- A domain operation such as `reserve`, `commit`, `abort`,
  `prepare_aggregate`
- Relocation phase/manifest/participant DTO and a provider-generated
  relocation reference
- Raw Redis command adapter, script, and key codec
- Spot/Actor-dedicated Store and per-capability Store interface
