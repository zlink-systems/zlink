# C++ Location·Relocation Store·Redis exact interface

[C++ exact interface 목차](README.ko.md) ·
[Location Runtime](../../../../21-location-runtime.ko.md) ·
[Redis Location Store](../../../../22-location-store-redis.ko.md)

이 문서는 외부 provider가 구현하는 최소 public SPI, application이 사용하는 location option과 운영
query, 공식 Redis extension의 public declaration을 고정한다. Authority, owner lease, reservation,
capacity, aggregate와 relocation state machine은 Framework가 opaque record로 encoding한다. Provider는
해당 domain type이나 처리 단계를 알지 않는다.

Store primitive와 abstract Store class는 opt-in CMake target
`zlink::framework_provider_abstractions`가 소유한다. Provider 구현은 이 target만 link해 Store를 구현할 수
있으며 Actor·Spot application target에 의존하지 않는다. C++ namespace는 기존
`zlink::framework`를 유지한다.

## 1. Root option과 등록

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

Store 등록 member는 [Configuration과 host](02-configuration-host.ko.md)의
`zlink_framework_options_t`가 소유한다. Application은 기존 `add_location_store(...)`와
`add_relocation_store(...)`로 두 capability를 각각 한 번 등록한다. 같은 의미의 `use_*` member나
Redis 전용 등록 helper는 제공하지 않는다.

등록이 성공하면 Store instance의 논리적 수명은 Framework가 인수한다. Caller는 등록 뒤 Store를 직접
호출하거나 교체하지 않는다. Framework가 보관한 `std::shared_ptr`는 dependent runtime과 진행 중인
operation을 먼저 종료한 뒤 해제한다. 두 Store가 connection을 공유하면 마지막 Store가 해제될 때
connection을 닫는지, 외부 owner가 connection을 유지하는지는 provider가 관리한다.

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

Key는 Framework가 발급하는 opaque UTF-8 `1..1024` bytes 문자열이며 case-sensitive exact match를
사용한다. Version과 cursor는 provider가 발급하는 opaque UTF-8 `1..4096` bytes 문자열이다.
Value는 최대 1 MiB다. `retention`이 없으면 만료되지 않으며, 만료 판단에는 provider clock을
사용한다. `store_now`는 같은 provider 관측에서 얻은 시각이므로 Framework는 local clock을 TTL
판정에 사용하지 않는다. 지정한 `retention`은 양수여야 한다.

`write(...)`는 모든 condition을 먼저 검사하고 모두 참일 때만 모든 mutation을 하나의 atomic
commit으로 적용한다. 조건 하나라도 거짓이면 mutation과 version 증가는 모두 0이고
`store_write_conflict_t`를 반환한다. Condition은 Missing 또는 exact Version 비교만 제공한다.
Conflict 결과에 domain state나 current value를 싣지 않으며 Framework가 필요한 key를 exact read한다.

한 write request는 condition과 mutation을 합쳐 최대 2,048개의 unique key와 최대 4 MiB의 encoded
크기를 허용한다. 같은 key의 condition 또는 mutation을 중복할 수 없다.

`scan(...)`은 recovery와 maintenance가 bounded key set을 찾기 위한 필수 operation이다. Prefix는
UTF-8 `0..1024` bytes이고 limit은 `1..1000`이다. 첫 page가 만든 snapshot은 마지막 page까지
고정된다. Provider가 snapshot을 더 유지할 수 없으면 `store_scan_expired_t`를 반환하고 Framework는
부분 결과를 버린 뒤 첫 page부터 다시 읽는다. 한 page는 encoded 4 MiB에 도달하면 limit보다 적은
item을 반환할 수 있다.

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

    // Reference가 없어도 성공하는 idempotent operation이다.
    virtual task_t<void> erase(blob_reference_t reference) = 0;
};

} // namespace zlink::framework
```

Reference는 Framework가 put 전에 발급하는 opaque UTF-8 `1..4096` bytes 문자열이며 exact match를
사용한다. 삭제되거나 만료된 reference도 다른 content에 다시 사용하지 않는다. 같은 reference와 같은
bytes를 다시 put하면 `blob_already_stored_t`, 다른 bytes를 put하면 `blob_conflict_t`다. 이 규칙으로
Framework는 timeout이나 연결 오류 뒤에 같은 reference를 exact read하여 저장 결과를 재조정할 수 있다.
`retention`은 양수여야 한다.

Blob 하나는 최대 64 MiB다. Framework는 최대 4,096개의 chunk와 immutable root manifest를 사용해
최대 256 GiB의 logical relocation stream을 구성한다. Checksum, root·chunk 관계, participant
inventory와 relocation phase는 Framework가 소유하며 provider는 payload를 해석하지 않는다.

Provider가 받은 input span은 asynchronous operation이 끝날 때까지만 유효하다. 반환한 byte vector의
ownership은 caller에게 이전된다.

## 4. 호출 실패와 재조정

C++ interface는 다른 언어의 cancellation token을 그대로 옮기지 않는다. Framework가 operation을
시작하지 않은 상태에서 host shutdown이나 deadline이 확정되면 provider를 호출하지 않는다. 호출을
시작한 뒤 timeout, transport 오류 또는 process interruption이 발생하면 commit 여부가 불확실할 수
있다. Framework는 Location Store의 exact read와 version 또는 Relocation Store의 Framework-issued
reference로 결과를 재조정한다.

입력 범위 위반과 같은 caller 오류는 operation을 시작하기 전에 검증한다. Conflict, Missing,
Expired와 AlreadyStored는 정상 result variant이며 exception으로 표현하지 않는다. Provider 장애는
runtime의 provider failure 분류로 변환하며 Redis key나 script 같은 내부 정보를 application error에
노출하지 않는다.

## 5. 운영 query

Application은 저장 key나 private record를 직접 읽지 않고 aggregate projection을 조회한다.

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

NodeRid는 transport routing identity이므로 public `zlink::routing_id_t`를 유지한다. Store version,
private owner token과 provider clock은 운영 query에 노출하지 않는다.

## 6. Redis extension

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

공식 Redis package가 공개하는 provider 표면은 options 두 개와 Store 구현 class 두 개다. Redis key
layout, Lua script, private record encoding, retry와 connection lease는 implementation detail이다. 두
Store는 같은 Redis deployment를 사용하거나 물리적으로 분리할 수 있다. 같은 deployment를 사용해도
서로 다른 `key_prefix`를 사용하며 cross-store transaction은 요구하지 않는다.

## 7. 공개하지 않는 타입

다음 타입과 operation은 Framework private record 또는 Redis implementation detail이다.

- Authority·owner lease·reservation·capacity·fence·aggregate DTO
- `reserve`, `commit`, `abort`, `prepare_aggregate` 같은 domain operation
- relocation phase·manifest·participant DTO와 provider-generated relocation reference
- raw Redis command adapter, script와 key codec
- Spot·Actor 전용 Store와 capability별 Store interface
