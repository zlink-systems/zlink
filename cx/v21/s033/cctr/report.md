213e579f52939d637ea39edd3bfc9d31e15752f6

추천은 C++ public request의 완료 저장소를 binding의 typed completion 저장소로 합치고, Core가 byte credit과 request completion 자리를 먼저 수락한 같은 record에 기존 worker가 body를 나중에 공개하는 구조다. Framework에 submit queue, native retry, 별도 terminal flag를 추가하는 안은 채택하지 않는다. 계약 검토는 이 구조로 진행하되, 현재 자료로 6.6 µs 달성과 같은 rate의 source CPU 비악화를 입증했다고 판정해서는 안 된다. 아래의 6.58 µs는 구현의 합격 예산이며 측정에 근거한 예측값이 아니다.

| V2에서 시작한 caller 회계 | caller µs/request | 수치의 의미 |
|---|---:|---|
| V2 기준 | 6.767 | 변경 (1)에 해당하는 native awaitable 경로를 이미 사용하지만 실제 Framework 작업을 생략한 실험이다. |
| (1)만 제품에 적용하고 실제 작업을 복원한다 | `6.767 + E + R + P` | E는 실제 encode·framing, R은 등록·owner turn, P는 typed 완료 연결 등의 복원 비용이다. 독립 측정값이 없으므로 숫자를 확정할 수 없다. |
| (1)+(2)를 적용한다 | `6.767 − L + N + A + Hc` | L은 기존 public call 경로의 실제 삭제 비용, N은 native submit을 reservation으로 바꾼 caller 비용 차이, A는 caller에 남는 실제 Framework 작업, Hc는 caller 쪽 인계 비용이다. 모두 같은 driver·rate에서 확인해야 한다. |
| 구현 합격 예산의 기준점 | 5.831 | V3를 기준점으로만 쓴다. V2−V3의 0.936을 특정 함수의 삭제 효과로 확정하지 않는다. 새 native reservation 경로도 이 기준점을 유지해야 한다는 요구다. 기존 Core 1.23.0의 실측값을 새 API 호출의 비용으로 대입한 예측은 아니다. |
| 문맥·절대 deadline·기본 timeout 캡처 | +0.15 | caller에 남긴다. |
| 불변 plan 참조·정확하거나 증명된 size bound 취득 | +0.15 | serializer를 실행해 size를 구하는 비용은 이 예산에 들어갈 수 없다. |
| typed binding 저장소·최종 전달 policy 연결의 추가분 | +0.15 | 기존 native 저장소를 대체한다. Framework task를 병행하지 않는다. |
| caller 쪽 setup 게시·통지 | +0.20 | Hc다. worker 쪽 비용은 source 전체 CPU에 별도로 포함한다. |
| public call facade·admission 관찰의 추가분 | +0.10 | 실제 call 생성과 다음 제출 제어를 포함한다. benchmark가 call을 재사용하도록 바꾸어 얻는 수치는 아니다. |
| (1)+(2)의 합격 예산 | **6.581, 반올림 6.58** | `5.831 + 0.75`다. 6.6까지 여유가 0.019뿐이므로 안전한 예측 범위로 해석할 수 없다. |

위 표의 5.831 아래 항목은 V2에 다시 더하는 항목이 아니다. public facade까지 생략한 기준점에서 제품 기능을 복원할 때 허용할 총액을 배정한 것이다. V2에서 직접 계산하려면 `L − N − A − Hc ≥ 0.167`이어야 한다. V2가 (1)을 이미 반영했으므로 V1−V2를 여기서 한 번 더 빼면 이중 계산이다. Request-to-channel에는 selector 비용도 남으며, 위 C++ node request 예산에서 자동으로 충족된다고 보지 않는다.

사용자가 결정할 항목은 다음과 같다.

- C++ request 결과를 `request_task_t<TReply>`에서 binding 소유의 move-only, single-consumer `request_completion_t<TReply>`로 바꿀지 결정해야 한다. 기존 `task_t` 대입, 결과 재관찰, observer와 `result()` 사용은 source compatibility가 깨진다.
- `admitted` 성공을 완성된 body의 enqueue에서, 그 body를 반드시 수용할 byte·work credit과 FIFO 자리를 Core가 확정한 시점으로 바꿀지 결정해야 한다. 이후 encode·등록·Framework 논리 검증 실패는 admitted 성공을 취소하지 않고 reply 실패로 전달한다.
- 정확한 encoded size를 caller에서 구할 수 없는 serializer에 증명된 size bound 계약을 요구할지 결정해야 한다. Size bound 없이 message count만 예약하거나 나중에 HWM을 재판정하는 안은 추천하지 않는다.
- 반환 전에 C1/C2와 dispatcher 자리가 등록되어 있다는 보장을, 반환 전에는 binding이 작업과 수명을 인수하고 wire 공개 전에는 Framework 등록이 완료되어 있다는 보장으로 바꿀지 결정해야 한다. 등록 전 대기의 timeout·취소 관찰도 아래와 같이 달라진다.
- Serializer의 실행 thread 변경과 요청 값의 소유·불변성 계약을 허용할지 결정해야 한다. Java는 현재 factory에서 encode하는 경로가 있고, managed 언어의 mutable 객체를 나중에 encode하면 값의 snapshot 시점도 달라진다.
- 공통으로 바꾸는 것은 admission·오류 timing·lifetime이고, C++에서만 binding completion 타입을 직접 public에 노출하는 투영을 허용할지 결정해야 한다. Node의 임의 JavaScript codec을 다른 thread로 옮긴다는 보장은 이 설계에 없다.

이 결정 목록은 구현이나 spec 수정의 승인을 요청하는 절차가 아니다. 이번 산출물은 읽기 전용 설계 연구이며, 미확정 계약을 기존 계약 적응으로 분류하지 않는다.

**범위와 근거**

기준 commit은 작업 트리의 `.git`가 가리키는 HEAD 파일을 직접 읽어 확인했다. Git 명령은 사용하지 않았다. 경로 축약 C는 `/root/worktree/zlink-cctr/framework/languages/cpp/framework`, BC는 `/root/worktree/zlink-cctr/bindings/cpp`, S는 `/root/worktree/zlink-cctr/framework/doc/framework/common/spec/server`다. D는 `/root/worktree/zlink-cctr/framework/languages/dotnet`, J는 `/root/worktree/zlink-cctr/framework/languages/java`, N은 `/root/worktree/zlink-cctr/framework/languages/node`, BS는 `/root/worktree/zlink-cctr/bindings/doc/spec`다. S01은 `S/01-execution/01-submit-and-completion.ko.md`, S08은 같은 디렉터리의 `08-messaging-hot-path.ko.md`, S05는 `05-payload-ownership-and-codec.ko.md`, lane은 `06-state-ownership-and-lanes.ko.md`다. 아래 `경로:행`은 이 기준 source의 위치다.

[cfloor 보고서](/root/jobs/fx/cx/v21/s033/cfloor/report.md), 그 `final.diff`와 실험 source의 native 연결, [cdes 보고서](/root/jobs/fx/cx/v21/s033/cdes/report.md), [cproto 보고서](/root/jobs/fx/cx/v21/s033/cproto/report.md), [cpr2 보고서](/root/jobs/fx/cx/v21/s033/cpr2/report.md)를 근거로 사용했다. 기존 실험을 다시 실행하지 않았다. cpr2는 별도 작업 트리의 제품 patch이며 성능 측정을 하지 않았다고 명시한다. 그 변경이 현재 HEAD에 적용되어 있거나 그 제품 patch의 성능이 측정되었다고 가정하지 않는다.

이 보고서의 새 타입과 method는 모두 제안이며 현재 공개 API가 아니다. Core는 공개 spec의 동작만 읽었고 `core/include` 및 Core 구현은 읽지 않았다. 따라서 Core 내부 자료구조를 그대로 재사용할 수 있는지까지 확인한 결과가 아니다. 공개 spec의 부재를 Framework의 자체 구현으로 보충하지 않는다.

**결정 소유 표**

| 결정과 소유 조항 | C++ 현재 위치 | .NET 현재 위치 | Java/Kotlin 현재 위치 | Node 현재 위치 | 제안 뒤 소유자 |
|---|---|---|---|---|---|
| C1 logical terminal은 S01:392–397,447–451,462가 소유한다. | `C/src/runtime/foundation/operation_registry.cpp:539–549`의 take다. | `D/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5838–5848`의 TryRemove다. | `J/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:407–422,511–519`의 take다. Kotlin은 이 runtime을 쓴다. | `N/packages/framework/src/runtime/foundation/operation-registry.ts:434–441`의 take다. | 각 Framework registry의 기존 C1 하나다. Binding의 typed 저장소는 승자가 준 결과를 저장하며 logical 승자를 다시 고르지 않는다. |
| Logical deadline은 S01:357–365,453–488이 소유한다. | `operation_registry.cpp:382–474,488–516`의 기존 entry·index다. | `ZLinkManagedMeshNode.cs:5669–5705,12676–12678`의 operation lane·index다. | `ZLinkServiceOperationRegistry.java:341–360`의 deadline·등록이다. | `operation-registry.ts:240–261`의 absolute deadline·등록이다. | 기존 C2 index 하나다. Caller에서 정한 절대 deadline을 그대로 넘긴다. 준비 작업 전용 timer를 추가하지 않는다. |
| Physical admission·WRITABLE 재제출은 BS/async-execution-model:117–125와 Core socket:1123–1157,1225–1240이 소유한다. | `BC/src/Runtime/Messaging/completion_owner.cpp:279–356,585–595,668–763`이다. | `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:136–161,308`이다. | `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:186–198,828–829`이다. | `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:371–460,539–557`의 제출·재제출 engine이다. | 기존 binding/Core다. Reservation·credit 반환·publish도 같은 native operation에서 처리한다. |
| Typed 결과 변환과 전달은 S01:331–333,490–519, S08:300–328이 소유한다. | `C/include/zlink/framework/contracts/dispatch/request_task.hpp:682–698`, `operation_registry.cpp:269–295`다. | `D/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:13–16`의 독립 turn이다. | `ZLinkServiceCompletionDispatcher.java:51–74`의 독립 turn이다. | `N/packages/framework/src/runtime/messaging/request-result.ts:99–132`의 typed Promise 전달과 `runtime/channels/channel-clients.ts:580–607`의 turn 연결이다. | Codec 소유자는 Framework에 남는다. C++의 저장소만 binding bundle로 옮기며, 최종 typed delivery 경로는 하나다. |

현재 task를 유지하는 정적 경로 정리는 기존 계약에서 가능하지만 floor가 요구하는 결과 저장소 제거는 하지 못한다. Raw native awaitable에 Framework map/observer를 붙이는 대안은 typed 저장소와 완료 연결이 다시 생긴다. 추천한 native typed 저장소와 staged record 조합은 이 중복을 제거할 수 있지만 새 binding 계약이 필요하다. 편의성이 아니라 결과 상태의 실제 삭제와 단일 소유를 선택 기준으로 삼았다.

C1 등록과 deadline 등록은 같은 operation 소유 turn이 직렬화한다. 완료 알림을 받은 자리는 C1을 직접 take하고 owner turn으로 왕복하지 않는다. Binding의 physical request 상태는 원격 전송과 native 자원 수명이라는 다른 사실을 소유한다. 그것을 Framework C1이나 C2의 대체품으로 해석하지 않는다.

**변경 (1)의 공개 C++ 표면**

현재 factory는 `C/include/zlink/framework/contracts/channels/channel.hpp:897–944`에 있다. 현재 call terminal은 `call.hpp:53–149,151–225`에 있고, 정식 exact interface는 `S/languages/cpp/interfaces/03-channel-messaging.ko.md:602–631,883–894`다. Factory의 인자와 반환 call 타입을 유지하는 안을 추천한다. 요청별 함수 연결을 줄이기 위해 public factory에 reply 타입이나 native frame을 추가할 필요는 없다.

현재와 제안 factory signature는 같다.

```cpp
// 전과 후가 같다. 두 factory는 아직 제출하지 않은 call을 돌려준다.
template<class TRequest>
channel_request_call_t request_to_node(
  std::string router_channel_id,
  zlink::routing_id_t target_node_rid,
  TRequest request);

template<class TRequest>
channel_request_call_t request_to_channel(
  std::string channel_name,
  TRequest request);
```

반환되는 call의 terminal 결과 타입을 바꾼다. 아래 alias에는 Framework의 별도 상태나 coroutine이 없다.

```cpp
// 제안. zlink::request_completion_t는 아래 binding API에서 새로 정의한다.
template<class TReply>
using request_awaitable_t = zlink::request_completion_t<TReply>;

// 현재
request_task_t<TReply> request_call_t<TReply>::async();
task_t<TReply> request_call_t<TReply>::yield();
// 제안
request_awaitable_t<TReply> request_call_t<TReply>::async();
zlink::async_result_t<TReply> request_call_t<TReply>::yield();

// 현재
// template<class TReply> request_task_t<TReply> channel_request_call_t::async();
// template<class TReply> task_t<TReply> channel_request_call_t::yield();
class channel_request_call_t {
public:
  channel_request_call_t& timeout(std::chrono::milliseconds);
  channel_request_call_t& metadata(std::string key, std::string value);
  template<class TReply> request_awaitable_t<TReply> async();
  template<class TReply> TReply submit();
  template<class TReply> zlink::async_result_t<TReply> yield();
};

template<class TReply>
class request_call_t {
public:
  request_call_t& timeout(std::chrono::milliseconds);
  request_call_t& metadata(std::string key, std::string value);
  request_awaitable_t<TReply> async();
  TReply submit();
  zlink::async_result_t<TReply> yield();
};
```

`submit()`의 반환 타입과 runtime 실행 문맥의 blocking 금지는 유지한다. 구현은 task의 `result().value()` 대신 binding 결과의 blocking 관찰을 사용한다. Call을 복사하거나 같은 call을 다시 실행할 수 있는지에 관한 기존 규칙은 이 변경으로 바꾸지 않는다. `&&` terminal이나 새 submit-once flag를 추가하는 성능안을 섞지 않는다.

현재 public `request_call_t<TReply>::submit_fn_t`는 `call.hpp:57–58`에서 Framework task를 반환한다. 제안 뒤에는 같은 인자 `(const std::string&, std::chrono::milliseconds, const metadata_map_t&)`로 `request_awaitable_t<TReply>`를 반환한다. `channel_request_call_t`의 raw task 기반 submit constructor/alias(`call.hpp:155–165`)와 receiver 기반 private constructor(`:278–290`)는 binding의 미제출 deferred builder를 반환하는 한 연결로 합친다. 그 alias는 `std::function<zlink::deferred_request_t(const std::string&, std::chrono::milliseconds, const metadata_map_t&)>`로 제안한다. Typed terminal이 그 builder에 아래 `request_policy<TReply>`를 부착한다. 두 constructor 경로를 유지하면서 observer로 이어 붙이지 않는다. Public constructor를 직접 사용한 코드도 source 변경 대상이다.

공유 call 타입의 영향은 node/channel factory에 한정되지 않는다. `channel.hpp:766–805`의 spot_request_call_t 상속과 `:440–465`의 Channel client, `contracts/actors/actor.hpp:594–597,921–922`의 relay와 bind, `contracts/spots/spot.hpp:1157–1204`의 typed call도 새 결과 투영으로 이전해야 한다. 같은 공개 class에 새 결과형과 옛 task형을 조건별로 남기는 adapter는 만들지 않는다. 실제로 Core REQUEST를 사용하지 않는 operation의 admission 의미를 이 타입 변경으로 확대하지 않으며, 기존 logical 결과를 같은 binding 저장소 primitive로 투영하는 범위를 C++ 계약 PR에서 확정해야 한다. 이 범위를 승인하지 않는다면 factory 반환형을 별도 call 타입으로 바꾸는 대안을 다시 선택해야 하므로, 현재 제안을 두 파일만의 호환 변경으로 보아서는 안 된다.

제품 경로는 `co_await std::move(pending)`에서 binding의 `async_result_t<TReply>` awaiter를 직접 사용한다. Framework request coroutine이나 public/physical task projection을 거치지 않는다. Public 결과 타입은 다음 기능을 가진다.

```cpp
// 제안. 모든 handle은 같은 binding operation bundle을 가리킨다.
namespace zlink {
template<class T> class request_completion_t {
public:
  request_completion_t(request_completion_t&&) noexcept;
  request_completion_t& operator=(request_completion_t&&) noexcept;
  request_completion_t(const request_completion_t&) = delete;
  request_completion_t& operator=(const request_completion_t&) = delete;

  static request_completion_t ready(T value);
  static request_completion_t failed(std::exception_ptr error);
  bool admission_ready() const noexcept;
  async_result_t<void> admitted();   // 해당 stage를 한 번 꺼낸다.
  async_result_t<T> reply();         // 해당 stage를 한 번 꺼낸다.
  auto operator co_await() &&;       // reply()의 native awaiter다.
  T get_reply();                    // 별도 poller owner가 진행하는 동안 blocking 관찰한다.
};
}
```

현재 binding 결과는 `BC/include/zlink/Contracts/Messaging/operation_contracts.hpp:175–180`의 `async_result_t<void>`와 `async_result_t<vector<message_t>>`다. 그것을 그대로 반환하는 것은 typed Framework 계약을 만족하지 않는다. 제안의 `TReply` 저장소는 기존 raw reply 저장소를 대체하고, raw physical 결과는 producer 내부의 payload·오류로만 보유한다. Raw async result를 만든 뒤 `then`, 별도 Promise, task나 coroutine으로 typed 값을 만드는 안은 제외한다. Preflight 실패와 즉시 결과도 binding의 ready/failed factory로 표현하며 Framework의 `_immediate` task를 만들지 않는다. 이 factory의 단일 소비 상태는 요청마다 격리한다.

Timeout은 caller 진입 때 기존 기본값을 적용해 절대 deadline 하나로 만든다(`channel_runtime.cpp:1742,1851–1860`). 등록 worker가 같은 값을 C2에 넣고, 만료·취소·shutdown·physical 결과는 C1 take로 경쟁한다. C1 승자가 dispatcher에서 `completion_sink_t<TReply>`에 typed 값이나 Framework exception을 저장한다. Binding 저장소의 ready bit는 결과 공개와 단일 소비를 위한 값이며, 두 번째 logical terminal 권한이 아니다.

C++에는 현재 별도 public cancellation 인자가 없다(S/01-execution/03-cancellation-and-shutdown.ko.md:34–35,59). 이 제안도 `async(std::stop_token)`을 새로 만들지 않는다. 기존 ambient wait-owner·host/handler 종료 연결은 request policy가 캡처해 같은 C1 cancellation에 연결한다. Handle drop은 waiter만 detach하며 native 취소를 뜻하지 않는다. Native lifetime과 late cleanup은 BS/async-execution-model:129–152를 재사용한다.

Framework의 service reply 검증(`raw_mesh_node_owner.cpp:1369–1454`), envelope/error 해석(`channel_runtime.cpp:1778–1831`), serializer의 typed decode(`call.hpp:227–247`)는 각 소유 모듈에 남는다. Native 오류는 기존 `raw_binding_adapter.hpp:64–92`와 request failure mapper를 거쳐 Framework 오류로 변환한다. Binding이 Framework 오류 enum이나 service wire를 새로 해석하지 않는다.

Typed decode를 `await_resume()`로 옮기는 안은 제외한다. 그러면 caller에 decode CPU를 더하고, public timeout 뒤 physical 처리에 필요한 변환을 생략하게 된다. Framework에 실제로 전달된 physical 결과는 기존 codec 체인을 한 번 거치고 정리되며, C1을 잃은 뒤의 변환 결과는 이미 완료한 public 결과를 바꾸지 않는다(S01:333). Binding이 correlation을 닫아 Framework에 전달하지 않은 native reply까지 다시 decode하라는 뜻은 아니다.

현재 binding awaiter는 promise의 `zlink_continuation_scheduler()`를 이미 사용한다(`operation_contracts.hpp:84–101`). Framework promise에도 그 hook이 있다(`task.hpp:1162–1165`, scheduler 캡처는 `:319–350`). 이를 우선 재사용한다. Promise hook이 없는 외부 coroutine과 Yield에는 binding이 public continuation policy를 받을 수 있어야 한다. Policy의 resume scheduler와 실제 suspend 시 gate를 반납하는 hook은 기존 native await registration을 확장하며, Framework task를 만들지 않는다. Yield의 gate 반납은 실제 suspend 때 한 번만 하고 같은 queue의 새 turn에서 재개한다. 기존 gate·claim 소유자를 다시 구현하지 않는다.

완료를 poller에서 임의로 inline 재개하는 것은 최적화에 포함하지 않는다. C1 dispatcher가 실행하는 새 turn 안에서 typed 값을 공개하고 그 turn에서 continuation을 실행하거나, 기존 gate scheduler에 한 번 인계한다. 현재 `C/src/runtime/dispatch/provider_task_context.cpp:162–190`의 completion turn 재사용 판정을 사용해 dispatcher에서 다시 같은 dispatcher로 게시하지 않는다. 동일한 실행 권한 판정을 binding과 Framework 양쪽에 복제하지 않는다. Admission 대기의 suspended continuation도 기존 scheduler를 사용한다. 이미 완료된 native 값을 직접 소비하는 동작은 S01:519와 binding의 single-consumer 규칙을 따른다.

제안한 `with_continuation`은 기존 native await 등록의 scheduler 필드를 policy로 대체한다. `on_suspend`는 gate 반납 뒤 재개 scheduler를 반환하며 실제 suspension 경로에서만 호출한다. Ready 확인과 등록 사이에 완료가 경합하면 gate를 반납한 경로를 inline으로 되돌리지 않고 새 turn에 재개한다. 기존 native resume slot의 단일 재개 handshake를 확장하며 별도의 Framework gate-released flag는 두지 않는다. 이 경합 처리는 구현 전 binding contract로 검증해야 한다.

**다른 언어의 공개 타입과 구현 영향**

Native 완료 저장소를 재사용한다는 원칙은 공통으로 적용할 수 있다. C++의 `request_task_t` 제거와 같은 public 타입 교체를 다른 언어에 그대로 적용할 이유는 없다. 이미 언어 표준 awaitable로 투영하는 타입을 유지하고 내부의 추가 완료 소스를 없애는 안을 추천한다.

| 언어 | 현재 public 위치와 타입 | 제안 public 변화 | 필요한 binding 변화 |
|---|---|---|---|
| .NET | `D/src/Zlink.Framework/Contracts/Channels/RouteCalls.cs:7–15`의 IZLinkRouteClient, `Calls.cs:32–48`의 IZLinkRequestCall, `ZLinkRequest.cs:6–25`의 `ZLinkRequest<TReply>`다. | RequestToNode/RequestToChannel과 `Async<TReply>(CancellationToken)` signature를 유지한다. ZLinkRequest의 Admitted·Reply ValueTask가 binding의 typed Task를 직접 감싸게 한다. | `bindings/dotnet/src/Zlink/Contracts/Messaging/OperationContracts.cs:24–41`의 raw RequestSubmission에 대응하는 typed projection·deferred builder가 필요하다. Raw `Task<IReadOnlyList<Message>>` 뒤에 Framework TCS를 더하지 않는다. |
| Java | `J/zlink-framework-core/src/main/java/systems/zlink/framework/channels/ZLinkRequestCall.java:18–36`, `ZLinkRequestStage.java:6–7`, `ZLinkRouteClient.java:10–18`이다. | `submit(Class<TReply>) -> ZLinkRequestStage<TReply>`와 admitted()를 유지한다. Stage wrapper는 binding의 최종 typed future를 위임하고 별도 terminal future를 만들지 않는다. | `bindings/java/src/main/java/systems/zlink/contracts/messaging/RequestSubmission.java:10–18`의 `CompletionStage<List<Message>>`를 생산한 뒤 thenApply chain으로 보충하지 않고 typed 저장소를 직접 생산하는 기능이 필요하다. |
| Kotlin | `J/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/contracts/ZLinkOneWayContracts.kt:26–35`, `ZLinkOneWayCalls.kt:120–137,259–275`다. | `ZLinkKotlinRequestCall<TReply>`와 suspend await/yield는 유지한다. Java stage의 취소 연결을 유지한다. | Java binding의 같은 기능을 사용한다. 별도 Kotlin request engine이나 deadline을 만들지 않는다. |
| Node | `N/packages/framework/src/contracts/Channels/RouteCalls.ts:11–13`, `Calls.ts:10–18`, `Handlers/Attributes.ts:22–24`의 ZLinkRequestCall·ZLinkChannelRequestCall·`ZLinkRequest<TReply>`다. | `submit<TReply>(signal?) -> ZLinkRequest<TReply>`를 유지한다. Binding이 최종 typed Promise를 만들고 admitted를 연결한다. Framework가 raw Promise 다음에 별도 최종 Promise를 만들지 않는다. | `bindings/node/src/zlink/contracts/messaging/operations.ts:54–68`의 raw RequestSubmission에 대응하는 typed projection·deferred 준비 기능이 필요하다. JS callback 실행은 owning isolate/event loop에 남는다. |

현재의 다른 언어도 단순히 raw binding reply를 public에 반환할 수 있는 상태는 아니다. .NET은 `Runtime/Channels/ZLinkRouteClient.cs:387–392,439–443`에서 admission 연결과 encode를 수행한다. Java는 `ZLinkChannelRuntime.java:1351–1365`에서 requestToNode factory가 이미 payload를 encode한다. Node는 `runtime/channels/channel-clients.ts:580–591`의 제출 연결과 현재 Promise 생성을 사용한다. 공통 변경은 이 실제 경로와 함께 수행해야 한다. Node에는 이미 `runtime/messaging/request-result.ts:123–125`의 mapCaller 경로가 있어 추가 then Promise를 만들지 않는 경우도 있다. 이미 제거한 Promise를 새 절감으로 계산하지 않으며, 각 언어에서 실제 남은 저장소와 관찰 연결만 제거 대상으로 삼는다.

Node에서 다음 event-loop turn에 encode를 실행해도 같은 caller thread의 CPU다. 임의 JS serializer를 native worker나 다른 isolate에서 실행할 수 있다고 가정하지 않는다. 새로운 worker·structured clone·복사를 추가해 C++과 같은 off-thread 수치를 주장하는 안도 제외한다. 공통 계약은 작업의 순서와 소유를 정하고, 언어 객체를 실행할 수 있는 자원은 그 언어 runtime이 정한다. C++ caller 6.6을 다른 언어의 CPU 수치로 외삽하지 않는다.

**변경 (2)의 binding 공개 API 제안**

현재 `BC/include/zlink/Contracts/Messaging/operation_contracts.hpp:332–343,366–367`은 완성된 message를 받은 뒤 async를 실행한다. `BC/src/Runtime/Messaging/request_reply.cpp:102–142`가 native bundle을 만들고 `completion_owner.cpp:585–595`가 같은 caller stack에서 첫 request를 실행한다. 기존 public API만으로 body 없는 admission과 worker 준비를 구현할 수 없다.

다음은 새 공개 header의 제안 declaration이다. `Contracts/Messaging/operation_contracts.hpp`에 builder 진입점과 결과 타입을 연결하고, 선언이 길어지면 같은 디렉터리의 `deferred_request.hpp`가 그 선언을 소유한다. Core C signature는 이 연구의 제안 범위에 넣지 않는다.

```cpp
namespace zlink {
using schedule_fn_t = std::function<void(std::function<void()>)>;

struct request_executors_t {
  schedule_fn_t registration;  // 기존 Framework operation lane에 게시한다.
  schedule_fn_t encoding;      // 기존 worker에 게시한다. Poller drain에서 실행하지 않는다.
};

// Sink는 별도 결과 상태가 아니라 같은 native bundle의 producer handle이다.
template<class T> class completion_sink_t {
public:
  completion_sink_t(completion_sink_t&&) noexcept;
  completion_sink_t& operator=(completion_sink_t&&) noexcept;
  completion_sink_t(const completion_sink_t&) = delete;
  completion_sink_t& operator=(const completion_sink_t&) = delete;
  void set_value(T value) &&;
  void set_exception(std::exception_ptr error) &&; // 미완료 admitted와 reply를 함께 실패시킨다.
};

using request_outcome_t =
  std::variant<std::vector<message_t>, std::exception_ptr>;

template<class T> struct typed_request_submission_t {
  zlink_submit_result_t result;      // OK | BACKPRESSURED, 최초 시도 snapshot이다.
  request_completion_t<T> completion; // 같은 bundle의 public typed view다.
};

struct continuation_policy_t {
  schedule_fn_t resume;
  std::function<schedule_fn_t()> on_suspend;
};
// 기존 async_result_t<T>에 합칠 member declaration이다.
template<class T> class async_result_t {
public:
  async_result_t<T> with_continuation(continuation_policy_t) &&;
  // 기존 move, co_await, drop 계약을 유지한다.
};

class deferred_request_t {
public:
  deferred_request_t(deferred_request_t&&) noexcept;
  deferred_request_t& operator=(deferred_request_t&&) noexcept;
  deferred_request_t(const deferred_request_t&) = delete;
  deferred_request_t& operator=(const deferred_request_t&) = delete;

  deferred_request_t&& deadline(std::chrono::steady_clock::time_point) &&;

  template<class TReply, class Policy>
  typed_request_submission_t<TReply> async(Policy&& policy) &&;

  template<class TReply, class Policy>
  TReply submit(Policy&& policy) &&;
};

// request_operation_t에 추가할 member declaration이다.
// 각 part의 payload byte 상한이며 native message의 고정비는 포함하지 않는다.
// span은 호출 안에서 소비한다. Policy가 body 생성도 소유한다.
deferred_request_t request_operation_t::deferred(
  std::span<const std::uint64_t> part_payload_bounds,
  request_executors_t executors) &&;
}
```

이 declaration은 설계안이며 compile 검증한 header가 아니다. Builder는 limit 배열을 기존 operation의 part 표현 안에 소유한다. Policy는 template으로 받아 move-only request 값도 native bundle 안에서 소유한다. 별도 body closure와 Policy가 각각 protocol 문맥을 소유하게 하지 않는다. Policy의 기존 serializer callable과 wire 문맥을 encode와 physical 변환이 공유한다. Factory의 erased encoder도 이 한 Work 안으로 옮기며 Framework에 복사용 두 번째 payload owner를 만들지 않는다.

`async<TReply>`는 snapshot과 native typed view를 가진 `typed_request_submission_t<TReply>`를 반환한다. Framework는 snapshot으로 최초 결과를 처리한 뒤 `completion`만 public으로 반환하므로 Framework의 Backpressured 비노출 계약을 유지한다. 이 분리는 value view이며 추가 결과 상태가 아니다. Async는 다음 계약을 가진 Policy를 같은 bundle 안에 소유한다. Policy는 외부의 logical 결과 소유자와 native 결과 저장소를 연결하는 공개 확장점이다. Binding이 Framework의 service wire나 registry를 직접 알도록 만드는 API는 아니다.

```cpp
// Policy의 요구 형태이며 새 Framework public class가 아니다.
template<class TReply> struct request_policy {
  void register_wait(zlink::completion_sink_t<TReply> final_result);
  std::vector<zlink::message_t> encode();
  void on_physical(zlink::request_outcome_t result);
  std::exception_ptr map_admission_error(std::exception_ptr error);
};
```

`register_wait`는 registration executor의 turn에서 실행한다. Framework가 full OperationId·ReplyRouteId·C1·C2·dispatcher를 기존 registry에 등록하고 최종 callback에 sink를 넘긴다. 등록과 close는 같은 operation lane이 직렬화한다. Policy는 sink를 C1 callback으로 넘기기 전 실패와 넘긴 뒤의 실패를 기존 등록 예외 안전성 규칙으로 구분해야 한다. 이양 뒤 오류는 C1 경로에서 끝내며 binding이 별도로 같은 public 결과를 실패시키지 않는다. `on_physical`은 등록과 합류한 뒤 한 번 호출되어 기존 raw 결과 분류·codec 변환·C1 take로 이어진다. 등록 전에 받은 native 오류는 같은 binding bundle의 기존 failure/submit-completion join에 보존한다. Framework에 early reply map을 추가하지 않는다.

`map_admission_error`는 기존 Framework failure mapper를 호출하는 값 변환이다. Raw binding의 submit_error_t를 Framework public admitted에 그대로 노출하지 않는다. Native admission 실패와 Framework timeout이 경쟁하면, 먼저 raw admitted를 실패시키고 나중에 C1에서 다른 reply 오류를 선택하는 방식도 사용하지 않는다. C1 승자의 final sink가 미완료인 public admitted와 reply에 같은 예외를 저장한다. 이미 admission이 성공했다면 sink는 reply만 실패시킨다. Native 재제출·정리는 별개의 physical producer 상태에서 계속 관리한다.

`completion_sink_t`는 C1을 대체하는 first-wins 판정자가 아니다. 등록된 Framework operation에서는 C1을 획득한 producer만 sink를 사용한다. Binding은 결과 공개·waiter 수명·단일 소비를 보장한다. 기본 raw request policy도 같은 engine을 사용하며 Framework 전용 native engine을 만들지 않는다. 등록이 거절된 시작 실패는 현재처럼 등록된 operation의 terminal 경쟁 밖에서 처리하고, native reservation은 binding이 정리한다.

Generic hook이 기존 native bundle에 들어가지 못해 Framework task/receiver와 같은 결과 상태를 binding 이름으로 하나 더 만드는 구현은 변경 (1)의 목적을 충족하지 못한다. 구현 검토에서는 타입 이름이 아니라 allocation, 결과 상태, continuation 저장 위치가 실제로 대체되는지 확인해야 한다.

**Admission과 body의 순서**

Core에는 같은 request의 record와 credit을 먼저 확보하고, 완성된 body를 같은 record에 나중에 공개하는 기능이 필요하다. 현재 whole-message API가 제공하는 기능은 아니다. 제안하는 관측 동작은 다음과 같다.

1. Caller는 기존 selector로 logical target을 선택하고 최초 monotonic 시각에서 절대 deadline을 만든다. Flow·ambient 문맥과 request 값의 소유도 캡처한다. Request-to-channel의 selector를 Core로 옮기지 않는다.
2. Binding은 public poller owner와 handle을 검증하고 기존 request bundle을 준비한다. Core는 지정 target의 byte HWM, pause/readiness, pair의 work/count budget, unified completion slot을 현재 native admission 규칙으로 판정한다. Body가 없어도 증명된 part 크기 상한과 고정 frame 비용에 해당하는 credit을 실제로 차감한다.
3. Core reservation이 성공하면 admitted 성공이다. Worker queue에 넣었다는 뜻이 아니다. 기존 outbound queue의 record 위치를 확보하고 같은 request completion identity를 보존한다. 이후 body 준비 실패로 admitted를 실패로 되돌리지 않는다.
4. 첫 시도 결과가 OK이든, 대기 token을 가진 BACKPRESSURED이든 binding은 registration executor에 setup을 게시한다. 기존 operation lane에서 C1/C2와 dispatcher를 등록한다. 따라서 backpressure가 지속되어도 등록 worker와 C2는 진행한다. Caller는 이 등록 완료를 기다리지 않는다.
5. 등록 성공과 실제 Core reservation 성공이 모두 확인된 뒤 기존 encoding executor로 진행한다. 등록 turn과 lane-current scope를 벗어난 worker가 선택된 serializer·동적 envelope·canonical service writer로 encode와 framing을 한 번 수행한다. Binding poller의 수신 역할을 가진 채 custom codec을 실행하지 않는다.
6. Binding은 같은 Core record에 완성된 part를 한 번 공개한다. 크기가 상한 이하면 새 admission을 하지 않고, 여분 byte/work charge를 같은 owner에서 반환한다. Body 완성 뒤 capacity 재판정, 다른 request 전송, 추가 FIFO로의 이동은 하지 않는다.
7. 정상 reply, encode 실패, deadline, 취소와 종료는 기존 Framework C1에 합류한다. 기존 dispatcher의 독립 turn에서 typed 결과를 전달하고, native 자원은 기존 binding/Core lifecycle로 회수한다.

등록이 만든 ID·sink·codec 문맥은 기존 합류의 release/acquire 공개 뒤에만 encode·physical 소비자가 읽는다. 완성 body도 같은 record의 공개 전이 뒤에만 Core I/O가 읽는다. 부분 byte가 수신자에게 노출되거나 아직 초기화되지 않은 Framework ID를 사용하는 경로를 두지 않는다. 이 동기화는 기존 operation/queue 공개 표현을 대체하며 별도 Framework ready flag로 보충하지 않는다.

등록 완료와 admission 성공의 합류는 기존 native submit/completion 합류 표현을 대체하는 한 상태 표현이 소유해야 한다. 기존 published/captured/settled 상태 옆에 setup_done·admitted_done·encode_started flag를 추가하는 안은 제외한다. 등록이 닫힌 lane 때문에 거절되거나 executor가 작업을 거절하면 그 작업을 버리지 않고 같은 native failure 경로로 합류한다. Executor는 수락한 작업의 실행 또는 종료 통지를 보장해야 한다.

등록 executor에서 encoding executor로의 인계는 추가 작업 게시다. 기존 worker가 lane scope를 반환한 뒤 다음 turn을 맡더라도 게시·cache 이동·필요한 wake 비용은 남는다. Caller 쪽 handoff와 함께 source 전체 CPU에 포함한다. 등록용 또는 encode용 worker, Framework 송신 queue, 두 번째 poller를 새로 만들지 않는다. C++ encode 실행 자원은 기존 `offload_executor_t`의 worker다(`C/src/runtime/dispatch/offload_executor.hpp:21–57`). Worker 수 상한은 그대로 두고 기존 internal work·drain 수명(`:58–85`)을 재사용한다. Native poller thread, 새 전용 serializer thread, caller의 handler turn을 encode 실행 자원으로 삼지 않는다.

Core에서 record를 예약했더라도 Framework의 logical Ready·등록·close 검증은 setup에서 현재 소유자가 수행한다. 이 검증이 거절되면 admitted 성공 뒤 reply 실패가 될 수 있다. 그 차이를 encode 실패만의 문제로 한정하지 않는다. Logical target 선택은 Framework, physical route와 credit 수락은 Core가 각각 소유한다. Cached Ready나 미리 읽은 HWM snapshot으로 현재 수락을 대신 판정하지 않는다.

**Size bound와 credit**

Byte HWM은 payload 길이만 세지 않는다. Core 공개 Auto HWM spec `core/doc/spec/core/systems/06-auto-hwm.ko.md:390–417,447–460`은 각 frame의 payload와 고정 message 비용을 합산한다. 같은 문서 `:548–575`는 request accounted size에 대한 size-weighted work charge와 count도 정한다. 1 KiB application 값을 native credit 1,024 byte로 예약하는 설계는 잘못이다. 기존 fixture의 serialized body도 1,027 byte이며 envelope/service frame과 고정 비용이 더 필요하다.

Caller에서 기존 serializer를 실행해 size만 구한 뒤 worker에서 다시 encode하는 안은 제외한다. Body 없이 count만 예약하고 완성 뒤 byte HWM을 기다리는 안도 제외한다. 후자는 실제 admission을 완료한 것이 아니다. 모든 message에 큰 고정 상한을 적용하는 안은 cubic work charge 때문에 동시 request 수를 크게 줄일 수 있어 6.6과 0.6을 만족할 근거가 되지 않는다.

추천은 codec의 기존 선택 결과에 encoded payload size의 안전한 상한을 반환하는 능력을 추가하는 것이다. C++에서는 `C/include/zlink/framework/contracts/codecs/serializer.hpp:215–245`의 serializer에 다음 능력을 제안한다. 변경 (2)에 필요한 추가 public 호환성 변경이며 현재 제공되는 기능은 아니다.

```cpp
// 제안. serialize를 호출하지 않고 wire payload byte 상한을 반환한다.
using encoded_size_bound_fn_t = std::function<std::uint64_t(const T&)>;
std::uint64_t serializer_t<T>::encoded_size_bound(const T&) const;
```

선택된 codec, channel 정적 fragment, metadata 설정 때 확정한 정보로 Framework wire writer가 outer part 상한을 구성한다. Binding은 native 고정비를 더한다. Core의 work 식이나 HWM 판정을 Framework에 복제하지 않는다. Overflow, 상한 부재, 잘못된 limit은 reservation 전 typed validation 오류다. 실제 encode의 값·syntax 검증과 구별한다.

기존 custom serializer에는 이 상한 함수가 없다. .NET의 `D/src/Zlink.Framework.Contracts/Codecs/IZLinkMessageSerializer.cs:10–16`, Java/Kotlin의 `J/zlink-framework-core/src/main/java/systems/zlink/framework/ZLinkMessageSerializer.java:6–20`, Node의 `N/packages/framework/src/contracts/Codecs/ZLinkMessageSerializer.ts:3–5`도 변경 후보이다. Default typed JSON을 포함한 codec 소유자가 상한을 제공하며 application에 message별 codec을 등록시키지 않는다. 상한이 없는 codec만 다른 admission 규칙을 쓰는 flag나 fallback engine은 만들지 않는다. 기존 codec의 이전 방법은 계약 승인 전에 해결할 blocker다.

실제 body가 상한보다 작으면 기존 queue charge와 pair work reservation의 같은 값을 갱신해 차액을 반환한다. Framework에 별도 reserved-byte counter를 두지 않는다. 상한을 넘긴 body는 같은 operation의 local encode/contract 오류로 공개 전에 폐기하고 예약을 한 번 반환한다. 추가 credit을 받아 이미 admitted인 record를 다시 backpressure시키지 않는다.

보수적인 상한은 실제 size를 쓸 때보다 admission 거절을 늘릴 수 있다. HWM·work budget 수치를 유지해도 이 차이는 관측 가능하다. Cheap하고 충분히 좁은 상한이 존재하는지, 실제 취득 비용이 얼마인지, Core가 해당 예약 record를 표현할 수 있는지는 이번에 실증하지 않았다. 새 API만으로 caller와 throughput이 모두 개선된다고 단정할 수 없다.

**BACKPRESSURED와 순서**

| 사건 | 같은 operation에서 수행하는 처리 |
|---|---|
| 첫 reservation이 성공한다. | Initial snapshot을 OK로 고정하고 native admitted는 반환 때 성공 상태다. Body는 아직 준비되지 않았다. |
| Byte credit·work/count·readiness 부족으로 nonzero token을 받는다. | Snapshot을 BACKPRESSURED로 고정한다. 기존 binding entry가 target·limit·body closure·deadline을 소유한다. 정확한 token의 WRITABLE을 drain한 뒤 같은 reservation을 DONTWAIT로 재제출한다. |
| 재제출도 backpressure가 된다. | 새 token을 같은 binding entry에 연결한다. Framework retry·sleep·timer나 socket 전체 POLLOUT을 재제출 조건으로 추가하지 않는다. |
| Completion slot 포화로 token이 0이다. | 대기할 token이 없는 시작 실패로 처리하고 현재의 Unavailable로 변환한다. Framework queue에서 나중에 성공시키지 않는다. |
| Admission 전에 binding stage가 실패한다. | Admitted와 reply는 같은 원인으로 각각 한 번 실패한다. Framework projection에서는 C1 승자가 두 public stage의 오류 일치를 보장한다. |
| Reservation 뒤 encode·등록·공개가 실패한다. | Admitted 성공은 유지하고 reply만 실패한다. 같은 reservation을 해제해 다음 record가 진행하게 한다. |
| Setup 전에 native failure가 도착한다. | 같은 binding bundle의 기존 join에 보존한다. 등록 성공 뒤에는 C1을 거치고, 등록 거절이면 기존 시작 실패 경계에서 결과를 전달한다. 두 경우 모두 native는 binding이 정리한다. |

Native completion queue는 기존 owner 하나가 NO_DATA까지 drain하고 그 뒤 WRITABLE 대상만 재제출한다. 근거는 BS/async-execution-model:67–80,104–125, Core socket:1140–1157,1225–1240과 현행 `completion_owner.cpp:668–763`이다. 새 request를 만들어 재시도하지 않고 같은 logical operation에서 native token을 갱신한다. WRITABLE token에서 admitted REQUEST로 진행할 때 native ID 값이 같아야 한다는 뜻은 아니다. Binding의 같은 entry/context가 token 갱신과 REQUEST ID 합류를 소유하고, Framework OperationId와 ReplyRouteId는 별도로 한 번만 만든다.

성공한 reservation의 공개 순서는 Core의 기존 outbound queue 위치가 소유한다. 뒤 body를 먼저 encode해도 앞의 미공개 record를 추월하지 않는다. 앞 record가 실패·timeout·close로 제거되면 같은 queue의 다음 record가 진행한다. Framework sequence counter·재정렬 map·ready flag·두 번째 queue는 추가하지 않는다. Completion과 관리는 미완성 body의 완료를 기다리지 않는다. 연결 선택·handover도 Core가 소유한다. Reservation 전에는 기존 RID/token 계약대로 현재 route를 선택하고, reservation 뒤에는 이미 admitted된 request의 route 종료·handover 계약을 따른다. 준비된 body를 다른 physical pair에 새 request로 재제출하지 않는다. 이 과정에서 Framework의 별도 connection generation·pair index를 추가하지 않는다.

S08:112–113은 같은 caller의 binding 제출 순서를 보장한다. 다른 target 또는 backpressure 중인 모든 request 사이에 새로운 전역 admission 순서를 보장하는 조항은 아니다. 이번 driver는 admitted를 기다려 다음 요청을 내므로 앞 reservation이 성공한 뒤 다음을 제출한다. Admitted를 기다리지 않는 병렬 caller에 더 강한 순서를 제공하려면 별도 계약 결정이 필요하다. 기존 token 회복 규칙 밖의 Framework FIFO로 보충하지 않는다.

**Encode 실패·deadline·등록 대기의 관측**

현재 `channel.hpp:912–921`은 terminal 실행 중 encoder를 사용하고, `channel_runtime.cpp:1884–1886`은 encode가 끝난 뒤 native에 넘긴다. `call.hpp:222–224`는 시작 실패를 public request task로 변환하고 `request_task.hpp:741–749`는 admitted와 physical/public 결과를 실패시킨다. 제안에서는 reservation 성공 뒤 serializer가 실행되므로 같은 encode 오류가 admitted 성공과 reply 실패로 관측된다. Async 반환 때 encode 성공이 확정되어 있다는 전제도 사라진다.

오류의 Framework 분류와 origin은 보존한다. `serializer.hpp:227–238`의 payload_encode→ProtocolError 변환을 worker에서 같은 serializer로 수행하고 `std::exception_ptr`로 전달한다. 뒤늦은 encode 실패를 Timeout, 일반 Unavailable, native protocol error로 바꾸지 않는다. 진단에서는 미공개 body 실패를 구별하되, public timeout/cancellation이 항상 미전송을 보장한다고 바꾸지 않는다.

Encode 또는 크기 검증이 실패하면 binding이 같은 미공개 Core record를 실패로 종료시키는 동작도 새 기능에 포함한다. Core는 그 record의 queue 위치와 byte/work/count 예약을 한 번 회수하고, 이미 수락한 REQUEST의 native terminal completion도 한 번 정리한다. Binding은 원래 encode 예외를 보존해 policy에 한 번 전달하며, native 정리 결과로 그 오류를 덮거나 두 번째 public 결과를 만들지 않는다. 이 내부 실패 종료는 caller wait 취소를 Core operation 취소로 바꾸는 기능이 아니다. Public 실패 뒤 native 정리가 늦게 끝날 때의 bundle 수명도 기존 owner가 보유한다.

첫 native 시도부터 setup 등록까지는 logical timeout·취소를 확정할 C1 entry가 없다. 등록 owner는 캡처한 deadline과 기존 cancellation 입력을 확인하고 이미 만료되었으면 encode·공개를 시작하지 않는다. Native failure는 등록과 합류한 뒤 C1과 경쟁한다. 승자는 먼저 발생한 wall-clock 사건이 아니라 기존과 같이 먼저 terminal 권한을 확정한 경로다. 등록 worker가 지연되면 timeout·취소 관측도 지연될 수 있으며, 현재의 반환 전 등록과 같은 보장으로 취급해서는 안 된다.

이 지연을 없애려고 caller에 임시 C1, 바깥 timeout Promise, 예약 중 flag 또는 별도 timer를 두면 소유자가 늘어난다. 채택하지 않는다. 반환 전 등록 보장을 유지하려면 C1/C2 등록과 operation turn을 caller에 남기고 성능을 다시 평가해야 한다. 등록까지 caller에서 빼는 변경 (2)와 이 선택의 차이를 명시해야 한다.

Logical deadline의 소유자는 기존 Framework C2다. Core의 request timeout은 native correlation의 물리 수명을 끝내는 기존 책임이며 별도의 Framework deadline 계산을 추가하는 근거가 아니다. Staged request의 Core timeout은 예약 때 전달한 잔여 예산을 소비하고 body 준비 시간도 포함하게 한다. 공개할 때 timeout을 재시작하지 않는다. Core가 physical timeout·close로 예약을 끝낸 뒤 도착한 body는 같은 무효 record로의 publish 실패로 정리한다. Framework가 generation 상태를 만들어 다른 대상으로 재제출하지 않는다.

C++의 by-value 값과 codec/runtime 참조는 binding bundle이 physical 처리까지 보유한다. Shared pointer의 대상이나 managed 객체 그래프까지 자동 snapshot되는 것은 아니다. 입력의 불변성을 계약으로 요구할 수 없다면 필요한 snapshot CPU와 copy를 caller에 다시 넣고 예산을 재판정해야 한다. Custom serializer의 임의 thread-local 의존도 worker 이동으로 보존된다고 가정하지 않는다.

**변경할 spec 조항과 유지할 조항**

다음 표는 개정 제안의 위치이며 이번에 spec을 편집한 목록이 아니다. 행 번호는 현재 한국어 정본이다. 승인한 계약의 영문 mirror는 같은 계약 PR에서 동기화해야 한다.

| 소유 문서와 행 | 현재 규칙 | 제안의 처리 |
|---|---|---|
| BS/async-execution-model.ko.md:117–121 | OK이면 admitted가 반환 때 완료되며 BACKPRESSURED이면 WRITABLE 재제출의 admission 뒤 완료된다. | 시점과 snapshot 불변은 보존하고 admission을 완성 body에서 실제 credit·record 위치의 확정으로 확장한다. Encode 성공을 admitted의 조건에서 제외한다. |
| 같은 문서:121–125 | Admitted 실패이면 reply도 같은 원인으로 실패하며, 수명·완료 결과를 지키는 등록 시점은 구현 재량이다. | 같은 원인과 한 번 완료는 유지한다. Admission 뒤 준비 실패는 reply만의 local failure로 정의하고 등록 전 native 결과는 같은 bundle에서 합류한다. |
| 같은 문서:67–80,97–100,129–152 | Poller owner 하나, background drain 금지, caller wait 취소와 native cleanup 분리다. | 유지한다. Encode executor를 completion owner로 만들거나 binding이 worker를 자동 생성하지 않는다. |
| S01:61–71 | Request admitted는 send와 같은 source-local admission이다. | Request의 실제 credit 예약을 admission에 포함할 범위를 공통 admission 정의에서 확정해야 한다. Send 의미를 암묵적으로 바꾸거나 용어 차이를 숨기지 않는다. |
| S01 §9:331–333 | 제출 전에 최종 typed 수신자를 준비해 physical 처리까지 유지한다. | Binding typed 저장소 준비는 반환 전, Framework registry 등록은 wire 공개 전으로 나눈다. Late physical 변환과 한 번 정리는 유지한다. |
| S01 §9:335–340,357–365 | 첫 terminal 하나, 전체 request를 포함하는 절대 deadline 하나다. | 소유자와 budget을 유지하고 예약·setup·encode·publish 시간을 포함한다. Setup 전 만료·취소의 관측 경계를 명시한다. |
| S01 §10:392–405 | C1 take가 완료 권한을 정하고 모든 request를 등록한다. | C1을 유지한다. 등록 거절과 native 예약 뒤 실패의 합류를 정하며 binding에 두 번째 logical arbiter를 두지 않는다. |
| S01 §10:424–441 | ID·pending·dispatcher·reply route 등록 뒤 transport에 submit한다. | Body 없는 reservation은 등록 전에 허용하고 wire 공개는 모든 등록 뒤로 제한한다. 단어만 submit에서 publish로 바꾸는 내부 정리가 아니라 계약 변경이다. |
| S01 §10.1:447–466 | Pending 하나, entry의 deadline, 등록·close의 owner 직렬화다. | 소유와 pending 하나는 유지하고 반환 뒤 최초 등록되는 기간을 명시한다. Caller에 임시 pending을 추가하지 않는다. |
| S01 §10.1:468–488 | C2 참조 회수·방문 시각·reply와 timeout 경쟁 규칙이다. | 유지한다. 예약 전용 timer나 두 번째 index를 추가하지 않는다. |
| S01 §11:490–519 | Callback과 suspended continuation은 독립된 새 turn에서 실행한다. | 유지한다. Binding 직접 await를 이유로 poller에서 public callback을 inline 실행하지 않는다. |
| S01 §11:521–540 | Dispatcher 자리는 capacity 부족으로 실패하지 않으며 inflight 제한은 Core가 소유한다. | 유지한다. Setup 게시의 shutdown 실패와 새 capacity 제한을 혼동하지 않는다. |
| S08 §3:68–70,82–85,96–100 | E1은 caller, E3/E4는 operation turn에서 동기적으로 끝난다. | E1 encode와 E3 등록을 caller 뒤로 옮긴다. Caller는 문맥·limit·Core 예약·작업 인계를 수행한다. 같은 소유 절의 sequence와 표를 함께 개정한다. |
| S08 §3:102–113,136–139 | 제출 자원 이전은 selector·operation owner로의 최대 두 번이며 별도 등록 queue를 추가하지 않는다. | Caller→setup과 setup→encode의 실제 이전·게시 비용을 포함하도록 허용 경계를 새로 정한다. 기존 상한을 만족한다고 주장하지 않는다. Binding 제출 순서는 유지한다. |
| S08 §4.4:300–308 | E5 새 turn과 poller를 포함한 수신 역할을 정의한다. | 유지한다. 수신 역할을 유지한 자원에서 typed callback·custom encode를 실행하지 않는다. |
| S08 §4.4:309–316 | 같은 자원에서 application turn을 실행하기 전에 수신 역할을 확실히 인계한다. | 유지한다. Codec은 기존 worker에서 실행한다. 역할 인계를 쓰면 기존 owner가 성공을 판정하며 게시 성공을 인계 성공으로 보지 않는다. |
| S08 §4.4:322–326,328–332 | Gate·queue·callback 독립·자원 상한과 handoff 비용 회계다. | 유지한다. 새 poller·worker를 만들거나 typed callback들을 하나의 blocking drain으로 묶지 않는다. Node는 같은 event loop의 별도 turn을 사용한다. |
| lane §5:166–191,214–228 | FIFO owner turn, 재진입 금지, scope 밖 완료와 blocking 조건이다. | 유지한다. Setup→encode를 worker의 동기 wait로 연결하지 않는다. |
| lane §5:230–255 | 동기 method의 등록·캡처·claim은 반환 전에 끝내며 등록 자체를 뒤로 보내지 않는다. | 변경이 필요하다. Request는 캡처·binding 소유권 이전을 반환 전, Framework 등록을 wire 공개 전으로 한다. 이 계약은 S01이 소유하고 lane은 참조한다. 모든 lane 작업으로 일반화하지 않는다. |
| S05:34–42,218–239 | 추가 copy를 없애고 불변 registry의 codec 선택을 재사용한다. | 유지한다. Size bound·codec 실행 문맥·입력 소유의 새 계약은 codec 소유 절에 둔다. Custom codec만을 위한 별도 flag·queue를 만들지 않는다. |
| C++ exact interface, S/languages/cpp/interfaces/03-channel-messaging.ko.md:602–631,883–894 | Request task와 call terminal·factory 타입이다. | Factory는 유지하고 request 결과·Yield 타입·단일 소비·blocking 관찰을 새 binding 타입으로 바꾼다. |
| BS/cpp/README.ko.md:373–390,556–599, BS/async-coroutine-policy.ko.md:160–168 | Builder·terminal 이름·raw native 결과와 timeout이다. | Request builder에 deferred 단계·typed projection을 추가한다. Operation은 request, terminal은 async/submit으로 유지한다. Native ID·raw drain은 공개하지 않는다. |

Core의 동작 변경 위치도 특정할 수 있다. `core/doc/spec/core/socket/README.ko.md:49–53,1091–1099`의 미완성 record 부재와 staged request를 정합시켜야 한다. 같은 문서 `:1123–1157,1219–1245`에서 예약·WRITABLE·동일 request 공개·timeout 기산점을 정한다. `core/doc/spec/core/systems/06-auto-hwm.ko.md:396–413,447–460,476,548–575`에서 상한 credit의 보유·차액 반환·oversize·work/count 소유를 정한다. 이 규칙을 Framework에 복제하지 않는다.

S08:12,396–401에는 현재 별도의 0.90 spec 기준이 있다. 이번 연구는 그 값을 수정하지 않는다. 사용자가 지정한 Framework÷binding 0.6과 caller 6.6도 낮추지 않는다. Caller 예산만 만족한 것으로 어느 throughput 기준도 통과했다고 판정하지 않는다.

**현행 계약에서 실제로 도달한 값**

실제 Framework 작업과 public task를 유지한 도달 근거는 cproto의 최종 17.476 µs/request다. 과거 그 보고서 조건의 실측점이며 현재 HEAD나 cpr2 제품판의 새 측정값은 아니다. V1의 9.632는 encode·framing·C1/C2·owner turn을 생략한 floor이므로 이 질문의 도달값으로 사용하지 않는다.

| 현행 계약에서 구현한 단계 | cproto의 전 단계 대비 caller 변화 | 기능을 보존하는 위치 |
|---|---:|---|
| 준비된 transport entry와 bind/type plan, 단계 1–2 | −0.281 µs | Typed encode·동적 header·등록을 실제로 실행한다. |
| 최종 service payload에 header를 한 번 쓰고 admission projection을 삭제한 단계 3–4 | −0.061 µs | Canonical framing·실제 admission·최종 receiver를 유지한다. |
| Host forwarding 삭제, 단계 5 | −0.013 µs | Raw owner의 등록·deadline과 binding retry 소유를 유지한다. |
| 정적 fragment 작성의 소유를 통합한 후속 점검 | −0.087 µs | Writer를 하나로 통합한다. |
| 합계 | **−0.442 µs, 도달 17.476 µs** | Public task와 실제 Framework 처리를 유지한 실험이다. |

단계별 차이는 서로 다른 build 중앙값의 차이이며 개별 함수의 독립 CPU가 아니다. 작은 단계 차이에서 추가 절감량을 외삽하지 않는다. Cproto에는 이름 lookup과 variant가 남았고 cpr2는 해당 단계의 제품화와 wire 검증을 수행했다. Benchmark가 매 요청 call을 생성하므로 같은 caller의 일을 call 생성이나 bind라는 이름으로 옮긴 것만으로 비용이 사라지지 않는다.

현행 계약의 모든 동작을 보존한 추가 최적화의 최선값은 자료로 수치화할 수 없다. 17.476을 이론 하한이라고 할 수도, 9.632를 제품에서 실제 도달할 값이라고 할 수도 없다. C1/C2·wire·typed decode를 생략한 floor로 실제 제품의 6.6을 입증할 수 없다. 기존 serial PR의 절감을 새 D로 다시 계산하지 않는다.

**Caller에서 이동하는 CPU와 source 전체 수지**

Caller에서 제외하는 encode·framing과 C1/C2 등록은 worker에서 실행되므로 CPU 자체가 삭제되지는 않는다. Native typed 저장소 통합으로 삭제하는 task·projection·observer와 실제 작업의 이동을 구분한다.

| 작업 | Caller에 남는 부분 | Caller 밖 실행과 비용 |
|---|---|---|
| Factory·입력 소유 | 기존 application 값 생성·call 생성·type/route plan 참조다. | Binding bundle이 원래 값을 보유한다. Managed snapshot이 필요하면 copy도 계상한다. |
| Deadline·flow·ambient | 최초 캡처다. | Worker에 같은 값을 복원한다. 새 deadline은 만들지 않는다. |
| Native admission | 실제 credit·completion slot 예약과 결과 관찰이다. | 같은 operation 공개 G와 credit 차액 반환 Q가 추가된다. |
| C1/C2 등록 | Callback·sink 연결 준비다. | 기존 operation lane으로 R을 옮긴다. Setup 게시 수신·queue 처리 Hs가 필요하다. |
| Encode·framing | Cheap size bound다. | 기존 worker로 E를 옮긴다. Encode 측 게시·수신 Hw와 cache/allocator 차이가 추가된다. |
| Typed reply·terminal | Framework task 생성·receiver 연결을 삭제한다. | 필요한 typed decode·C1 take·dispatcher·native cleanup은 유지한다. |
| Application 관찰 | Admission 제어·reply 검증·slot 회수·benchmark CPU다. | 기존 application의 선택 밖으로 옮긴 일을 caller 절감이라고 하지 않는다. |

같은 rate r의 현재 source CPU를 F0(r), 삭제하는 실제 CPU를 D(r), caller에서 이동하는 실제 CPU를 W(r)로 둔다. 새 native 예약 API 차이를 N, caller 인계를 Hc, setup 수신을 Hs, encode 수신을 Hw, 공개를 G, 상한 credit 등의 증가분을 Q, cache/allocator/기존 알림 차이를 K로 둔다. 모든 값은 같은 rate의 요청당 CPU다.

```text
Fnew(r) = F0(r) − D(r) − W(r) + W(r)
          + N(r) + Hc(r) + Hs(r) + Hw(r) + G(r) + Q(r) + K(r)

Fnew(r) ≤ F0(r)
  ⇔ D(r) ≥ N(r) + Hc(r) + Hs(r) + Hw(r) + G(r) + Q(r) + K(r)
```

W는 상쇄된다. Native retry·completion poller 재사용은 새 engine의 추가 비용을 피하지만 기존 engine의 CPU를 D로 삭제 계상할 수 있다는 뜻은 아니다. K에는 이동으로 줄거나 늘어난 비용을 모두 포함한다. 같은 통지·I/O batching 효과를 여러 절감 항목에 반복 반영하지 않는다.

추가 CPU의 임시 합격 예산을 N≤0, Hc≤0.20, Hs+Hw≤0.30, G+Q+K≤0.50 µs/request로 두면 D≥1.00 µs/request가 같은 rate에서의 비악화 충분조건이다. 이 값들은 독립 실측값이 아니다. 상한 credit 때문에 동시 request 수가 줄거나 worker의 codec 비용이 높아져 rate를 유지하지 못하면 불합격이다.

지정 자료에는 새 N/G/H/Q/K와 실제 Framework 기능을 보존한 task 통합의 D를 같은 rate에서 측정한 값이 없다. Cfloor의 source CPU는 변형별 rate가 다르고 V1–V3에는 기능 생략도 있다. 그 차이로 새 source CPU가 현재 이하임을 수치로 입증할 수 없다. 이 요구는 이번 자료에서 미입증이며, 다른 rate의 CPU 차이를 같은 rate의 절감으로 대체하지 않는다.

작업을 옮겨 받는 자원도 병목이 되지 않아야 한다. 하나의 serial 자원이 r requests/s를 처리하면 그 자원의 전체 service CPU가 `1e6/r` µs/request 이하여야 한다. 등록 R과 encode E뿐 아니라 같은 자원의 관리·수신·completion CPU도 합산한다. 공유 worker의 request별 encode CPU는 batch 크기로 나눌 수 없다. 고정 지연으로 batch를 만들지 않으며 실제 공통 게시·wake 비용만 실제 batch 크기로 나눈다.

**위험과 PR 분할**

주요 blocker는 cheap size bound, 반환 뒤 등록 보장의 수용 여부, 예약 record와 기존 queue/accounting의 통합 가능성, native bundle 통합의 실제 상태 삭제다. 미공개 record의 head-of-line 대기, mutable 값 snapshot, codec 실행 문맥, 느린 setup과 close, typed decode 실패 뒤 정리도 검증해야 한다. Credit 상한과 기존 worker의 비용을 0으로 두지 않는다.

다음은 PR 순서 제안이며 이번에 PR을 만들지는 않았다. 각 PR의 수치는 직전의 실제 경로를 기준으로 확인해야 한다. Δcaller≤0.00 µs는 해당 PR의 caller CPU를 늘리지 않는 목표다.

| 순서와 PR | 구현 범위·삭제 | Caller 목표 | 합격 조건 |
|---|---|---:|---|
| 0. 감독자의 계약 확정 | Admission·반환/등록·codec 상한·입력 소유·C++ 결과 타입의 D 결정을 정본과 exact interface에 확정한다. | Δ≤0.00 µs | 구현에 맞춰 spec을 묵시적으로 완화하지 않고 사용자 차이를 모든 언어에 확정한다. |
| 1. Core와 binding staged request | 실제 credit 예약·동일 record 공개, C++ deferred builder·typed native bundle·기존 executor 게시·continuation policy를 제공한다. | 같은 driver의 direct binding staged caller **≤5.831 µs**, 기존 Framework caller Δ≤0.00 µs | Worker에서 실제 body를 만들고 같은 rate의 N/G/H/Q/K를 측정한다. 기존 raw 경로와 engine을 공유하며 Framework에서 Core header를 직접 호출하지 않는다. |
| 2. 각 binding의 언어 투영 | .NET typed Task, Java/Kotlin typed stage, Node typed Promise와 동일한 예약·수명을 공개한다. | 각 binding Δ≤0.00 µs | 각 언어의 같은 rate에서 측정한다. Node의 새 isolate·자동 drain thread를 만들지 않는다. |
| 3. C++ Framework | Public task/receiver·raw observer chain을 삭제하고 deferred body·반환 뒤 등록을 연결한다. 두 변경을 같은 제품 검증에서 확인한다. | **≤6.60 µs, 내부 예산 6.58** | 실제 encode·고유 identity·deadline·typed 내용 검증을 수행하며 비율≥0.6과 같은 rate의 Fnew≤F0를 만족한다. Channel selector 비용도 별도 확인한다. |
| 4. .NET Framework | ZLinkRequest의 ValueTask view를 typed binding Task에 직접 연결하고 중간 admission·결과 소스를 삭제한다. | Δcaller **≤0.00 µs** | .NET의 기존 caller에서 절대값을 확정한다. C++ 수치를 대입하지 않으며 cancellation 의미를 보존한다. |
| 5. Java/Kotlin Framework | Factory encode 시점을 변경하고 ZLinkRequestStage를 typed binding stage에 직접 연결한다. Kotlin cancel/yield도 함께 정합시킨다. | Δcaller **≤0.00 µs** | Snapshot 시점과 취소 연결을 검증한다. Java·Kotlin의 request runtime을 나누지 않는다. |
| 6. Node Framework | 최종 typed Promise를 binding으로 통합하고 native 예약·JS encode 순서를 맞춘다. | Δcaller **≤0.00 µs** | 같은 event-loop 후속 turn을 다른 thread의 절감으로 세지 않는다. Public stage 의미와 event-loop 응답성을 검증한다. |

다른 세 언어의 절대 caller 값은 이번 지정 자료에 없으므로 임의의 절대 µs 목표를 만들지 않았다. 표의 Δ≤0.00은 각 PR의 수치 목표이며 개선의 증명이 아니다. 공통 계약을 C++만 바꾼 상태로 완료 처리하지 않고 각 언어 PR을 같은 개정의 완료 조건에 포함한다.

C++ PR 안에 변경 (1)만의 검토용 중간 commit을 두는 것은 가능하다. 그러나 그것을 6.6 달성 PR로 분리해 완료 처리해서는 안 된다. V2에 실제 기능을 복원한 caller는 미측정이며, 필요한 binding 기능보다 Framework 보조 queue를 먼저 구현할 근거도 없다.

**삭제와 신규 상태 점검**

| 변경 | 삭제·대체하는 기존 기구 | 유지할 소유자와 신규 상태 제약 |
|---|---|---|
| (1) Public 결과 | `request_task.hpp:648–750`의 receiver_state, admitted/public/physical task와 관찰 연결을 해당 request에서 제거한다. `call.hpp:129–140`의 submit_owned coroutine을 native 결과 직접 반환으로 대체한다. | 기존 binding bundle의 raw reply 저장을 typed 저장으로 대체한다. Framework 결과 완료 flag·producer state를 추가하지 않는다. |
| (1) Raw 연결 | `raw_binding_adapter.hpp:147–198`의 await observer와 `raw_route_port.cpp:359–393`의 task admitted 연결을 typed native policy로 대체한다. | Native completion entry와 C1은 유지한다. 같은 coroutine을 binding 이름으로 옮기는 것만으로 완료하지 않는다. |
| (2) Body 표현 | Caller의 typed encode·envelope·service packing 실행을 이동한다. Deferred body closure가 기존 준비된 part 보관을 대체한다. | 같은 bundle에서 owned input을 완성 part로 전환하고 두 표현을 불필요하게 함께 보존하지 않는다. 새 payload queue를 추가하지 않는다. |
| (2) 등록 | Caller의 `raw_mesh_node_owner.cpp:1519–1578` run_sync 등록/submit을 기존 operation lane의 setup 작업으로 대체한다. | C1/C2 map/index·close 직렬화는 유지한다. Caller 임시 entry·terminal flag·별도 deadline index를 만들지 않는다. |
| (2) Native 수명 | `completion_owner.hpp:81–90`의 operation 보관과 published/captured/settled/writable join을 하나의 staged operation 표현으로 대체한다. | 기존 bool 옆에 reserved/prepared/published flag를 추가하지 않는다. Core도 기존 record payload 표현·charge의 대체로 설계하며 예약 counter·두 번째 FIFO를 추가하지 않는다. Core 구현 가능성은 미확인이다. |
| 기존 serial PR | cpr2의 transport index·static plan 통합, header 중간 message·host forwarding·admission projection 삭제로 수렴한다. | 이미 삭제한 기구를 새 D에 다시 세지 않는다. 새 cache·pool·arena·socket lock을 추가하지 않는다. |

변경 전/후의 판정 위치 수는 등록된 request의 C1 terminal **1→1**, logical deadline **1→1**, physical admission/WRITABLE **1→1**, 최종 typed delivery **1→1**이다. 결과 저장소는 Framework task와 binding raw awaitable의 병존에서 binding bundle의 typed public 저장과 내부 physical producer 상태로 통합한다. Physical 상태는 다른 logical terminal 판정자가 아니다.

여섯 기구를 점검해 삭제·대체안을 정리했으며 실제 수정은 0건이다. Framework의 새 flag/counter/queue/lock/poller를 추가하지 않는 것이 설계 조건이다. Binding/Core는 기존 상태와의 대체 관계가 필요하다고 특정한 단계이며, 구현을 확인하지 않고 추가 상태 0건을 입증했다고 보고하지 않는다. 삭제 없이 새 상태가 필요해지면 이 안의 같은 blocker로 재판정해야 한다.

**구현 뒤 검증 조건과 이번 결과**

이번에는 build·test·benchmark를 실행하지 않았다. 뒤 구현에서는 기존 request transport·operation registry·state lane·native-count·wire fixture와 binding request/reply contract를 사용한다. 실제 Framework 기능을 복원한 public API로 정상 입력과 의도적인 위반 입력을 함께 검증해야 한다.

- 실제 encode하는 정상 request와 admission 뒤 serializer가 예외를 내는 request를 대조한다. 후자는 admitted 성공·reply의 원래 Framework encode 오류로 끝나야 한다.
- 상한 이내 body와 상한 초과·overflow·상한 부재를 대조한다. Credit 이중 계상·공개 뒤 재admission·예약 누수를 검출해야 한다.
- 정확한 target/token의 WRITABLE과 다른 target 회복·token 0·중복 token을 대조한다. NO_DATA 전 재제출을 검출하고 같은 operation의 진행을 확인해야 한다.
- 앞 body를 늦추고 뒤 body를 먼저 완성해 admission 순서의 공개를 확인한다. 앞 record의 실패·timeout·close 뒤에는 후속이 진행하고 completion·관리는 body 대기와 독립적이어야 한다.
- Setup 전·중·후 deadline·ambient cancellation·shutdown을 경쟁시킨다. C1 승자 하나·같은 절대 deadline·late physical 변환·native 정리·독립 dispatcher 전달을 확인한다.
- 즉시 결과와 suspended 결과, 일반 await와 Yield, Framework promise와 외부 coroutine을 대조한다. 수신 역할의 poller에서 public callback을 실행하는 구현을 검출한다.
- 한 결과의 소비·drop·취소가 다른 request에 전파되지 않는지 C++와 다른 언어의 public stage로 확인한다.
- Caller·setup·encode·receive·completion·I/O·process 전체 CPU를 같은 rate에서 집계한다. 비교하는 binding도 새 Core/binding package의 같은 버전과 동일 조건을 사용한다. Driver의 내용 검증·payload 생성·slot 회수를 이동하거나 생략하지 않았는지도 확인한다.

이번 결과는 공개 타입 변경안, binding 선행 기능, 바뀌는 계약, 단일 소유 구조와 성능 수지의 합격 조건을 특정한 것이다. 6.58은 합격 예산이며 도달 예측이 아니다. 같은 rate의 source CPU 비악화와 cheap size bound는 미입증이다. 현행 계약에서 실제 Framework 작업을 유지한 도달 근거는 cproto의 17.476이다.

소유 계층은 Framework의 C1/C2·codec·typed delivery와 binding/Core의 native admission·lifecycle이다. 근거 조항은 S01 §9–11, S08 §3/§4.4, lane §5, binding async model §5–6이다. 교차언어 대조는 정적 조사이며 Java factory encode와 Node event-loop 제약을 추가 계약 판단으로 특정했다. 변경 분류는 **D(spec gap을 가진 새 계약안)**이며 A/B 승인된 runtime 수정이 아니다.

지정 report 외의 작업 트리 파일·spec·build 산출물을 변경하지 않았다. Git·GitHub 작업, 내부 docs 준비 script, sub-agent, 다른 model/AI tool을 사용하지 않았다.

보고서의 commit 표기·코드 블록 쌍·산술을 확인했다. Caller 예산 합계 6.581과 cproto 단계 합계 0.442를 확인했고, 예산 한 항목을 0.01 바꾼 입력은 합계 불일치로 거부했다. 이는 runtime test나 신규 측정이 아니다.
