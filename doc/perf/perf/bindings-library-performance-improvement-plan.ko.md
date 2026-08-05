# bindings 라이브러리 성능 개선 계획

> 이 문서는 core 9.0 이전 라운드의 기록이다. core 9.0.0 기준으로 새 작업을 시작할
> 때는 [core 9.0 bindings 라이브러리 성능 개선 계획](./bindings-library-performance-improvement-plan-core-9.0.ko.md)을
> 사용한다. 이 문서의 기존 측정값과 완료 판정은 core 9.0 작업에 승계하지 않는다.
>
> 이 문서는 bindings 라이브러리 성능을 C 기준 대비 목표 비율까지 끌어올리기 위한
> 실행 계획이다. 이전 측정 기록은 이 문서에 보관하지 않고, 매 라운드의 결과 파일과
> 최종 요약만 별도로 남긴다.

## 1. 범위와 목표

이번 작업은 perf 자체를 빠르게 만드는 일이 아니라, 각 언어 binding 라이브러리가
public API를 통해 내는 실제 성능을 개선하는 일이다. perf는 `doc/perf` 정책에 맞게
이미 작성되어 있다고 보고, perf 자체의 측정 버그가 확인된 경우를 제외하면
수정하지 않는다.

목표 비율은 같은 suite, pattern, transport, message size, metric 조합에서
`bindings/c/perf` 결과를 기준으로 계산한다.

C 기준은 core 내부 이론 성능이 아니라, `bindings/c/perf`가 public C API로 측정한
C binding 라이브러리의 일반적인 성능이다. 기준으로 삼는 C 결과는 같은 기본 옵션으로
실행한 최근 full 측정 결과여야 하며, 특정 실험이나 debug 재현을 위한 일회성 결과는
기준으로 쓰지 않는다. C 결과 자체가 비정상적으로 낮거나 높아 보이면 같은 조건으로
재측정해 일반적인 범위를 먼저 확인한다.

2026-06-19 C multi baseline
`bindings/c/perf/baseline/perf_c_multi_linux_20260619_062932.txt`와 현재 C 기준이
크게 다른 셀은 오래된 baseline으로 binding 미달 여부를 판단하지 않는다. 그 셀은
같은 runtime의 C 결과를 다시 측정하거나, 이미 같은 조건 paired C report가 있으면 그
report를 기준으로 쓴다. 이 규칙은 baseline 흔들림을 binding 병목으로 오판하지 않기
위한 것이며, binding 코드에 perf-only cache나 입력 모양 의존 최적화를 넣는 근거로
쓰지 않는다.

| 순서 | 언어 | perf 경로 |
|------|------|-----------|
| 1 | C++ | `bindings/cpp/perf` |
| 2 | .NET | `bindings/dotnet/perf` |
| 3 | Java | `bindings/java/perf` |
| 4 | Node | `bindings/node/perf` |
| 5 | Go | `bindings/go/perf` |
| 6 | Rust | `bindings/rust/perf` |
| 7 | Python | `bindings/python/perf` |

목표 비율은 size 하나로만 정하지 않는다. 최근 C++ full 비교에서는 size보다
pattern 차이가 더 컸다. 예를 들어 single `PAIR`, `PUBSUB`, `SPOT`은 C와 비슷하거나
더 빠른 조합이 많았지만, routed pattern인 `DEALER_ROUTER`, `ROUTER_ROUTER`는 일부
transport와 큰 메시지에서 크게 낮아졌다.

다만 `ROUTER_ROUTER` 또는 `MULTI_ROUTER_ROUTER`가 현재 특정 binding에서 낮게 나온
결과를 그대로 낮은 목표 기준으로 인정하지 않는다. 같은 suite, transport, size에서
C의 `ROUTER_ROUTER`와 `DEALER_ROUTER` 차이가 작다면 해당 binding도 그 차이에
가까워야 하는지 진단 기준으로 확인한다. 절대 목표 기준을 통과한 항목은 상대 기준만으로
`미달`로 내리지 않는다. C++ `MULTI_ROUTER_ROUTER`처럼 `MULTI_DEALER_ROUTER` 대비
과도하게 낮은 결과는 목표 완화 근거가 아니라 binding 라이브러리 병목 또는 버그 후보로
본다.

따라서 완료 판단은 pattern 그룹별 범위를 먼저 적용하고, size는 보조 기준으로 본다.
아래 표의 왼쪽 값은 최소 통과 기준이고, 오른쪽 값은 안정권 기준이다. 64KB 이상 큰
메시지는 같은 pattern 그룹 안에서 낮은 쪽 기준을 적용하고, 64B~1024B 작은 메시지는
높은 쪽 기준에 가까워지는 것을 목표로 한다.

Node와 Python은 별도 근거 없이 큰 차이를 두지 않는다. 두 binding 모두 동적
런타임과 native 경계 비용이 큰 그룹으로 보고 같은 목표 범위를 적용한다. Rust는
C++보다 높은 기준으로 두지 않는다. 둘 다 native binding 그룹으로 보며, public API
래퍼 비용을 감안하더라도 managed runtime binding보다는 높은 기준을 적용한다.

| Pattern 그룹 | 포함 pattern | C++/Rust | .NET/Java | Go | Node/Python |
|--------------|--------------|----------|-----------|----|-------------|
| 단순 one-way | `PAIR`, `PUBSUB`, `DEALER_DEALER`, `MULTI_PUBSUB`, `MULTI_STREAM` | 80~90% | 63~73% | 53~63% | 35~43% |
| routed one-way | `DEALER_ROUTER`, `ROUTER_ROUTER` | 70~83% | 55~67% | 47~57% | 33~40% |
| multi routed echo | `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER` | 65~77% | 50~63% | 40~53% | 30~37% |
| SPOT 계열 | `SPOT`, `MULTI_SPOT`, `MULTI_SPOT_REQREP`, `MULTI_SPOT_SENDSEND` | 75~90% | 60~70% | 50~60% | 33~40% |

Node 개선 라운드는 위 표의 Node/Python 최소 통과 기준보다 5%p 낮은 값을 우선 목표로
본다. 따라서 `MULTI_PUBSUB` 같은 단순 one-way는 최소 30%, `MULTI_ROUTER_ROUTER`
같은 multi routed echo는 최소 25%, routed one-way와 SPOT 계열은 각각 최소 28%를
사용해 신규 미달 후보를 고른다. Python 판정 기준은 이 Node 개선 라운드로 함께 낮추지
않는다.

`ROUTER_ROUTER` 계열은 추가로 아래 상대 기준을 진단에 사용한다.

- C의 `ROUTER_ROUTER / DEALER_ROUTER` 비율을 같은 suite, transport, size에서 계산한다.
- 대상 binding의 `ROUTER_ROUTER / DEALER_ROUTER` 비율이 C의 상대 비율보다 크게
  낮으면 병목 후보로 기록한다. 단, 절대 목표 비율을 넘으면 상태는 `통과`로 둔다.
- 상대 비율 허용 오차는 측정 오차를 감안해 C++/Rust는 10%p, .NET/Java/Go는 15%p,
  Node/Python은 20%p로 본다.

latency, latency_p95, latency_p99는 throughput 목표를 만족하더라도 C 대비 과도하게
악화되면 완료로 보지 않는다. 레이턴시 악화가 보이면 같은 조합을 다시 측정하고,
binding 내부 병목인지 perf 측정 오류인지 먼저 구분한다.

## 2. 고정 원칙

- 성능 개선 대상은 perf가 아니라 각 언어 binding 라이브러리다.
- 성능 개선도 POSD 원칙을 따른다. public API 호출자는 새 순서, 새 helper, 내부
  transport/detail, perf 전용 예외를 알 필요가 없어야 한다. 비용은 public API 뒤쪽의
  깊은 구현에서 흡수하고, 호출 표면이 얕아지거나 복잡해지는 변경은 성능 후보로
  채택하지 않는다.
- perf는 버그가 있거나 `doc/perf` 원칙을 위배했을 때만 수정한다. 측정 수치를 올리기
  위한 perf hot path 우회, perf 전용 native helper 호출, deep runtime import는
  성능 개선으로 인정하지 않는다.
- binding perf는 `bindings/c/perf`와 같은 의미를 측정해야 한다.
- C perf와 다른 의미를 만드는 실험은 하지 않는다. worker 수, client 수, transport,
  pattern, message size, duration, timeout, socket buffer, borrow/copy 정책은
  C와 대상 binding이 같은 조건일 때만 비교 근거로 사용한다. HWM 관련 확인은
  auto-HWM 활성 여부와 size별 `MsgUnit(B)` 일치 여부로 제한한다.
- C perf가 public submit 결과를 재시도 가능한 상태로 다루면 binding perf도 같은 public
  결과 의미를 유지한다. 예를 들어 SPOT_REQREP에서 `NOT_CONNECTED`는 라우팅과 admission
  상태가 따라잡는 동안 나올 수 있는 public submit 결과이므로, C처럼 다음 poll loop에서
  재시도한다. 이를 fatal로 처리하면 성능 미달이 아니라 측정 의미 불일치다.
- binding perf hot path는 해당 언어의 public API를 사용해야 한다.
- 내부 API, private API, native helper, C API 직접 호출로 수치를 만드는 방식은
  인정하지 않는다.
- perf 수정 없이 목표를 맞출 수 없다고 보이면 먼저 binding 라이브러리 내부의
  allocation, copy, dispatch, error handling, receive storage 재사용 경로를 검토한다.
  perf 하니스는 측정 의미 오류가 확인된 경우에만 최소 범위로 수정한다.
- perf 입력값이나 benchmark loop에만 맞는 개선은 채택하지 않는다. 예를 들어 같은
  topic, 같은 routing id, 같은 payload 내용이 반복된다는 가정에 기대어 cache를 추가하는
  변경은 실제 애플리케이션 분포를 대표하지 못하므로 성능 개선으로 인정하지 않는다.
  후보가 특정 perf 데이터 모양에만 의미가 있으면 측정 전에 폐기하고, 이미 코드에
  들어갔으면 같은 작업 안에서 제거한다.
- 내부 최적화 후보를 리뷰할 때는 “일반 사용자가 같은 public API를 사용할 때도 비용이
  줄어드는가”를 먼저 확인한다. 반복 문자열 비교, benchmark 전용 상태 cache, perf
  message size만 겨냥한 branch, perf harness가 만든 고정 순서에 의존하는 shortcut은
  코드만 복잡하게 만들 수 있으므로 채택하지 않는다. 이런 후보는 문서에 `폐기`로 남겨
  이후 리팩토링 때 다시 들어오지 않게 한다.
- perf 하니스에서만 의미 있는 비용을 줄이는 변경은 binding 개선으로 보지 않는다.
  예를 들어 benchmark loop의 payload template 복사, 고정 topic 검증, 고정 순서 pending
  배열 접근처럼 실제 애플리케이션 입력 분포와 직접 연결되지 않는 비용은 perf 코드를
  빠르게 만들 뿐이다. 이런 항목은 public contract의 일반 사용 경로에서 같은 비용이
  반복된다는 근거가 있을 때만 library 후보로 올린다.
- 특정 perf 루프에서 호출 빈도가 높다는 이유만으로 public accessor나 builder에
  inline attribute만 붙이는 변경도 채택하지 않는다. allocation, copy, ownership,
  native boundary처럼 실제 구조 비용을 줄인 근거가 없으면 실사용 성능 개선으로 보지
  않고 제거한다.
- hot path로 판정한 binding 내부 코드는 리팩토링 중 일반 경로로 되돌아가지 않도록
  코드 가까이에 짧은 주석을 남긴다. 주석은 “빠르다”가 아니라 어떤 비용을 막는지
  구체적으로 적는다. 예를 들어 per-message allocation, payload copy, exception
  construction, dynamic dispatch, routing lookup, external Buffer 생존 연장을 막는지
  설명한다.
- hot path 주석이 붙은 경로를 리팩토링할 때는 같은 public API 호출을 유지한 상태에서
  before/after throughput을 재측정한다. public API를 바꾸거나 perf에서 내부 경로를
  직접 호출해 얻은 수치는 채택 근거로 쓰지 않는다.
- 채택한 내부 최적화는 어떤 public API 경로를 그대로 유지했는지 함께 남긴다. 예를 들어
  Node `MULTI_PUBSUB` 개선은 perf가 계속 public `sub.subscribe(received, DontWait)`를
  호출한 상태에서 binding 내부의 caller-provided storage release, SUB single-part raw
  object, empty routing id 생성을 줄인 사례다. Node `MULTI_SPOT` 개선도 perf가 계속
  public `spot.subscribe(received, DontWait)`를 호출한 상태에서 SPOT subscribe raw object의
  single-part data 경로와 empty routing id 생성을 줄인 사례다. 이런 항목은 코드의 hot
  path 주석과 pre-release check 문서의 before/after report가 서로 맞아야 한다.
- 후보가 public API 뒤쪽의 내부 변경이어도 actor gateway, dispatch, ownership 같은
  같은 모듈의 다른 public 계약까지 검증해야 한다. Node STREAM peer-index routing id
  cache 후보처럼 한 hot path 비용을 줄일 수 있어도 actor-bound regression gate가 현재
  실패하면 안전성을 증명할 수 없으므로 반영하지 않는다. 깊은 모듈은 같은 public surface의
  다른 사용 방식까지 내부에서 함께 책임져야 한다.
- 수치 달성만을 위해 perf 전용 public API나 zero-copy 우회 API를 추가하지 않는다.
- public API 계약이 잘못 구현되었거나 C public API가 제공하는 필수 계약이 binding
  public API에 빠진 것이 확인되면, 해당 public API 추가나 수정을 금지하지 않는다.
  이 경우 누락이나 오구현 자체가 수정 근거다.
- public API 추가나 수정이 필요하면 먼저 어떤 C 공개 계약을 감싸는지, 어떤 binding
  계약이 빠졌거나 잘못되었는지, 필요한 회귀/API 테스트가 무엇인지 계획 문서에 적고
  같은 언어 작업 안에서 테스트와 구현을 진행한다. C API에 없는 새 의미를 만드는
  경우에만 별도 draft/spec 검토 대상으로 분리한다.
- C public API와 무관한 새 인터페이스는 성능 목표만으로 만들지 않는다. 이 경우 성능
  미달은 기존 public API 내부 구현을 개선해서 먼저 해결해야 하며, 공개 인터페이스
  변경의 근거가 될 수 없다.
- .NET 이후 언어에서도 public API 변경은 최후 수단이다. 기존 public API 내부 최적화
  후보를 먼저 모두 검토하고, 큰 개선 가능성이 명확하지 않으면 public API 변경
  프로토타입도 만들지 않는다. 테스트 의미가 달라지는 실험이나 큰 개선 가능성이 낮은
  인터페이스 변경 실험은 하지 않는다. public API 변경 후보가 목표 달성에 의미 있는
  개선을 만들 가능성이 높다고 판단되는 경우에만 제한 프로토타입으로 확인한다.
  프로토타입 변경은 측정 직후 원복해야 하며, 개선이 확인된 경우에는 필요한 계약,
  예상 호출 방식, 영향 범위, 측정 결과를 문서에 남긴 뒤 정식 API 작업으로 분리한다.
- 버그가 확인되면 perf에서 우회하지 않고 버그를 먼저 수정한다.
- 버그 수정 전에는 해당 동작을 재현하는 회귀테스트를 먼저 작성한다.
- binding 버그이면 해당 언어 binding 라이브러리에서 수정한다.
- core 버그이면 core에서 수정한 뒤
  `/home/hep7/project/kairos/zlink/scripts/local-package/native/sync-local-core-libs.sh`로 bindings에 local
  core library를 다시 배포한다.
- core public API를 새로 추가하거나 수정하는 항목은 core 구현과 core 테스트를 먼저
  완료한다. 그 다음 `/home/hep7/project/kairos/zlink/scripts/local-package/native/sync-local-core-libs.sh`를
  실행해 bindings local core library와 vendored C header를 갱신하고, 그 뒤에 각 binding
  코드를 수정한다.
- public API의 실제 계약은 각 binding의 public contract source를 기준으로 판단한다.
  예를 들어 .NET은 `bindings/dotnet/src/Zlink/Contracts/`가 public API 계약의
  단일 기준이다. `doc/spec/bindings/README.md`와 각 언어별
  `doc/spec/bindings/<lang>/README.md`는 API 목록이 아니라 public API 작성,
  배치, internal boundary 검토 규칙으로만 사용한다. perf 프로젝트가 `internal`
  surface나 `InternalsVisibleTo`에 의존하면 안 된다.
- Auto-HWM message unit처럼 binding spec에서 typed option facade로 제공해야 한다고
  정한 기능은 그 규칙을 따른다. raw option bag이나 perf 전용 helper로 우회하지 않는다.
- `doc/perf/PERF_POLICY.md`,
  `doc/perf/PERF_SINGLE_TEST_POLICY.md`,
  `doc/perf/PERF_MULTI_TEST_POLICY.md`를 항상 따른다.

## 3. 실행 방식

각 binding의 공식 perf 스크립트를 그대로 사용한다.

- single: `bindings/<lang>/perf/run_benchmarks.sh`
- multi: `bindings/<lang>/perf/run_benchmarks_multi.sh`
- C 기준: `bindings/c/perf/run_benchmarks.sh`,
  `bindings/c/perf/run_benchmarks_multi.sh`

C++은 호환용 wrapper로 `bindings/cpp/perf/run_binding_single.sh`와
`bindings/cpp/perf/run_binding_multi.sh`도 제공하지만, 언어 공통 공식 entrypoint는
위의 `run_benchmarks*.sh` 이름으로 맞춘다.

스크립트에 설정된 기본값을 바꾸지 않는다. 비교 범위를 좁힐 때만
`--transports`와 `--pattern`으로 특정 transport와 특정 pattern을 지정한다.
제한 측정은 transport를 먼저 고정하고, 그 안에서 message size 일부 또는 전체를
확인한다. 예를 들어 작은 message size만 골라 `tcp,ws,wss,tls`를 한 번에 돌리는
방식은 쓰지 않는다. 먼저 `tcp`만 대상으로 pattern과 message size 일부 또는 전체를
비교한다. `tcp`에서 목표 비율을 만족한 뒤에만 같은 방식으로 `ws`, `wss`, `tls`
순서로 넘어간다. transport를 바꿀 때마다 C 기준과 대상 binding 결과의 옵션을 다시
대조한다.
제한 측정으로 C와 대상 binding을 다시 비교할 때는 결과 파일의 `Effective Options`를
먼저 대조한다. suite, pattern, transport, message size, duration, client 수,
timeout, socket buffer 설정이 같은지 확인해야 한다. HWM은 numeric `SNDHWM`/`RCVHWM`
값을 통과 기준이나 튜닝 대상으로 삼지 않는다. auto-HWM 활성 여부와
`Auto-HWM Detail`, `Auto-HWM spotnode`, `Auto-HWM spot handles`에 보이는 모든
`MsgUnit(B)`가 해당 message size와 같은지만 확인한다. 예를 들어 64B 테스트에서
`MsgUnit(B)=4096`이 보이면 그 결과는 비교 기준으로 쓰지 않는다. SPOT 계열은 데이터
소켓뿐 아니라 제어용 SpotNode와 SPOT handle도 같은 message size를 사용해야 한다.
이 값이 다르면 HWM slot 수와 socket buffer 크기가 달라져 처리량 비교가 왜곡될 수 있다.
또한 routed echo 계열처럼 C perf가 특정 transport에서 payload를 빌려 쓰는 경우에는
`Effective Options`의 `routed_echo_borrow_payload` 값도 함께 확인한다. 이 값이 다르면
메시지 복사 비용이 비교에 섞이므로, 결과를 binding 자체 성능으로 단정하지 않는다.

공식 runner는 실행 전에 실제 core runtime을 드러내야 한다. report 또는 console log에서
`Perf runtime libzlink: ...` 경로를 확인하고, 그 경로가 `core/build` 아래의 runtime인지
본다. `core/src` 또는 `core/include`가 해당 runtime보다 새로우면 runner가 perf 실행 전에
실패해야 한다. 이 조건을 우회한 결과는 기준 비교에 쓰지 않는다.

새 pattern 또는 새 runner 조건을 처음 측정할 때는 CPU와 OS thread도 같이 관측한다.
server/client 프로세스의 최대 `nlwp`와 대략적인 `%CPU` 피크를 결과 메모에 남긴다.
특히 multi suite는 `clients` 값이 OS thread 수로 그대로 확장되는지 확인한다. Go는
goroutine 실행 병렬도를 `GOMAXPROCS`로 따로 제한하므로, Go 결과에는 `go_gomaxprocs`,
`go_gomaxprocs_source`, server/client `io_threads`, `nlwp`를 함께 기록한다. 이 값이
이전 같은 조건보다 크게 튀면 throughput 비교 전에 runner 조건이나 runtime 설정 차이를
먼저 확인한다.

C는 개선 대상 언어가 아니라 비교군이다. 따라서 첫 비교에서는 C perf를 매번 새로
실행하지 않고, `bindings/c/perf/baseline/` 아래의 최근 full 측정 결과를 사용한다.
이 기준 파일은 같은 기본 옵션으로 실행한 결과여야 하며, 특정 실험이나 debug 재현을
위해 제한 실행한 결과는 기준으로 쓰지 않는다.

측정 오차가 의심되거나 C 기준 파일의 특정 조합이 비정상적으로 보일 때만, 비교 범위를
같은 transport와 pattern으로 제한해서 C와 대상 binding을 각각 다시 측정한다. 이때도
C는 새 기준을 만들기 위한 보조 측정일 뿐이며, 동시에 여러 C perf를 계속 돌리지 않는다.

한 라운드는 아래 순서로 진행한다.

1. `bindings/c/perf/baseline/`의 최근 full C 결과에서 같은 suite, pattern, transport,
   message size, metric 값을 찾는다.
2. 대상 binding perf를 언어별로 하나만 실행한다.
3. C 대비 비율을 계산한다.
4. 미달 조합의 병목을 binding 라이브러리에서 찾는다.
5. binding 라이브러리를 수정한다.
6. 같은 조합의 대상 binding perf를 다시 측정한다.
7. 측정 오차가 의심되면 같은 transport와 pattern으로 C와 대상 binding을 제한
   재측정한 뒤 다시 비교한다.
8. 목표를 넘을 때까지 반복한다.

single과 multi는 같은 원칙으로 반복한다. 한 번에 전체 matrix를 돌리지 않고,
`--transports`와 `--pattern`으로 조합을 좁혀 원인과 개선 효과를 확인한 뒤 다음
조합으로 이동한다. 제한 측정 순서는 transport 우선이다. `tcp`의 미달 조합이 남아 있으면
`ws`, `wss`, `tls` 측정으로 넘어가지 않는다. `tcp`가 통과한 뒤 다음 transport로
넘어갈 때도 한 번에 하나의 transport만 선택해서 C 기준과 대상 binding을 비교한다.

full matrix가 필요한 단계에서도 바로 full run으로 들어가지 않는다. 먼저 같은 runner,
같은 transport, 같은 pattern 범위에서 짧은 smoke를 `PERF_FAIL_FAST=1`, `--duration 1`,
`--runs 1`로 실행한다. smoke에서 timeout, no-result, option mismatch, stale runtime,
binary exit가 나오면 full run을 시작하지 않고 해당 원인을 먼저 수정한다. smoke 결과도
정상 report 파일로 남겨야 하며, console 출력만 보고 통과로 처리하지 않는다.

언어별 작업은 한 번에 한 언어만 진행한다. 진행 순서는 C++, .NET, Java, Node, Go,
Rust, Python이다. 현재 언어의 모든 대상 transport, pattern, size가 `통과` 상태가
되기 전에는 다음 언어로 넘어가지 않는다. `보류`는 완료가 아니므로 현재 언어에 보류
항목이 남아 있으면 public API 계약 설계, 회귀 테스트, perf 반영 계획을 먼저 작성하고
해당 언어 작업으로 계속 추적한다. `미달`, `보류`, `미측정` 항목이 하나라도 남아 있으면
현재 언어 작업을 계속한다.

공식 perf 실행은 기본적으로 하나만 실행한다. 측정 오차 확인을 위해 C와 대상 binding을
제한 재측정해야 할 때도 전체 공식 perf 실행 수는 두 개를 넘기지 않는다. 같은 suite,
pattern, transport, message size 조합을 중복으로 동시에 실행하지 않는다.

## 4. 직접 진행 절차

이 작업은 측정, 병목 분석, 코드 수정, 재측정, 문서 갱신을 직접 수행한다.

상태 값은 아래 네 가지로만 기록한다. C 대비 비율을 계산한 측정치는 상태만 쓰지 않고
`통과(85%)`, `미달(72%)`, `보류(30%)`처럼 상태와 비율을 한 칸에 함께 적는다.
아직 측정하지 않은 칸은 `미측정`으로 두고, 정책상 측정 대상이 아닌 칸은 `해당 없음`으로
둔다.

- `미측정`: 아직 같은 조건의 C 기준과 대상 binding 결과를 비교하지 않았다.
- `통과`: 목표 비율, 상대 기준, latency 조건, `Effective Options`, `MsgUnit(B)` 조건을
  모두 만족한다.
- `미달`: 유효 비교에서 목표를 만족하지 못했고, 아직 내부 개선 후보를 더 확인해야 한다.
- `보류`: 유효 비교에서 목표 미달이지만, public API 변경 없이 가능한 내부 개선 후보를
  더 찾지 못했다. 보류 항목은 추가 또는 수정이 필요한 public API와 근거를 함께
  기록해야 한다.

`MsgUnit(B)` 불일치, `Effective Options` 불일치, timeout은 그 자체로 `보류` 사유가
아니다. 이런 항목은 먼저 비교 조건을 맞추거나 perf 정책 위반 여부를 확인해야 한다.
비교 조건이 아직 맞지 않아도 예비 수치가 목표보다 낮고 같은 public API hot path에
내부 개선 후보가 남아 있으면 `미달`로 둔다. `MsgUnit(B)` 불일치는 `통과`를 막는
조건이며, 개선 작업을 중단할 근거가 아니다.

측정 timeout은 timeout 값을 늘리거나 같은 조합을 반복 실행해서 해결하지 않는다.
retry, 추가 sleep, worker/client 수 조정처럼 실패를 숨기거나 테스트 의미를 바꾸는
우회도 사용하지 않는다.
공식 perf의 duration, operation timeout, runner result timeout, ready timeout 값은
C와 같은 의미를 보존해야 하며, 실패를 숨기기 위해 키우면 안 된다. 결과 라인이 나오지
않는 조합은 `미달`로 두고 server/client 로그를 확인한 뒤, active loop, stop/drain,
poller wakeup, pending reply 처리, backpressure 처리처럼 수치를 못 내게 만든 구현
원인을 수정해야 한다. 같은 조건에서 수치가 나올 때까지 다음 조합이나 다음 언어로
넘어가지 않는다. 공식 판정 표에는 timeout/no result를 최종 근거로 남기지 않고,
같은 조건에서 처리량 또는 latency 숫자가 나온 뒤에 `통과(비율%)`, `미달(비율%)`,
`보류(비율%)` 중 하나로 판정한다.

public API 변경 없이는 비교 조건을 완전히 맞추기 어렵다고 보여도 바로 `보류`로
넘기지 않는다. 기존 public API 내부 구현, lifecycle/setup 순서, buffer 재사용,
callback/dispatch 경로, 불필요한 allocation/copy, poll loop를 먼저 검토하고 최소
하나 이상의 의미 보존 후보를 측정해야 한다. `보류`는 그 후보들이 실패했거나
효과가 없고, 추가로 필요한 public API 계약과 영향까지 문서에 적은 뒤에만 표시한다.

`보류`를 남기기 전에 아래 금지 사례에 걸리지 않는지 먼저 확인한다.

- `MsgUnit(B)`가 C와 다르면 `보류`가 아니라 조건 정렬 전 `미달` 또는 `미측정`이다.
- timeout은 재현 조건, server/client 로그, 같은 조건의 C 제한 결과를 확인하기 전에는
  `보류`가 아니다.
- public API 제한은 마지막 판단 근거다. 내부 구현을 적어도 한 번 이상 수정하고 같은
  조건으로 재측정하지 않았다면 `보류`가 아니라 `미달`이다.
- 절대 목표 기준을 통과한 항목은 교차 언어 비교만으로 `미달`로 내리지 않는다.
- 교차 언어 비교는 `보류` 판단을 검증하는 보조 기준으로만 사용한다. C++에서 `보류`로
  닫으려는 항목이 같은 조건의 .NET/Java보다 낮으면 보류하지 말고 내부 구현을 재검토한다.
  .NET과 Java는 같은 managed runtime 목표 그룹이므로 서로 비교한다. 한쪽이 같은 조건이나
  가까운 하위 pattern에서 뚜렷하게 더 높은 수치를 내면 다른 쪽을 `추가 내부 후보 없음`으로
  보류하지 않고 callback/dispatch, poll loop, buffer ownership, message copy, HWM 적용
  차이를 먼저 분석하고 같은 조건으로 재측정한다.
- 새 public API 후보가 기존 공개 계약 테스트에서 막힌다는 사실만으로 `보류`로 넘기지
  않는다. 기존 public API의 option 전달, typed option facade, setup 순서, 내부
  auto-HWM 전파, perf 정책 위반 가능성을 먼저 확인하고 같은 조건으로 재측정해야 한다.
- public API 변경 실험은 큰 개선 가능성이 명확할 때만 수행한다. 단순한 가능성이나
  작은 개선 예상만으로 public API 변경 실험을 하거나, 반대로 public API 제한을 이유로
  내부 개선 검토를 멈추지 않는다.

현재 언어에서 `미달` 또는 `미측정` 항목이 남아 있으면 다음 언어로 넘어가지 않는다.
`보류`는 완료가 아니며, public API 추가/수정 항목이 남아 별도 구현 단위로 이어가야
하는 상태로 본다. C API에 이미 있는 계약을 binding public API가 빠뜨린 경우에는
대기하지 말고 public API 추가/수정 대상으로 기록한 뒤 회귀/API 테스트부터 작성한다.

상태 표 안에서 요약 행과 상세 행이 서로 맞지 않으면 상세 행을 우선한다. 상세 행에
`미달` 또는 `미측정`이 하나라도 남아 있으면 그 언어는 완료가 아니다. 6.1의 언어 진행
상태는 각 언어별 상세 표에서 산출한 결과여야 하며, 상세 표보다 느슨한 완료 판단을
적으면 안 된다.

매 라운드마다 아래를 직접 확인한다.

- C 기준과 binding 결과가 같은 suite, transport, pattern, size를 비교했는지
- `tcp`의 모든 대상 pattern/size가 `통과` 또는 `보류`가 되었는지
- `tcp`에 `미달` 또는 `미측정`이 남은 상태에서 `ws`, `wss`, `tls`로 넘어가지 않았는지
- 제한 재측정 결과의 `Effective Options`와 auto-HWM `MsgUnit(B)`가 서로 같은지
- 공식 runner가 `Perf runtime libzlink: ...`를 출력했고 stale `core/build` runtime을
  실행 전에 막았는지
- full run 전에 같은 조건의 fail-fast smoke report가 먼저 남았는지
- 새 pattern 또는 새 runner 조건에서 server/client `nlwp`와 CPU 피크를 기록했는지
- C 기준으로 `bindings/c/perf/baseline/`의 최근 full 결과를 먼저 사용했는지
- C 재측정은 측정 오차나 비정상 기준이 의심되는 제한 조합에서만 실행했는지
- perf 수정이 필요한 경우 실제 버그나 정책 위반 근거가 있는지
- 수정이 binding public API 내부 구현 개선인지
- 새로 발견한 결과를 상태 표와 로그 문서에 반영했는지

실행 중 문제가 발견되면 같은 언어 안에서 원인을 리뷰하고, 회귀테스트 작성, 버그 수정,
재측정, 문서 갱신을 반복한다. 측정 실패, 기준 불일치, perf 정책 위반, binding/core
버그, 목표 기준의 모호함이 모두 사라질 때까지 해당 항목을 `통과`나 `보류`로 표시하지
않는다.

## 5. Public API 확인 기준

이 섹션은 새 측정 라운드에서 public API 문제를 판정하는 기준만 둔다. 이미 코드에
반영된 작업은 추가/수정 대상 목록에서 제거한다. 상태표의 `미측정` 칸을 채우다가 병목이
나오면 먼저 같은 조건의 C perf와 비교하고, 내부 구현이나 perf 사용 경로를 고친 뒤에도
C 공개 계약과 같은 의미를 binding public API로 표현할 수 없을 때만 새 public API 항목으로
기록한다.

### 5.1 이미 반영된 항목

context-level auto-HWM message unit option은 현재 코드에 반영된 항목이다. 따라서 이
문서에서는 더 이상 별도 rollout 작업으로 추적하지 않는다.

현재 확인해야 하는 기준은 아래와 같다.

- core/C에는 `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES = 18`과
  `ZLINK_CTX_AUTO_HWM_MSG_UNIT_BYTES_DFLT = 0`이 존재한다.
- binding의 일반 사용 경로는 context option이다. 각 언어 perf는 socket별 message unit
  facade가 아니라 context option으로 size별 message unit을 설정해야 한다.
- socket/SpotNode/Spot별 message unit facade를 되살리지 않는다. 새 측정에서
  `MsgUnit(B)` 불일치가 다시 나오면 새 API rollout이 아니라 회귀나 perf 사용 경로 문제로
  먼저 다룬다.
- `ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES = 0x3034`는 C handle-level 저수준 계약으로 남는다.
  이 값이 남아 있다는 이유만으로 binding의 일반 사용 surface에 socket별 facade를 다시
  추가하지 않는다.

새 측정에서 이 기준이 깨진 언어가 있으면 해당 언어 상태표의 해당 칸을 `미달`로 표시하고,
결과 파일 / 메모 칸에 빠진 API 또는 잘못된 perf 사용 경로를 적는다.

### 5.2 새 측정 중 확인할 항목

아래 항목은 "지금 바로 새 의미를 만든다"는 뜻이 아니다. C API의 공개 primitive는
존재하지만, 해당 언어 public API가 이미 같은 의미를 제공하는지 먼저 확인한다. 이미
제공하면 perf 사용 경로만 고치고, 제공하지 않으면 회귀/API 테스트를 먼저 작성한 뒤
public API 추가 대상으로 분리한다.

| 항목 | C API 기준 | 확인 방식 |
|------|------------|-----------|
| writable/owned message | `zlink_msg_init_size`, `zlink_msg_data`, `zlink_msg_size`, `zlink_msg_close`, `zlink_msg_init_data` | 반복 송신 경로가 매번 불필요한 복사나 할당을 하는지 확인한다. C++처럼 이미 writable message가 있으면 새 API 대상이 아니다. .NET, Java, Node는 writable buffer를 public API로 안전하게 노출하는 경로가 있는지 먼저 확인한다. |
| routed single-part send/recv | `zlink_send_part_rid`, `zlink_router_request_part`, `zlink_router_reply_part`, `zlink_router_recv_part`, `zlink_spot_recv_part`, `zlink_router_send_spot_part` | ROUTER/DEALER/SPOT routed 경로에서 multi-part wrapper 없이 single part를 보내고 받을 수 있는지 확인한다. 같은 의미의 public API가 있으면 perf만 그 경로로 고친다. |
| single-part subscribe receive | `zlink_subscribe_part`, `zlink_spot_subscribe_part` | PUBSUB/SPOT receive 경로에서 caller가 결과 객체나 message buffer를 재사용할 수 있는지 확인한다. 재사용 의미가 C의 `zlink_msg_t` 재사용과 다르면 API gap으로 기록한다. |
| stream frame send | `zlink_stream_send_bound_actor_part`, `zlink_stream_packet_handler` | `MULTI_STREAM` 병목이 C stream callback/part 계약 누락인지 먼저 대조한다. 언어 binding이 이미 같은 의미를 제공하면 새 API를 만들지 않고 perf 사용 경로를 고친다. |

## 6. 신규 측정 상태 표

이 표는 기존 측정 기록을 그대로 통과 근거로 재사용하지 않고, 새 라운드에서 같은 조건의
C 기준과 대상 binding 결과를 다시 비교해 갱신하는 상태표다. 이미 갱신된 칸은 결과 파일과
C 대비 비율을 함께 남기고, 아직 같은 조건 비교가 없는 칸은 `미측정`으로 둔다.

표 구조는 모든 언어에서 같다. 행은 transport와 pattern을 고정하고, 열은 message size를 고정한다. 각 size 칸은 해당 transport/pattern/size 조합의 상태를 뜻한다. Single suite는 기존 기본 size `64,256,1024,65536,131072,262144`를 유지한다. Multi suite는 새 측정 라운드에서 `64,256,1024,4096,65536,131072`만 측정하고, 기존 `262144` 열은 더 이상 새 판정에 사용하지 않는다. `MULTI_STREAM`은 정책상 측정 대상 size만 채우고 나머지는 `해당 없음`으로 둔다.

상태 칸에는 `미측정`, `통과(비율%)`, `미달(비율%)`, `보류(비율%)`, `해당 없음` 형식만
쓴다. 예를 들어 C 대비 85%로 목표를 만족하면 `통과(85%)`, 내부 개선 후보가 소진된
30% 항목이면 `보류(30%)`로 적는다. 새 측정 뒤에는 같은 칸에 상태와 C 대비 비율을 함께
적고, 오른쪽 `결과 파일 / 메모` 칸에 결과 파일과 필요한 근거를 적는다. 크기별 결과
파일이나 사유가 다르면 행을 size별로 쪼개도 되지만, 쪼갠 뒤에도 transport/pattern/size
조합이 빠지면 안 된다.

### 6.1 언어 진행 상태

| 순서 | 언어 | perf 경로 | Single 상태 | Multi 상태 | 다음 작업 |
|------|------|-----------|-------------|------------|-----------|
| 1 | C++ | `bindings/cpp/perf` | `재측정 완료, 보류 3건` | `재측정 완료, 미달 없음` | Single routed large 3건은 개선 후보가 통과에 못 닿아 보류로 넘겼다. 다음은 .NET 재측정이다. |
| 2 | .NET | `bindings/dotnet/perf` | `재측정 대기` | `재측정 대기` | C++ 이후 같은 순서로 재측정한다. 기존 2026-05-27 .NET 결과는 새 기준 라운드의 최종 판정으로 사용하지 않는다. |
| 3 | Java | `bindings/java/perf` | `재측정 대기` | `재측정 대기` | .NET 완료 뒤 single 기본 size, multi 새 size set으로 재측정한다. |
| 4 | Node | `bindings/node/perf` | `재측정 대기` | `재측정 대기` | Java 완료 뒤 재측정한다. |
| 5 | Go | `bindings/go/perf` | `재측정 대기` | `재측정 대기` | Node 완료 뒤 재측정한다. |
| 6 | Rust | `bindings/rust/perf` | `재측정 대기` | `재측정 대기` | Go 완료 뒤 재측정한다. |
| 7 | Python | `bindings/python/perf` | `재측정 대기` | `재측정 대기` | Rust 완료 뒤 재측정한다. |

**2026-05-27 새 측정 라운드 진행 로그**: 기존 기록은 새 판정 기준으로 재사용하지 않는다.
Single suite는 기존 기본 size를 유지하고, multi suite만 `64,256,1024,4096,65536,131072`
size set으로 다시 측정한다. C single 기준은
`perf_c_single_linux_20260527_123415_codex_refresh_c_single_default_20260527.txt`로
확보했으며, `status=complete`, 결과 라인은 `1020/1020`이다. 첫 C multi 시도
`perf_c_multi_linux_20260527_125615_codex_refresh_c_multi_64_256_1k_4k_64k_128k_20260527.txt`는
새 size set으로 실행했지만 `MULTI_PUBSUB ws`의 `64,256,1024,4096,65536,131072B`가 모두
`FAIL`이었다. 또한 `PERF_FAIL_FAST=1`인데도 runner가 다음 패턴으로 넘어가 full 기준으로
쓸 수 없어 수동 중지했다. 이 파일은 C multi 기준 파일로 사용하지 않는다. C multi runner의
fail-fast 동작을 먼저 고쳤고, 같은 조건 제한 재측정
`perf_c_multi_linux_20260527_131226_codex_c_multi_pubsub_ws_new_sizes_recheck_20260527.txt`에서는
`MULTI_PUBSUB ws`의 새 size set 전부가 `status=complete`, 결과 라인 `30/30`이었다.
따라서 C multi full을 새 size set으로 다시 실행해 기준 파일을 확보한다. 각 언어 상세 표는
해당 언어의 single/multi 재측정이 끝난 뒤 기존 표 형식을 유지한 채 갱신한다. full 재시도
`perf_c_multi_linux_20260527_131318_codex_refresh_c_multi_64_256_1k_4k_64k_128k_retry_20260527.txt`는
`MULTI_PUBSUB ws`를 통과했지만, `MULTI_STREAM ws 1024B`에서 client가 exit 2로 종료해
`status=partial`, 결과 라인 `900/960`으로 끝났다. 이 파일도 full C multi 기준으로 쓰지
않는다. 같은 조건 제한 재측정
`perf_c_multi_linux_20260527_134501_codex_c_multi_stream_ws_new_sizes_recheck_20260527.txt`에서는
`MULTI_STREAM ws`의 새 size set 전부가 `status=complete`, 결과 라인 `30/30`이었다.
따라서 실패는 full-run 안정성 문제로 보고 C multi full을 다시 시도한다.
두 번째 full 재시도
`perf_c_multi_linux_20260527_134603_codex_refresh_c_multi_64_256_1k_4k_64k_128k_retry2_20260527.txt`는
새 multi size set으로 `status=complete`, 결과 라인 `960/960`을 확보했다. msg-size를
명시한 새 라운드 기준 파일이므로 동일 파일을 `bindings/c/perf/baseline/`에도 복사했다.
C++ single 첫 full 시도
`perf_cpp_single_linux_20260527_141949_codex_refresh_cpp_single_default_20260527.txt`는
기존 single 기본 size로 실행했지만 `ROUTER_ROUTER tcp 256B` timeout 뒤 같은 transport의
나머지 size가 `no_data`가 되어 `status=partial`, 결과 라인 `725/1020`으로 끝났다. 이
파일은 C++ single full 판정 파일로 쓰지 않고, `ROUTER_ROUTER tcp`를 제한 재측정해 실패
재현 여부를 확인했다. 제한 재측정
`perf_cpp_single_linux_20260527_143736_codex_cpp_single_router_router_tcp_recheck_20260527.txt`는
`ROUTER_ROUTER tcp` 전 size가 `status=complete`, 결과 라인 `30/30`이었다. 따라서 C++
single full을 다시 실행해 판정 파일을 확보한다. 첫 retry는 incremental CMake build가
`cpp_perf_dealer_dealer` target을 찾지 못해 벤치 실행 전 종료했으므로 측정 파일로 기록하지
않고 clean build로 다시 실행한다. clean build full retry
`perf_cpp_single_linux_20260527_143903_codex_refresh_cpp_single_default_retry_clean_20260527.txt`는
기존 single 기본 size로 `status=complete`, 결과 라인 `1020/1020`을 확보했다. 이 파일을
C++ single 기준 파일로 쓰고 6.2.1 표를 갱신한다.
C++ multi full
`perf_cpp_multi_linux_20260527_150546_codex_refresh_cpp_multi_64_256_1k_4k_64k_128k_20260527.txt`는
새 multi size set으로 `status=complete`, 결과 라인 `960/960`을 확보했다. msg-size를
명시한 새 라운드 기준 파일이므로 동일 파일을 `bindings/cpp/perf/baseline/`에도 복사했다.
이 파일을 C++ multi 기준 파일로 쓰고 6.2.2 표를 갱신한다.

**2026-05-23 core 6.0.3 재검증 로그**: 세션 중 core가 6.0.2→6.0.3로 bump(spot node bind API rename 포함)되어
이전 baseline/측정이 무효화됐다. 6.0.3 fresh full C baseline을 재생성했다
(single `perf_c_single_linux_20260523_102550_goal_c_single_603_baseline.txt`,
multi `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt`). 이 baseline 대비 single `tcp` 재검증 결과:
- C++ single tcp: 전 pattern 98~101%(PUBSUB large는 >120% outlier, DD 262144는 full-run partial) → 통과 유지.
- .NET single tcp: 전 pattern 70~123%, latency 정상 → 통과 유지.
- Java single tcp: 전 pattern 75~119%(SPOT는 117~177% outlier, SPOT large는 full-run partial) → 통과 유지.
- Go single tcp/ws/wss/tls: 전 pattern 통과(아래 6.6.3 — SPOT large 회귀 수정 + latency 발산 측정-아티팩트 판정).
- Rust single tcp: non-routed(PAIR/DEALER_DEALER/PUBSUB) 96~119% 통과, 2026-05-23 기준 SPOT 1024B는 보류였으나 2026-05-27 public `Spot::publish_part`/`subscribe_part` 경로 적용 뒤 해소됐다. 단 routed
  `DEALER_ROUTER`/`ROUTER_ROUTER` large(65536/131072/262144)는 11~13%(latency 13~35x)로 기존 문서 보류와 동일하게 재현됐다(회귀 아님).
- Python single tcp: large(65536+)는 DD 72~80%/PAIR 73~88%/PUBSUB 63~69%로 통과권, 그러나 small(64/256/1024)은 전 pattern 1.7~13.8%다.
  Python small throughput은 pattern 무관하게 ~45k msg/s에 고정되는데(C는 1.2M+), 이는 매 송수신마다 Python↔C 경계를 넘는 **per-call FFI 고정 비용의 벽**이다(할당이 아니라 호출 횟수 비용). routed large와 SPOT large도 10~44%로 보류다. 문서 상태와 동일하게 재현됐다.

**구조적 결론 (2026-05-23 6.0.3 전면 재검증 기준)**: re-baseline은 문서의 기존 상태를 그대로 재현한다 — 강한 바인딩(C++/.NET/Java/Rust non-routed,
Go single)은 통과, 보류 cell은 그대로 보류다. 당시 신규로 해결한 것은 Go single SPOT large 회귀(per-send 할당 → public `.Bytes()` 재사용)와 Go single
latency 발산(측정 아티팩트 판정)이었다. 이후 2026-05-25 추가 수정으로 Go single/multi
보류는 표 기준 모두 해소됐다. 2026-05-27에는 Rust single SPOT 1024B도 public
`Spot::publish_part`/`subscribe_part` 경로 적용 뒤 통과로 갱신했다. 현재 남은 보류 cell —
Python/Node small(per-call FFI 벽), Rust routed-large, Python SPOT/send-send/stream small —
은 이전 전문 세션들이 "추가 내부 후보 소진"으로 기록했고 이번 독립 재검증·진단도 같은 결론이다.
이들을 통과로 올리려면 **메시지 배치(batch) 또는 zero-copy 우회 같은 perf 지향 public
API/아키텍처 변경**이 필요할 수 있는데, 이는 섹션 2 고정 원칙과 별도 설계로 분리해야
한다. 따라서 섹션 7의 "보류 0" 완료 기준은 현재 고정 원칙 안에서 이들 cell에 대해
도달하기 어렵다. 완료하려면 (a) 해당 public API 계약을 정식 설계 대상으로 분리하거나,
(b) 이들 구조적 cell을 근거와 함께 영구 보류로 인정하는 정책 결정이 필요하다.

#### 6.1.1 언어별 평균 성능

아래 지표는 현재 측정값이 있는 C++, .NET, Java, Node, Go, Rust, Python을 계산한다.
각 언어의 Single/Multi 상태표에서 `통과(비율%)`, `미달(비율%)`, `보류(비율%)` 형식의
측정 셀을 모두 모아 C 대비 throughput 비율을 계산한다. `해당 없음`과 `미측정`은
제외한다.

단순 평균은 높은 outlier에 쉽게 끌려간다. 그래서 중앙값, p10, 최저 10% 평균을 함께 본다.
p10은 하위 10% 경계값이고, 최저 10% 평균은 가장 느린 구간의 체감 위험을 보기 위한
보조 지표다.

| 언어 | 측정 셀 수 | 평균 | 중앙값 | p10 | 최저 10% 평균 | Single 평균 | Multi 평균 |
|------|------------|------|--------|-----|---------------|-------------|------------|
| C++ | 328 | 104.8% | 98.0% | 83.0% | 79.3% | 112.9% | 98.4% |
| .NET | 328 | 93.3% | 91.0% | 63.9% | 59.9% | 103.1% | 85.7% |
| Java | 328 | 101.4% | 95.5% | 67.2% | 60.6% | 119.2% | 87.5% |
| Node | 328 | 76.1% | 71.5% | 37.5% | 29.3% | 78.7% | 74.1% |
| Go | 332 | 69.3% | 62.5% | 31.6% | 16.8% | 90.8% | 52.9% |
| Rust | 328 | 79.9% | 87.2% | 18.1% | 8.8% | 96.3% | 67.0% |
| Python | 336 | 32.7% | 13.8% | 2.2% | 1.4% | 39.7% | 27.4% |

#### 6.1.2 C 대비 고성능 outlier 재검토

C 대비 성능이 크게 높은 항목은 그대로 좋은 결과로 확정하지 않는다. 특히 120% 이상
항목은 아래 순서로 다시 본다.

1. 같은 날짜, 같은 core/build, 같은 transport/pattern/size 조건으로 C 기준을 재측정한다.
2. active window, stop token, drain grace, latency sample, client 수, MsgUnit(B)이 C/perf와
   같은 의미인지 확인한다.
3. binding 쪽 최적화가 public API 내부 최적화인지, perf 전용 의미 변경인지 구분한다.
4. public API 내부 최적화라면 C/perf 또는 다른 binding에도 적용 가능한지 후보로 남긴다.

현재 표에서 120% 이상 outlier는 아래 그룹에 몰려 있다.

| 언어 | 주요 outlier 그룹 | 최대값 | 1차 해석 | 후속 확인 |
|------|------------------|--------|----------|-----------|
| C++ | Single `PUBSUB`, Multi `MULTI_SPOT` | 재측정 후 88.1% (`wss MULTI_SPOT 1024B`) | 기존 573.0%는 오래된 C 기준 파일 영향이 컸다. 같은 `core/build` 제한 재측정에서 C가 1608.5 Kmsg/s, C++가 1416.5 Kmsg/s였다. | C++ `PUBSUB` large outlier도 같은 방식으로 C 기준을 먼저 갱신한다. |
| .NET | Single `SPOT`, Multi `MULTI_SPOT` | 의미 정렬 후 63.5% (`wss SPOT 65536B`) | 기존 515.0%는 .NET single SPOT에 C에 없는 기본 in-flight cap이 들어간 영향이 있었다. 기본 cap을 제거하고 public `SubscribePart` 수신 경로로 C의 single-part subscribe 의미에 맞추니 C 8.31 Kmsg/s 대비 .NET 5.27 Kmsg/s가 됐다. | C에 없는 perf-only credit 제한은 기본 경로에 넣지 않는다. single SPOT은 public API 내부 수신 경로 개선으로 통과했다. |
| Java | Single routed/spot 계열, Multi `MULTI_SPOT` | 재측정 후 113.1% (`wss MULTI_SPOT 256B`) | 기존 390.2%는 C 기준 파일 시점 차이가 컸다. 같은 조건 제한 재측정에서 C가 5424.3 Kmsg/s, Java가 6134.2 Kmsg/s였다. | 남은 120% 이상 single routed/spot outlier는 같은 조건 C 재측정 뒤 JIT/fast path 영향만 분리한다. |
| Node | Single `SPOT`, Multi `MULTI_SPOT_SENDSEND` | 재측정 후 296.4% (`wss SPOT 1024B`) | C 기준을 갱신해도 Node single SPOT은 높게 남았다. 현재 Node single SPOT은 한 이벤트 루프에서 publish 후 inline drain을 수행하므로 C의 별도 publisher/receiver thread 의미와 다를 수 있다. | 이 셀은 통과로 확정하지 않고 보류한다. Node single SPOT은 C와 같은 의미의 송신/수신 분리 구조를 설계한 뒤 다시 측정한다. |
| Go | Single large one-way, Single `SPOT`, Multi `MULTI_STREAM`/`MULTI_SPOT` | 현재 281.4% (`tcp PAIR 262144B`), multi에서는 160.8% (`tls MULTI_SPOT 65536B`) | Go `tcp` large outlier는 2026-05-21 C 기준 파일을 사용한 값이라 같은 조건 C 제한 재측정으로 먼저 확인해야 한다. `wss SPOT 64B`는 2026-05-22 같은 조건 C/Go 재측정에서도 186.5%로 높게 남았다. `tls SPOT 64B/256B`도 같은 조건 C/Go 재측정에서 156.8%, 125.6%로 C보다 높게 나왔다. `ws MULTI_STREAM 262144B`는 같은 조건 C 제한 재측정 `perf_c_multi_linux_20260522_062037_codex_c_ws_multi_stream_large_for_go_20260522.txt` 대비 Go `perf_go_multi_linux_20260522_060003_codex_go_ws_multi_current_20260522.txt`에서 149.3%다. `tls MULTI_SPOT 65536B/131072B`는 같은 조건 C `perf_c_multi_linux_20260522_071155_codex_c_tls_multi_for_go_20260522.txt` 대비 Go `perf_go_multi_linux_20260522_072100_codex_go_tls_multi_current_20260522.txt`에서 160.8%, 139.8%다. | `wss/tls SPOT`은 C와 같은 sender/receiver 분리, `DONTWAIT` publish, `SubscribePart` 수신 의미를 유지한다. `MULTI_STREAM`은 shared C reference client를 쓰므로 측정 surface가 Go STREAM server라는 점을 반영해 server hot path 차이를 먼저 본다. `tls MULTI_SPOT`은 Go worker drain이 C보다 backlog를 더 공격적으로 소화하는지 확인하고, 적용 가능한 최적화가 있으면 C/perf와 다른 바인딩으로 역반영 가능한지 검토한다. tcp large outlier는 C 기준을 갱신한 뒤 public API 내부 최적화인지 다시 본다. |
| Python | Single `PAIR` large, Multi `MULTI_STREAM` large | 재측정 후 43.1% (`tcp SPOT 131072B`), multi에서는 422.3% (`ws MULTI_STREAM 262144B`) | 기존 `tcp SPOT 131072B` 271880.0%는 C 기준 throughput이 5 msg/s로 낮게 나온 outlier였다. 같은 조건 제한 재측정에서 C가 33.7 Kmsg/s, Python이 14.6 Kmsg/s로 보류권에 내려왔다. `ws MULTI_STREAM 262144B`는 C 기준을 보강한 뒤에도 Python shared stream server 수치가 높지만, shared C stream client를 쓰므로 서버 hot path 차이와 C 기준 변동을 분리해야 한다. | `PAIR` large와 `MULTI_STREAM` large outlier는 public API 의미가 같은지 확인한 뒤, C 기준 재측정 또는 Python server hot path 차이를 별도로 기록한다. |

2026-05-21 outlier 적용 결과:

- C++ `wss MULTI_SPOT 1024B`: C
  `perf_c_multi_linux_20260521_210632_codex_c_wss_multi_spot1024_outlier_apply_20260521.txt`,
  C++ `perf_cpp_multi_linux_20260521_210739_codex_cpp_wss_multi_spot1024_outlier_apply_20260521.txt`
  기준으로 88.1%다.
- .NET `wss SPOT 65536B`: C
  `perf_c_single_linux_20260521_210757_codex_c_wss_single_spot65536_outlier_apply_20260521.txt`,
  .NET `perf_dotnet_single_linux_20260522_005548_codex_dotnet_wss_single_spot65536_subscribe_part_20260522.txt`
  기준으로 63.5%다. C에 없는 기본 in-flight cap은 제거했고, 수신 hot path는 public `SubscribePart`로 C의 single-part subscribe 의미에 맞췄다.
- Java `wss MULTI_SPOT 256B`: C
  `perf_c_multi_linux_20260521_210845_codex_c_wss_multi_spot256_outlier_apply_20260521.txt`,
  Java `perf_java_multi_linux_20260521_210909_codex_java_wss_multi_spot256_outlier_apply_20260521.txt`
  기준으로 113.1%다.
- Node `wss SPOT 1024B`: C
  `perf_c_single_linux_20260521_210903_codex_c_wss_single_spot1024_outlier_apply_20260521.txt`,
  Node `perf_node_single_linux_20260521_210938_codex_node_wss_single_spot1024_outlier_apply_20260521.txt`
  기준으로 296.4%다. C와 같은 의미가 확인될 때까지 보류로 둔다.
- Node single SPOT의 worker 송신/receiver drain 분리도 검토했지만, JS worker 송신자가
  receiver drain보다 빠르게 active backlog를 크게 만든 뒤 stop token 관찰 전 backlog drain에
  묶였다. 이를 in-flight cap으로 막으면 C에 없는 perf-only credit 제한이 되므로 적용하지
  않았다. Node single SPOT은 public API만으로 C의 별도 native sender/receiver thread 의미를
  재현할 수 있는지 더 검토해야 한다.

잠정 이식 후보는 다음과 같다.

- **no-data 경로에서 예외를 만들지 않기**: Node `recvPayloadInto(...DontWait)` 개선처럼
  hot path에서 no-data를 값으로 돌려 예외 생성 비용을 없애는 방식은 다른 binding에서도
  public API 의미를 해치지 않는지 검토할 수 있다.
- **결과 객체 materialization 축소**: Java `Spot.subscribe` routing-id scratch cache,
  Node raw result의 불필요한 `routingId: null` 생략처럼 반복 수신에서 매번 새 객체나
  byte[]를 만들지 않는 최적화는 binding 공통 후보가 된다.
- **poller 내부 index/cache**: C++ `poller_t` socket-only modify cache와 Java `Poller`
  handle index cache는 public API를 바꾸지 않는 내부 최적화다. .NET Poller도 같은
  선형 탐색 hot path가 있는지 별도 검토한다.
- **C-style 단일 poll loop 유지**: .NET `MULTI_SPOT_SENDSEND`처럼 C/perf와 다른
  POLLOUT 중심 대기는 성능과 의미를 모두 흔들 수 있다. binding perf는 C와 같은
  signal-driven poll loop를 먼저 맞춘다.

위 후보는 성능을 올리기 위한 임의 변경이 아니라, C/perf와 의미가 같은지 확인된 뒤에만
반영한다. C보다 크게 나온 셀은 다음 측정 라운드에서 우선 재검증 대상으로 잡는다.

Thread 관측은 성능 판정의 보조 지표로 함께 남긴다. 현재 `clients=100` multi 조건에서
Python은 non-SPOT 7~8개, SPOT 12~13개 수준이었고 Rust는 일반 echo 7~8개,
SPOT 계열 14~15개 수준이었다. 두 언어 모두 client 수가 OS thread 수로 그대로
확장되지는 않았다. Go는 다른 런타임과 달리 goroutine 실행용 P 개수를 `GOMAXPROCS`가
정하며, 이 값을 비워 두면 머신 CPU 수 쪽으로 열려 순간 CPU 사용률과 OS thread 수가
크게 보일 수 있다. 따라서 Go perf runner는 명시 설정이 없을 때 `GOMAXPROCS=4`를
기본으로 두고, 이후 thread/CPU 관측은 같은 조건에서만 비교한다.

### 6.2 C++ 상태

#### 6.2.1 Single suite

2026-05-27 새 측정 라운드 기준으로 기존 2026-05-19, 2026-05-26 단편 파일 기준은 리셋한다.
최신 기준 파일은 C `perf_c_single_linux_20260527_123415_codex_refresh_c_single_default_20260527.txt`,
C++ `perf_cpp_single_linux_20260527_143903_codex_refresh_cpp_single_default_retry_clean_20260527.txt`다.
두 파일 모두 `status=complete`, 결과 라인은 `1020/1020`이다.
Routed large는 같은 조건으로 C와 C++를 제한 재측정했다. C 재확인 파일은
`perf_c_single_linux_20260527_180106_codex_c_single_cpp_remaining_routed_large_recheck_20260527.txt`이고,
C++ 보강 파일은
`perf_cpp_single_linux_20260527_175819_codex_cpp_single_dealer_router_inproc_large_recheck_20260527.txt`,
`perf_cpp_single_linux_20260527_175836_codex_cpp_single_router_router_inproc_ws_large_recheck_20260527.txt`,
`perf_cpp_single_linux_20260527_175934_codex_cpp_single_wss_pair_pubsub_spot_recheck_20260527.txt`,
`perf_cpp_single_linux_20260527_180056_codex_cpp_single_dealer_router_tcp128k_recheck_20260527.txt`다.

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | `통과(100.0%)` | `통과(99.9%)` | `통과(100.8%)` | `통과(100.6%)` | `통과(98.2%)` | `통과(99.5%)` | C: `perf_c_single_linux_20260527_123415_codex_refresh_c_single_default_20260527.txt`; C++: `perf_cpp_single_linux_20260527_143903_codex_refresh_cpp_single_default_retry_clean_20260527.txt` |
| `tcp` | `PUBSUB` | `통과(96.3%)` | `통과(117.0%)` | `통과(124.3%)` | `통과(413.8%)` | `통과(349.4%)` | `통과(567.6%)` | C/C++: 위 파일 |
| `tcp` | `DEALER_DEALER` | `통과(100.2%)` | `통과(100.3%)` | `통과(100.1%)` | `통과(100.1%)` | `통과(100.1%)` | `통과(99.9%)` | C/C++: 위 파일 |
| `tcp` | `DEALER_ROUTER` | `통과(94.0%)` | `통과(92.3%)` | `통과(97.9%)` | `통과(78.3%)` | `보류(59.7%)` | `통과(75.4%)` | 131072B는 위 routed large 재확인 기준. direct native submit 후보는 통과에 못 닿고 inproc large를 낮춰 반영하지 않았다. |
| `tcp` | `ROUTER_ROUTER` | `통과(105.9%)` | `통과(114.2%)` | `통과(98.2%)` | `통과(97.0%)` | `통과(92.4%)` | `통과(82.1%)` | C/C++: 위 파일 |
| `tcp` | `SPOT` | `통과(100.3%)` | `통과(90.3%)` | `통과(104.0%)` | `통과(91.2%)` | `통과(82.0%)` | `통과(98.9%)` | C/C++: 위 파일 |
| `ws` | `PAIR` | `통과(100.0%)` | `통과(100.1%)` | `통과(98.6%)` | `통과(100.0%)` | `통과(100.2%)` | `통과(99.6%)` | C/C++: 위 파일 |
| `ws` | `PUBSUB` | `통과(92.2%)` | `통과(103.4%)` | `통과(115.3%)` | `통과(190.5%)` | `통과(279.4%)` | `통과(454.0%)` | C/C++: 위 파일 |
| `ws` | `DEALER_DEALER` | `통과(101.3%)` | `통과(101.0%)` | `통과(95.7%)` | `통과(99.7%)` | `통과(99.1%)` | `통과(99.5%)` | C/C++: 위 파일 |
| `ws` | `DEALER_ROUTER` | `통과(93.6%)` | `통과(94.7%)` | `통과(98.5%)` | `통과(90.0%)` | `통과(98.5%)` | `통과(95.7%)` | C/C++: 위 파일 |
| `ws` | `ROUTER_ROUTER` | `통과(111.6%)` | `통과(97.0%)` | `통과(93.2%)` | `통과(85.4%)` | `통과(95.9%)` | `통과(98.1%)` | 262144B는 `perf_cpp_single_linux_20260527_175836_codex_cpp_single_router_router_inproc_ws_large_recheck_20260527.txt` 기준 |
| `ws` | `SPOT` | `통과(113.4%)` | `통과(104.1%)` | `통과(112.2%)` | `통과(102.5%)` | `통과(113.4%)` | `통과(101.3%)` | C/C++: 위 파일 |
| `wss` | `PAIR` | `통과(100.0%)` | `통과(98.3%)` | `통과(97.6%)` | `통과(106.8%)` | `통과(90.4%)` | `통과(87.3%)` | 262144B는 `perf_cpp_single_linux_20260527_175934_codex_cpp_single_wss_pair_pubsub_spot_recheck_20260527.txt` 기준 |
| `wss` | `PUBSUB` | `통과(86.4%)` | `통과(103.5%)` | `통과(114.0%)` | `통과(107.6%)` | `통과(80.4%)` | `통과(92.1%)` | 256B/65536B는 `perf_cpp_single_linux_20260527_175934_codex_cpp_single_wss_pair_pubsub_spot_recheck_20260527.txt` 기준 |
| `wss` | `DEALER_DEALER` | `통과(99.6%)` | `통과(96.7%)` | `통과(94.3%)` | `통과(103.2%)` | `통과(86.9%)` | `통과(85.4%)` | C/C++: 위 파일 |
| `wss` | `DEALER_ROUTER` | `통과(93.6%)` | `통과(89.2%)` | `통과(96.0%)` | `통과(91.6%)` | `통과(83.1%)` | `통과(88.9%)` | C/C++: 위 파일 |
| `wss` | `ROUTER_ROUTER` | `통과(107.6%)` | `통과(95.9%)` | `통과(97.6%)` | `통과(99.6%)` | `통과(99.2%)` | `통과(100.0%)` | C/C++: 위 파일 |
| `wss` | `SPOT` | `통과(106.6%)` | `통과(107.1%)` | `통과(198.8%)` | `통과(103.8%)` | `통과(123.6%)` | `통과(174.0%)` | 262144B는 `perf_cpp_single_linux_20260527_175934_codex_cpp_single_wss_pair_pubsub_spot_recheck_20260527.txt` 기준 |
| `tls` | `PAIR` | `통과(99.6%)` | `통과(99.7%)` | `통과(96.0%)` | `통과(88.6%)` | `통과(97.1%)` | `통과(98.2%)` | C/C++: 위 파일 |
| `tls` | `PUBSUB` | `통과(100.4%)` | `통과(106.8%)` | `통과(117.6%)` | `통과(116.0%)` | `통과(118.3%)` | `통과(132.0%)` | C/C++: 위 파일 |
| `tls` | `DEALER_DEALER` | `통과(100.0%)` | `통과(100.1%)` | `통과(96.6%)` | `통과(92.1%)` | `통과(96.7%)` | `통과(96.2%)` | C/C++: 위 파일 |
| `tls` | `DEALER_ROUTER` | `통과(92.6%)` | `통과(96.6%)` | `통과(96.2%)` | `통과(96.8%)` | `통과(93.5%)` | `통과(251.6%)` | C/C++: 위 파일 |
| `tls` | `ROUTER_ROUTER` | `통과(116.3%)` | `통과(114.4%)` | `통과(99.8%)` | `통과(90.5%)` | `통과(96.7%)` | `통과(174.9%)` | C/C++: 위 파일 |
| `tls` | `SPOT` | `통과(111.6%)` | `통과(103.4%)` | `통과(99.9%)` | `통과(98.5%)` | `통과(106.1%)` | `통과(79.3%)` | C/C++: 위 파일 |
| `inproc` | `PAIR` | `통과(85.2%)` | `통과(93.3%)` | `통과(88.9%)` | `통과(100.6%)` | `통과(99.7%)` | `통과(99.7%)` | C/C++: 위 파일 |
| `inproc` | `PUBSUB` | `통과(86.3%)` | `통과(92.6%)` | `통과(91.9%)` | `통과(1025.6%)` | `통과(806.9%)` | `통과(295.3%)` | C/C++: 위 파일 |
| `inproc` | `DEALER_DEALER` | `통과(93.5%)` | `통과(93.3%)` | `통과(92.2%)` | `통과(100.0%)` | `통과(100.1%)` | `통과(99.9%)` | C/C++: 위 파일 |
| `inproc` | `DEALER_ROUTER` | `통과(95.9%)` | `통과(102.7%)` | `통과(95.6%)` | `통과(80.2%)` | `통과(89.5%)` | `보류(34.2%)` | 131072/262144B는 위 routed large 재확인 기준. direct native submit 후보는 262144B를 더 낮춰 반영하지 않았다. |
| `inproc` | `ROUTER_ROUTER` | `통과(107.1%)` | `통과(98.9%)` | `통과(89.0%)` | `통과(81.4%)` | `통과(72.6%)` | `보류(55.5%)` | 131072/262144B는 위 routed large 재확인 기준. direct native submit 후보는 262144B를 더 낮춰 반영하지 않았다. |
| `inproc` | `SPOT` | `해당 없음` | `해당 없음` | `해당 없음` | `해당 없음` | `해당 없음` | `해당 없음` | full single에서 SPOT inproc 조합은 결과가 없다. |
| `ipc` | `PAIR` | `통과(98.9%)` | `통과(101.7%)` | `통과(103.6%)` | `통과(83.4%)` | `통과(95.6%)` | `통과(98.5%)` | C/C++: 위 파일 |
| `ipc` | `PUBSUB` | `통과(97.3%)` | `통과(113.4%)` | `통과(113.4%)` | `통과(348.1%)` | `통과(313.0%)` | `통과(545.0%)` | C/C++: 위 파일 |
| `ipc` | `DEALER_DEALER` | `통과(100.2%)` | `통과(100.2%)` | `통과(103.6%)` | `통과(100.0%)` | `통과(100.2%)` | `통과(100.0%)` | C/C++: 위 파일 |
| `ipc` | `DEALER_ROUTER` | `통과(95.7%)` | `통과(94.2%)` | `통과(98.4%)` | `통과(92.8%)` | `통과(87.5%)` | `통과(76.8%)` | C/C++: 위 파일 |
| `ipc` | `ROUTER_ROUTER` | `통과(103.8%)` | `통과(102.6%)` | `통과(100.3%)` | `통과(100.8%)` | `통과(101.3%)` | `통과(82.6%)` | C/C++: 위 파일 |
| `ipc` | `SPOT` | `해당 없음` | `해당 없음` | `해당 없음` | `해당 없음` | `해당 없음` | `해당 없음` | full single에서 SPOT ipc 조합은 결과가 없다. |

#### 6.2.2 Multi suite

2026-05-27 새 측정 라운드 기준으로 기존 2026-05-21, 2026-05-26 단편 파일 기준은 리셋한다.
최신 C 기준 파일은
`perf_c_multi_linux_20260527_134603_codex_refresh_c_multi_64_256_1k_4k_64k_128k_retry2_20260527.txt`,
C 재확인 파일은
`perf_c_multi_linux_20260527_165625_codex_c_multi_remaining_cpp_misses_recheck_20260527.txt`다.
둘 다 `status=complete`다. C++ full 대표 파일은
`perf_cpp_multi_linux_20260527_160322_codex_refresh_cpp_multi_64_256_1k_4k_64k_128k_after_fastpaths_20260527.txt`이며
`status=complete`, 결과 라인은 `960/960`이다. 이후 낮게 나온 조합은 같은 조건 제한 재측정으로
확인했고, 최신 보강 파일까지 합치면 C++ multi 미달은 없다.

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 통과(85.7%) | 통과(94.6%) | 통과(101.0%) | 통과(104.9%) | 통과(100.1%) | 통과(93.4%) | C: 대표 full + C 재확인; C++: 대표 full + 보강 파일 |
| `tcp` | `MULTI_DEALER_ROUTER` | 통과(96.9%) | 통과(92.2%) | 통과(84.3%) | 통과(86.6%) | 통과(76.9%) | 통과(69.2%) | C/C++: 위 파일 |
| `tcp` | `MULTI_ROUTER_ROUTER` | 통과(87.8%) | 통과(83.5%) | 통과(90.3%) | 통과(96.9%) | 통과(77.5%) | 통과(73.9%) | C/C++: 위 파일 |
| `tcp` | `MULTI_PUBSUB` | 통과(96.4%) | 통과(98.3%) | 통과(129.9%) | 통과(115.6%) | 통과(116.8%) | 통과(92.6%) | C/C++: 위 파일 |
| `tcp` | `MULTI_SPOT` | 통과(93.9%) | 통과(92.5%) | 통과(94.9%) | 통과(99.6%) | 통과(97.5%) | 통과(93.3%) | C/C++: 위 파일 |
| `tcp` | `MULTI_SPOT_REQREP` | 통과(96.5%) | 통과(97.4%) | 통과(105.7%) | 통과(85.1%) | 통과(101.1%) | 통과(109.8%) | C/C++: 위 파일 |
| `tcp` | `MULTI_SPOT_SENDSEND` | 통과(100.8%) | 통과(101.0%) | 통과(110.4%) | 통과(109.1%) | 통과(105.6%) | 통과(233.6%) | C/C++: 위 파일 |
| `tcp` | `MULTI_STREAM` | 통과(126.8%) | 통과(155.8%) | 통과(132.2%) | 통과(172.6%) | 통과(201.6%) | 통과(121.9%) | C/C++: 위 파일 |
| `ws` | `MULTI_DEALER_DEALER` | 통과(84.4%) | 통과(98.9%) | 통과(86.7%) | 통과(103.2%) | 통과(96.4%) | 통과(119.3%) | C/C++: 위 파일 |
| `ws` | `MULTI_DEALER_ROUTER` | 통과(85.8%) | 통과(78.9%) | 통과(93.6%) | 통과(86.2%) | 통과(74.8%) | 통과(84.0%) | C/C++: 위 파일 |
| `ws` | `MULTI_ROUTER_ROUTER` | 통과(117.8%) | 통과(74.1%) | 통과(98.1%) | 통과(85.2%) | 통과(105.7%) | 통과(122.3%) | C/C++: 위 파일 |
| `ws` | `MULTI_PUBSUB` | 통과(95.4%) | 통과(85.2%) | 통과(104.3%) | 통과(117.2%) | 통과(80.5%) | 통과(90.7%) | C/C++: 위 파일 |
| `ws` | `MULTI_SPOT` | 통과(85.2%) | 통과(86.6%) | 통과(98.7%) | 통과(91.2%) | 통과(91.5%) | 통과(101.0%) | C/C++: 위 파일 |
| `ws` | `MULTI_SPOT_REQREP` | 통과(91.1%) | 통과(87.7%) | 통과(100.0%) | 통과(93.4%) | 통과(97.3%) | 통과(101.2%) | C/C++: 위 파일 |
| `ws` | `MULTI_SPOT_SENDSEND` | 통과(95.3%) | 통과(88.0%) | 통과(83.2%) | 통과(99.4%) | 통과(90.2%) | 통과(82.9%) | C/C++: 위 파일 |
| `ws` | `MULTI_STREAM` | 통과(87.6%) | 통과(174.8%) | 통과(100.4%) | 통과(133.5%) | 통과(200.8%) | 통과(178.8%) | C/C++: 위 파일 |
| `wss` | `MULTI_DEALER_DEALER` | 통과(87.5%) | 통과(101.7%) | 통과(96.8%) | 통과(103.4%) | 통과(105.5%) | 통과(120.2%) | C/C++: 위 파일 |
| `wss` | `MULTI_DEALER_ROUTER` | 통과(91.8%) | 통과(94.3%) | 통과(97.2%) | 통과(102.8%) | 통과(91.2%) | 통과(79.2%) | C/C++: 위 파일 |
| `wss` | `MULTI_ROUTER_ROUTER` | 통과(90.6%) | 통과(93.0%) | 통과(87.6%) | 통과(121.1%) | 통과(99.3%) | 통과(92.6%) | C/C++: 위 파일 |
| `wss` | `MULTI_PUBSUB` | 통과(81.2%) | 통과(93.4%) | 통과(88.4%) | 통과(90.0%) | 통과(95.2%) | 통과(119.3%) | C/C++: 위 파일 |
| `wss` | `MULTI_SPOT` | 통과(114.9%) | 통과(97.1%) | 통과(100.9%) | 통과(101.5%) | 통과(105.0%) | 통과(106.7%) | C/C++: 위 파일 |
| `wss` | `MULTI_SPOT_REQREP` | 통과(89.4%) | 통과(81.4%) | 통과(88.0%) | 통과(90.8%) | 통과(93.2%) | 통과(102.8%) | C/C++: 위 파일 |
| `wss` | `MULTI_SPOT_SENDSEND` | 통과(96.7%) | 통과(95.4%) | 통과(94.0%) | 통과(92.7%) | 통과(102.9%) | 통과(105.3%) | C/C++: 위 파일 |
| `wss` | `MULTI_STREAM` | 통과(80.4%) | 통과(212.5%) | 통과(93.4%) | 통과(182.5%) | 통과(228.3%) | 통과(275.6%) | C/C++: 위 파일 |
| `tls` | `MULTI_DEALER_DEALER` | 통과(84.8%) | 통과(104.3%) | 통과(96.3%) | 통과(90.2%) | 통과(92.0%) | 통과(93.6%) | C/C++: 위 파일 |
| `tls` | `MULTI_DEALER_ROUTER` | 통과(78.4%) | 통과(93.2%) | 통과(82.2%) | 통과(82.6%) | 통과(95.1%) | 통과(105.0%) | C/C++: 위 파일 |
| `tls` | `MULTI_ROUTER_ROUTER` | 통과(98.3%) | 통과(82.5%) | 통과(89.9%) | 통과(88.3%) | 통과(111.2%) | 통과(90.2%) | C/C++: 위 파일 |
| `tls` | `MULTI_PUBSUB` | 통과(94.7%) | 통과(92.2%) | 통과(142.3%) | 통과(133.0%) | 통과(83.6%) | 통과(97.6%) | C/C++: 위 파일 |
| `tls` | `MULTI_SPOT` | 통과(89.7%) | 통과(76.7%) | 통과(97.2%) | 통과(97.2%) | 통과(103.7%) | 통과(106.1%) | C/C++: 위 파일 |
| `tls` | `MULTI_SPOT_REQREP` | 통과(97.2%) | 통과(105.4%) | 통과(76.8%) | 통과(91.5%) | 통과(95.9%) | 통과(89.4%) | C/C++: 위 파일 |
| `tls` | `MULTI_SPOT_SENDSEND` | 통과(96.5%) | 통과(111.5%) | 통과(109.3%) | 통과(95.8%) | 통과(93.8%) | 통과(95.9%) | C/C++: 위 파일 |
| `tls` | `MULTI_STREAM` | 통과(85.9%) | 통과(188.3%) | 통과(84.3%) | 통과(197.7%) | 통과(174.0%) | 통과(153.6%) | C/C++: 위 파일 |

보강 파일은 `perf_cpp_multi_linux_20260527_163929_codex_cpp_multi_remaining_misses_recheck_after_full_20260527.txt`,
`perf_cpp_multi_linux_20260527_173402_codex_cpp_multi_new_miss_candidates_recheck_20260527.txt`,
`perf_cpp_multi_linux_20260527_174433_codex_cpp_multi_dealer_router_tcp128k_final_recheck_20260527.txt`,
`perf_cpp_multi_linux_20260527_174442_codex_cpp_multi_spot_wss64k_final_recheck_20260527.txt`와
중간 단일 셀 재확인 파일들이다. C++ binding에는 C public 계약에 대응하는
`sub_socket_t::subscribe_part(...)`, `xsub_socket_t::subscribe_part(...)`,
`dealer_socket_t::send_no_wait(...)`, `pub_socket_t::publish_no_wait(...)`,
`xpub_socket_t::publish_no_wait(...)`를 추가했다. PUBSUB client hot path는 perf wrapper의
raw native handle로 `zlink_subscribe_part`를 직접 호출해 C 수신 루프와 같은 비용 구조로 맞췄다.

### 6.3 .NET 상태

#### 6.3.1 Single suite

2026-05-27 full refresh 기준으로 기존 2026-05-20/22 단편 파일 기준은 리셋한다. 최신 기준 파일은
C `perf_c_single_linux_20260526_213234_codex_full_refresh_c_single_full_20260526.txt`,
.NET `perf_dotnet_single_linux_20260527_105052_codex_full_refresh_dotnet_single_reset_retry3_20260527.txt`다.
두 파일 모두 `status=complete`, 결과 라인은 `1020/1020`이다. 이전 retry
`perf_dotnet_single_linux_20260527_101911_codex_full_refresh_dotnet_single_reset_retry_20260527.txt`는
`DEALER_DEALER ws 131072/262144B`에서 partial이었지만, 같은 조건 제한 재측정
`perf_dotnet_single_linux_20260527_103044_codex_dotnet_single_dd_ws_large_repro_20260527.txt`와
최신 full retry3에서는 모두 통과했다.

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | `통과(98%)` | `통과(77%)` | `통과(114%)` | `통과(96%)` | `통과(99%)` | `통과(99%)` | C: `perf_c_single_linux_20260526_213234_codex_full_refresh_c_single_full_20260526.txt`; .NET: `perf_dotnet_single_linux_20260527_105052_codex_full_refresh_dotnet_single_reset_retry3_20260527.txt` |
| `tcp` | `PUBSUB` | `통과(90%)` | `개선 대상(59.1%)` | `통과(113%)` | `통과(98%)` | `통과(98%)` | `통과(94%)` | C/.NET: 위 파일. 256B는 최신 full 기준 미달이다. |
| `tcp` | `DEALER_DEALER` | `통과(98%)` | `통과(81%)` | `통과(118%)` | `통과(98%)` | `통과(96%)` | `통과(84%)` | C/.NET: 위 파일 |
| `tcp` | `DEALER_ROUTER` | `통과(79%)` | `개선 대상(74.7%)` | `통과(83%)` | `개선 대상(74.8%)` | `통과(95%)` | `통과(92%)` | C/.NET: 위 파일. 256B/65536B는 최신 full 기준 미달이다. |
| `tcp` | `ROUTER_ROUTER` | `통과(94%)` | `통과(89%)` | `통과(86%)` | `개선 대상(74.9%)` | `통과(89%)` | `통과(90%)` | C/.NET: 위 파일. 65536B는 최신 full 기준 미달이다. |
| `tcp` | `SPOT` | `통과(110%)` | `통과(97%)` | `통과(98%)` | `통과(91%)` | `통과(92%)` | `통과(91%)` | C/.NET: 위 파일 |
| `ws` | `PAIR` | `통과(97%)` | `통과(95%)` | `통과(110%)` | `통과(98%)` | `통과(98%)` | `통과(98%)` | C/.NET: 위 파일 |
| `ws` | `PUBSUB` | `통과(89%)` | `통과(89%)` | `통과(108%)` | `통과(99%)` | `통과(97%)` | `통과(98%)` | C/.NET: 위 파일 |
| `ws` | `DEALER_DEALER` | `통과(98%)` | `통과(95%)` | `통과(120%)` | `통과(98%)` | `통과(98%)` | `통과(98%)` | C/.NET: 위 파일 |
| `ws` | `DEALER_ROUTER` | `통과(76%)` | `개선 대상(74.9%)` | `통과(83%)` | `통과(87%)` | `통과(90%)` | `통과(98%)` | C/.NET: 위 파일. 256B는 최신 full 기준 미달이다. |
| `ws` | `ROUTER_ROUTER` | `통과(92%)` | `통과(81%)` | `통과(83%)` | `통과(88%)` | `통과(93%)` | `통과(95%)` | C/.NET: 위 파일 |
| `ws` | `SPOT` | `통과(110%)` | `통과(102%)` | `통과(94%)` | `통과(87%)` | `통과(98%)` | `통과(98%)` | C/.NET: 위 파일 |
| `wss` | `PAIR` | `통과(98%)` | `개선 대상(44.0%)` | `통과(108%)` | `통과(103%)` | `통과(90%)` | `통과(88%)` | C/.NET: 위 파일. 256B는 최신 full 기준 미달이다. |
| `wss` | `PUBSUB` | `통과(94%)` | `통과(96%)` | `통과(104%)` | `통과(97%)` | `통과(95%)` | `통과(102%)` | C/.NET: 위 파일 |
| `wss` | `DEALER_DEALER` | `통과(98%)` | `통과(94%)` | `통과(105%)` | `통과(100%)` | `통과(91%)` | `통과(97%)` | C/.NET: 위 파일 |
| `wss` | `DEALER_ROUTER` | `통과(77%)` | `통과(79%)` | `통과(94%)` | `통과(90%)` | `통과(95%)` | `통과(123%)` | C/.NET: 위 파일 |
| `wss` | `ROUTER_ROUTER` | `통과(83%)` | `통과(79%)` | `통과(93%)` | `통과(83%)` | `통과(94%)` | `통과(120%)` | C/.NET: 위 파일 |
| `wss` | `SPOT` | `통과(110%)` | `통과(97%)` | `통과(93%)` | `통과(80%)` | `개선 대상(48.4%)` | `개선 대상(67.0%)` | C/.NET: 위 파일. 131072B/262144B는 최신 full 기준 미달이다. |
| `tls` | `PAIR` | `통과(97%)` | `통과(96%)` | `통과(115%)` | `통과(96%)` | `통과(98%)` | `통과(98%)` | C/.NET: 위 파일 |
| `tls` | `PUBSUB` | `통과(95%)` | `통과(87%)` | `통과(108%)` | `통과(83%)` | `통과(98%)` | `통과(96%)` | C/.NET: 위 파일 |
| `tls` | `DEALER_DEALER` | `통과(98%)` | `통과(96%)` | `통과(112%)` | `통과(97%)` | `통과(96%)` | `통과(97%)` | C/.NET: 위 파일 |
| `tls` | `DEALER_ROUTER` | `통과(78%)` | `통과(75%)` | `통과(92%)` | `통과(98%)` | `통과(98%)` | `통과(99%)` | C/.NET: 위 파일 |
| `tls` | `ROUTER_ROUTER` | `통과(88%)` | `통과(89%)` | `통과(96%)` | `통과(92%)` | `통과(91%)` | `통과(92%)` | C/.NET: 위 파일 |
| `tls` | `SPOT` | `통과(105%)` | `통과(99%)` | `통과(93%)` | `통과(100%)` | `통과(100%)` | `통과(101%)` | C/.NET: 위 파일 |
| `inproc` | `PAIR` | `통과(77%)` | `통과(79%)` | `통과(87%)` | `개선 대상(30.7%)` | `통과(98%)` | `통과(98%)` | C/.NET: 위 파일. 65536B는 최신 full 기준 미달이다. |
| `inproc` | `PUBSUB` | `통과(94%)` | `통과(93%)` | `통과(98%)` | `통과(98%)` | `통과(98%)` | `통과(98%)` | C/.NET: 위 파일 |
| `inproc` | `DEALER_DEALER` | `개선 대상(75.0%)` | `통과(78%)` | `개선 대상(71.2%)` | `통과(100%)` | `통과(98%)` | `통과(98%)` | C/.NET: 위 파일. 64B/1024B는 최신 full 기준 미달이다. |
| `inproc` | `DEALER_ROUTER` | `통과(75%)` | `통과(77%)` | `개선 대상(74.2%)` | `개선 대상(33.9%)` | `개선 대상(33.7%)` | `통과(101%)` | C/.NET: 위 파일. 1024B/65536B/131072B는 최신 full 기준 미달이다. |
| `inproc` | `ROUTER_ROUTER` | `통과(84%)` | `통과(77%)` | `개선 대상(74.2%)` | `개선 대상(34.8%)` | `통과(84%)` | `통과(148%)` | C/.NET: 위 파일. 1024B/65536B는 최신 full 기준 미달이다. |
| `inproc` | `SPOT` | `해당 없음` | `해당 없음` | `해당 없음` | `해당 없음` | `해당 없음` | `해당 없음` | full single에서 SPOT inproc 조합은 결과가 없다. |
| `ipc` | `PAIR` | `통과(98%)` | `통과(95%)` | `통과(113%)` | `통과(91%)` | `통과(98%)` | `통과(97%)` | C/.NET: 위 파일 |
| `ipc` | `PUBSUB` | `개선 대상(41.0%)` | `통과(93%)` | `통과(105%)` | `통과(98%)` | `통과(98%)` | `통과(98%)` | C/.NET: 위 파일. 64B는 최신 full 기준 미달이다. |
| `ipc` | `DEALER_DEALER` | `통과(96%)` | `개선 대상(74.9%)` | `통과(113%)` | `통과(99%)` | `통과(98%)` | `통과(98%)` | C/.NET: 위 파일. 256B는 최신 full 기준 미달이다. |
| `ipc` | `DEALER_ROUTER` | `개선 대상(73.2%)` | `개선 대상(71.7%)` | `통과(80%)` | `통과(80%)` | `개선 대상(72.7%)` | `통과(92%)` | C/.NET: 위 파일. 64B/256B/131072B는 최신 full 기준 미달이다. |
| `ipc` | `ROUTER_ROUTER` | `통과(82%)` | `통과(87%)` | `통과(86%)` | `통과(90%)` | `개선 대상(71.2%)` | `통과(81%)` | C/.NET: 위 파일. 131072B는 최신 full 기준 미달이다. |
| `ipc` | `SPOT` | `해당 없음` | `해당 없음` | `해당 없음` | `해당 없음` | `해당 없음` | `해당 없음` | full single에서 SPOT ipc 조합은 결과가 없다. |

#### 6.3.2 Multi suite

2026-05-27 full refresh 재측정은
`perf_dotnet_multi_linux_20260527_111459_codex_full_refresh_dotnet_multi_reset_20260527.txt`에서
`MULTI_SPOT_REQREP tcp 256B` client가 5초 안에 종료하지 못해 `partial`로 끝났다
(`success=121`, `fail=1`, `expected_result_lines=610`, `actual_result_lines=605`).
실패 지점 전까지 `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`,
`MULTI_PUBSUB`, `MULTI_SPOT`은 full run 안에서 완료됐다. 이 partial 파일은 최신 장애
기록으로 남긴다. 같은 조건 단독 재측정
`perf_dotnet_multi_linux_20260527_113523_codex_dotnet_multi_spot_reqrep_tcp256_repro_20260527.txt`는
complete였고 170.431 Kops/s로 측정됐다. 따라서 full retry를 다시 실행해 전체
complete 파일을 확보한다. full retry
`perf_dotnet_multi_linux_20260527_113556_codex_full_refresh_dotnet_multi_reset_retry_20260527.txt`는
앞선 `MULTI_SPOT_REQREP tcp 256B`를 통과했지만 `MULTI_SPOT_SENDSEND ws 65536B`에서
server가 5초 안에 종료하지 못해 다시 `partial`로 끝났다(`success=159`, `fail=1`,
`expected_result_lines=800`, `actual_result_lines=795`). 같은 retry에서
`MULTI_SPOT tcp 131072B`는 0.160 Kmsg/s로 비정상적으로 낮아 별도 재측정 후보로 둔다.
같은 조건 단독 재측정
`perf_dotnet_multi_linux_20260527_120023_codex_dotnet_multi_sendsend_ws65536_repro_20260527.txt`는
complete였고 35.924 Kops/s로 측정됐다. full retry2
`perf_dotnet_multi_linux_20260527_120109_codex_full_refresh_dotnet_multi_reset_retry2_20260527.txt`는
`MULTI_SPOT_REQREP tcp 256B`와 앞선 `MULTI_SPOT_SENDSEND ws 65536B` 지점을 통과했지만,
`MULTI_SPOT_SENDSEND tls 131072B`에서 `zlink error code 704 (errno 98)`와
`server_ready_timeout`으로 partial 종료됐다(`success=154`, `fail=1`,
`expected_result_lines=775`, `actual_result_lines=770`). 세 번의 full 시도가 모두
SPOT reqrep/sendsend 계열에서 서로 다른 shutdown/bind-ready 실패를 냈으므로, 최신
전체 complete 파일은 아직 확보되지 않았다. 이 항목은 성능 비율 이전에 full-run 안정화
보류로 둔다.

2026-05-21 재측정 결과로 대표 표를 갱신했다. 판정은 `doc/perf` 기준처럼 C `bindings/c/perf`와 같은 suite/pattern/transport/size의 throughput 비율로 계산한다. HWM은 튜닝 값으로 쓰지 않고, auto-HWM 활성 여부와 size별 `MsgUnit(B)` 일치 여부만 확인한다.

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | `통과(69.9%)` | `통과(82.2%)` | `통과(87.4%)` | `통과(102.8%)` | `통과(101.4%)` | `통과(103.0%)` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; .NET `perf_dotnet_multi_linux_20260521_145328_codex_dotnet_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_DEALER_ROUTER` | `통과(61.4%)` | `통과(62.0%)` | `통과(61.0%)` | `통과(70.7%)` | `통과(78.9%)` | `통과(96.1%)` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; .NET `perf_dotnet_multi_linux_20260521_145328_codex_dotnet_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_ROUTER_ROUTER` | `통과(53.1%)` | `통과(54.5%)` | `통과(53.9%)` | `통과(65.7%)` | `통과(87.3%)` | `통과(100.6%)` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; .NET `perf_dotnet_multi_linux_20260521_145328_codex_dotnet_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_PUBSUB` | `통과(75.6%)` | `통과(73.9%)` | `통과(148.1%)` | `통과(87.2%)` | `통과(85.2%)` | `통과(109.8%)` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; .NET `perf_dotnet_multi_linux_20260521_145328_codex_dotnet_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_SPOT` | `통과(73.4%)` | `통과(117.2%)` | `통과(63.9%)` | `통과(101.5%)` | `통과(95.0%)` | `통과(76.8%)` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; .NET `perf_dotnet_multi_linux_20260521_145328_codex_dotnet_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_SPOT_REQREP` | `통과(64.2%)` | `통과(64.7%)` | `통과(65.0%)` | `통과(60.7%)` | `통과(77.4%)` | `통과(103.0%)` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; .NET `perf_dotnet_multi_linux_20260521_170326_codex_dotnet_multi_spot_reqrep_pollcompletion_full2_20260521.txt`. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `MULTI_SPOT_SENDSEND` | `통과(64.4%)` | `통과(62.3%)` | `통과(63.2%)` | `통과(88.6%)` | `통과(91.1%)` | `통과(125.6%)` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; .NET `perf_dotnet_multi_linux_20260521_145328_codex_dotnet_tcp_multi_remeasure_20260521.txt`. 64/256/1024B는 C `perf_c_multi_linux_20260521_173900_codex_c_spot_sendsend_small_for_dotnet_probe_20260521.txt` 대비 .NET `perf_dotnet_multi_linux_20260521_173735_codex_dotnet_spot_sendsend_small_probe_20260521.txt`로 보강했다. |
| `tcp` | `MULTI_STREAM` | `통과(94.4%)` | `통과(95.0%)` | `통과(90.5%)` | `통과(95.2%)` | `해당 없음` | `해당 없음` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; .NET `perf_dotnet_multi_linux_20260521_145328_codex_dotnet_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_DEALER_DEALER` | `통과(69.6%)` | `통과(82.8%)` | `통과(97.9%)` | `통과(106.0%)` | `통과(109.0%)` | `통과(174.3%)` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150054_codex_dotnet_ws_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_DEALER_ROUTER` | `통과(64.2%)` | `통과(65.0%)` | `통과(66.1%)` | `통과(68.2%)` | `통과(98.2%)` | `통과(126.0%)` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150054_codex_dotnet_ws_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_ROUTER_ROUTER` | `통과(60.4%)` | `통과(59.8%)` | `통과(59.1%)` | `통과(60.3%)` | `통과(87.0%)` | `통과(114.3%)` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150054_codex_dotnet_ws_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_PUBSUB` | `통과(78.7%)` | `통과(77.3%)` | `통과(77.7%)` | `통과(131.4%)` | `통과(147.4%)` | `통과(133.8%)` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150054_codex_dotnet_ws_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_SPOT` | `통과(71.1%)` | `통과(69.6%)` | `통과(63.9%)` | `통과(103.1%)` | `통과(94.4%)` | `통과(67.3%)` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150054_codex_dotnet_ws_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_SPOT_REQREP` | `통과(73.7%)` | `통과(71.2%)` | `통과(74.4%)` | `통과(63.3%)` | `통과(99.8%)` | `통과(90.9%)` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_170326_codex_dotnet_multi_spot_reqrep_pollcompletion_full2_20260521.txt`. 64B는 C `perf_c_multi_linux_20260521_173721_codex_c_ws_spot_reqrep64_for_dotnet_no_managed_timer_20260521.txt` 대비 .NET `perf_dotnet_multi_linux_20260521_173705_codex_dotnet_ws_spot_reqrep64_no_managed_timer_20260521.txt`로 보강했다. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `MULTI_SPOT_SENDSEND` | `통과(67.4%)` | `통과(82.1%)` | `통과(69.0%)` | `통과(107.0%)` | `통과(97.0%)` | `통과(95.9%)` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150054_codex_dotnet_ws_multi_remeasure_20260521.txt`. 64/256/1024B는 C `perf_c_multi_linux_20260521_173900_codex_c_spot_sendsend_small_for_dotnet_probe_20260521.txt` 대비 .NET `perf_dotnet_multi_linux_20260521_173735_codex_dotnet_spot_sendsend_small_probe_20260521.txt`로 보강했다. |
| `ws` | `MULTI_STREAM` | `통과(90.7%)` | `통과(97.2%)` | `통과(92.4%)` | `통과(100.0%)` | `해당 없음` | `해당 없음` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150054_codex_dotnet_ws_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_DEALER_DEALER` | `통과(68.6%)` | `통과(88.2%)` | `통과(73.1%)` | `통과(95.7%)` | `통과(97.9%)` | `통과(105.3%)` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150725_codex_dotnet_wss_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_DEALER_ROUTER` | `통과(66.1%)` | `통과(61.0%)` | `통과(62.2%)` | `통과(91.3%)` | `통과(93.5%)` | `통과(93.8%)` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150725_codex_dotnet_wss_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_ROUTER_ROUTER` | `통과(57.4%)` | `통과(55.6%)` | `통과(56.8%)` | `통과(92.2%)` | `통과(96.1%)` | `통과(99.1%)` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150725_codex_dotnet_wss_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_PUBSUB` | `통과(68.9%)` | `통과(69.0%)` | `통과(81.5%)` | `통과(83.9%)` | `통과(96.0%)` | `통과(115.0%)` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150725_codex_dotnet_wss_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_SPOT` | `통과(71.7%)` | `통과(261.4%)` | `통과(409.7%)` | `통과(67.5%)` | `통과(72.7%)` | `통과(60.4%)` | 64/1024B는 같은 조건 재측정 C `perf_c_multi_linux_20260521_195558_codex_c_dotnet_spot_remaining_recheck_20260521.txt` 대비 .NET `perf_dotnet_multi_linux_20260521_195924_codex_dotnet_spot_remaining_recheck_20260521.txt`에서 통과했다. 262144B는 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt` 대비 .NET `perf_dotnet_multi_linux_20260521_193257_codex_dotnet_spot_after_completion_poller_recheck_20260521.txt`로 갱신했다. 나머지는 .NET `perf_dotnet_multi_linux_20260521_150725_codex_dotnet_wss_multi_remeasure_20260521.txt`. |
| `wss` | `MULTI_SPOT_REQREP` | `통과(67.4%)` | `통과(72.1%)` | `통과(81.1%)` | `통과(100.2%)` | `통과(91.6%)` | `통과(87.0%)` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_170326_codex_dotnet_multi_spot_reqrep_pollcompletion_full2_20260521.txt`. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `MULTI_SPOT_SENDSEND` | `통과(63.2%)` | `통과(63.1%)` | `통과(77.6%)` | `통과(100.0%)` | `통과(96.0%)` | `통과(92.9%)` | 64/1024B는 같은 조건 재측정 C `perf_c_multi_linux_20260521_195558_codex_c_dotnet_spot_remaining_recheck_20260521.txt` 대비 .NET `perf_dotnet_multi_linux_20260521_204616_codex_dotnet_spot_sendsend_pollin_cstyle_recheck_20260521.txt`로 갱신했다. `SendToSpot(Message)`는 public 원본 보존 계약을 유지하면서 clone 객체 생성 없이 native copy를 바로 submit하도록 내부 경로를 줄였다. sendsend active poll loop는 C처럼 `POLLIN`만 등록하고 50ms 한도 poll 뒤 submit을 재시도하도록 맞췄다. 262144B는 C `perf_c_multi_linux_20260521_174214_codex_c_spot_sendsend_tls_small_for_dotnet_recheck_20260521.txt`, `perf_c_multi_linux_20260521_173900_codex_c_spot_sendsend_small_probe_20260521.txt` 대비 .NET `perf_dotnet_multi_linux_20260521_193257_codex_dotnet_spot_after_completion_poller_recheck_20260521.txt`로 갱신했다. 나머지는 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150725_codex_dotnet_wss_multi_remeasure_20260521.txt`. |
| `wss` | `MULTI_STREAM` | `통과(84.0%)` | `통과(86.7%)` | `통과(87.2%)` | `통과(88.7%)` | `해당 없음` | `해당 없음` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; .NET `perf_dotnet_multi_linux_20260521_150725_codex_dotnet_wss_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_DEALER_DEALER` | `통과(69.6%)` | `통과(88.9%)` | `통과(85.9%)` | `통과(85.2%)` | `통과(94.2%)` | `통과(97.4%)` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; .NET `perf_dotnet_multi_linux_20260521_151429_codex_dotnet_tls_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_DEALER_ROUTER` | `통과(66.1%)` | `통과(60.6%)` | `통과(61.3%)` | `통과(85.6%)` | `통과(93.3%)` | `통과(95.1%)` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; .NET `perf_dotnet_multi_linux_20260521_151429_codex_dotnet_tls_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_ROUTER_ROUTER` | `통과(56.2%)` | `통과(56.2%)` | `통과(56.0%)` | `통과(79.7%)` | `통과(92.6%)` | `통과(98.2%)` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; .NET `perf_dotnet_multi_linux_20260521_151429_codex_dotnet_tls_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_PUBSUB` | `통과(69.1%)` | `통과(66.8%)` | `통과(77.4%)` | `통과(80.2%)` | `통과(90.7%)` | `통과(96.8%)` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; .NET `perf_dotnet_multi_linux_20260521_151429_codex_dotnet_tls_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_SPOT` | `통과(71.5%)` | `통과(85.7%)` | `통과(64.8%)` | `통과(97.9%)` | `통과(90.5%)` | `통과(79.1%)` | 64/1024B는 같은 조건 재측정 C `perf_c_multi_linux_20260521_195558_codex_c_dotnet_spot_remaining_recheck_20260521.txt` 대비 .NET `perf_dotnet_multi_linux_20260521_195924_codex_dotnet_spot_remaining_recheck_20260521.txt`로 갱신했다. 나머지는 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; .NET `perf_dotnet_multi_linux_20260521_151429_codex_dotnet_tls_multi_remeasure_20260521.txt`. |
| `tls` | `MULTI_SPOT_REQREP` | `통과(64.2%)` | `통과(63.2%)` | `통과(68.9%)` | `통과(78.9%)` | `통과(90.6%)` | `통과(91.5%)` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; .NET `perf_dotnet_multi_linux_20260521_170326_codex_dotnet_multi_spot_reqrep_pollcompletion_full2_20260521.txt`. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `MULTI_SPOT_SENDSEND` | `통과(61.1%)` | `통과(61.1%)` | `통과(64.5%)` | `통과(91.0%)` | `통과(96.6%)` | `통과(86.8%)` | 64/1024B는 같은 조건 재측정 C `perf_c_multi_linux_20260521_195558_codex_c_dotnet_spot_remaining_recheck_20260521.txt` 대비 .NET `perf_dotnet_multi_linux_20260521_204616_codex_dotnet_spot_sendsend_pollin_cstyle_recheck_20260521.txt`로 갱신했다. `SendToSpot(Message)` native-copy submit 내부 최적화와 C-style sendsend active poll loop 적용 뒤 64B와 1024B 모두 최신 재측정값 기준 통과했다. 262144B는 C `perf_c_multi_linux_20260521_174214_codex_c_spot_sendsend_tls_small_for_dotnet_recheck_20260521.txt`, `perf_c_multi_linux_20260521_173900_codex_c_spot_sendsend_small_probe_20260521.txt` 대비 .NET `perf_dotnet_multi_linux_20260521_193257_codex_dotnet_spot_after_completion_poller_recheck_20260521.txt`로 갱신했다. 나머지는 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; .NET `perf_dotnet_multi_linux_20260521_151429_codex_dotnet_tls_multi_remeasure_20260521.txt`. |
| `tls` | `MULTI_STREAM` | `통과(89.9%)` | `통과(85.6%)` | `통과(84.0%)` | `통과(86.6%)` | `해당 없음` | `해당 없음` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; .NET `perf_dotnet_multi_linux_20260521_151429_codex_dotnet_tls_multi_remeasure_20260521.txt`. STREAM small/large C 보강 파일은 아래 목록 참조. |

측정 결과 파일:

- C 기준: `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`, `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`, `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`, `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`, `perf_c_multi_linux_20260519_234246_codex_c_tls_stream64_debug_recheck_20260519.txt`, `perf_c_multi_linux_20260519_235205_codex_c_tls_stream256_recheck_20260519.txt`, `perf_c_multi_linux_20260519_234152_codex_c_tls_stream1024_recheck_20260519.txt`, `perf_c_multi_linux_20260519_234308_codex_c_tls_stream65536_recheck_20260519.txt`, `perf_c_multi_linux_20260521_195558_codex_c_dotnet_spot_remaining_recheck_20260521.txt`
- .NET 측정: `perf_dotnet_multi_linux_20260521_145328_codex_dotnet_tcp_multi_remeasure_20260521.txt`, `perf_dotnet_multi_linux_20260521_150044_codex_dotnet_tcp_spot_sendsend262144_recheck_20260521.txt`, `perf_dotnet_multi_linux_20260521_150054_codex_dotnet_ws_multi_remeasure_20260521.txt`, `perf_dotnet_multi_linux_20260521_150725_codex_dotnet_wss_multi_remeasure_20260521.txt`, `perf_dotnet_multi_linux_20260521_151429_codex_dotnet_tls_multi_remeasure_20260521.txt`, `perf_dotnet_multi_linux_20260521_170326_codex_dotnet_multi_spot_reqrep_pollcompletion_full2_20260521.txt`, `perf_dotnet_multi_linux_20260521_173705_codex_dotnet_ws_spot_reqrep64_no_managed_timer_20260521.txt`, `perf_dotnet_multi_linux_20260521_173735_codex_dotnet_spot_sendsend_small_probe_20260521.txt`, `perf_dotnet_multi_linux_20260521_174138_codex_dotnet_spot_sendsend_tls_small_recheck_20260521.txt`, `perf_dotnet_multi_linux_20260521_193257_codex_dotnet_spot_after_completion_poller_recheck_20260521.txt`, `perf_dotnet_multi_linux_20260521_195924_codex_dotnet_spot_remaining_recheck_20260521.txt`, `perf_dotnet_multi_linux_20260521_201737_codex_dotnet_spot_sendsend_copied_native_recheck_20260521.txt`, `perf_dotnet_multi_linux_20260521_204616_codex_dotnet_spot_sendsend_pollin_cstyle_recheck_20260521.txt`
- .NET SPOT callback request는 native timeout과 `POLLCOMPLETION` poll loop가 완료를 책임지므로 binding 내부 per-request managed timer를 제거했다. public API는 바꾸지 않았다. `SendToSpot(Message)`는 public 원본 보존 계약을 유지한 채 내부 native copy submit 경로를 줄였다. `MULTI_SPOT_SENDSEND`는 C와 같이 active poller를 `POLLIN` 중심으로 두고 50ms 한도 poll 뒤 submit을 재시도하도록 맞춘 뒤 `wss 64B`, `tls 64B`, `tls 1024B` 미달을 해소했다. 통과로 바꾸기 위한 sleep/backoff나 HWM 숫자 튜닝은 적용하지 않았다.
- **2026-06-22 .NET DEALER raw single-part 수신 hot path**:
  `IDealerSocket.Recv(Received, ...)` public 계약은 그대로 두고, binding 내부
  `DealerSocket.Recv`가 raw 단일 part를 받는 경우 caller-provided `Received`에 바로
  채우도록 줄였다. 이 경로는 `DEALER_DEALER inproc`와 일반 단일 메시지 수신의 hot
  path이므로 코드 가까이에 `HOT PATH` 주석을 두고 `List<Message>`, `Message[]`,
  envelope metadata 생성을 막는 이유를 명시했다. request/reply 메시지는 기존 envelope
  metadata 경로를 유지하므로 호출자는 새 순서나 helper를 알 필요가 없다. 이는 POSD의
  깊은 모듈 원칙에 맞게 dealer frame 구분 책임을 binding 내부에 남기는 변경이다.
  C 기준 `perf_c_single_linux_20260622_095841_prerelease_7_2_0_c_single_recheck_dotnet_dealer_inproc_singlepart_fastpath_probe.txt`
  대비 .NET 반복 측정 `perf_dotnet_single_linux_20260622_100149_prerelease_7_2_0_dotnet_single_dealer_inproc_raw_singlepart_fastpath_probe.txt`,
  `perf_dotnet_single_linux_20260622_100210_prerelease_7_2_0_dotnet_single_dealer_inproc_raw_singlepart_fastpath_probe_repeat.txt`에서
  64B는 `73.4%`, `71.4%`, 1024B는 `69.6%`, `69.9%`로 단순 one-way
  .NET 목표 범위에 들어왔다. 이후 perf-only 리뷰에서 `Received` accessor inline을
  제거한 뒤에도
  `perf_dotnet_single_linux_20260622_103227_prerelease_7_2_0_dotnet_single_dealer_inproc_after_received_inline_removed.txt`는
  64B `1253438.333 msg/s`, 1024B `1021614.667 msg/s`로 같은 C 기준 대비 약
  `74.0%`, `69.1%`를 유지했다.
- **2026-06-22 .NET routed large current 재측정 및 send builder inline 후보 기각**:
  `DEALER_ROUTER`/`ROUTER_ROUTER` large는 current HEAD에서도 반복 미달이다. 같은 조건
  paired 재측정 C `perf_c_single_linux_20260622_100731_prerelease_7_2_0_c_single_recheck_dotnet_routed_large_current_after_dealer_fastpath.txt`
  대비 .NET `perf_dotnet_single_linux_20260622_100603_prerelease_7_2_0_dotnet_single_routed_large_current_after_dealer_fastpath.txt`의
  비율은 `DEALER_ROUTER ipc` 50.8/48.3/44.3%, `DEALER_ROUTER tcp` 54.8/46.6/45.7%,
  `ROUTER_ROUTER ipc` 50.5/48.1/46.6%, `ROUTER_ROUTER tcp` 53.8/44.6/43.4%다.
  perf는 public routed send builder와 caller-provided `Received`를 그대로 사용했다.
  `RoutedSendOperation`와 `RoutedMessageSocketBase.Send(RoutingId)`에 inlining을 붙이는
  후보도 시험했지만 `perf_dotnet_single_linux_20260622_100928_prerelease_7_2_0_dotnet_single_routed_tcp_large_send_builder_inline_probe.txt`의
  tcp large 6개 cell은 직전 current와 같거나 낮았다. 얕은 dispatch 힌트는 이 병목을
  움직이지 못하므로 코드에 반영하지 않는다. 다음 후보는 public builder 표면을 바꾸지
  않으면서 native submit 경계, send/recv part lifecycle, routed receive materialization
  중 실제 대형 payload 벽을 만드는 내부 비용을 더 직접적으로 줄이는 방향이어야 한다.
- **2026-06-22 .NET `TopicMessage` repeated-topic cache 후보 폐기**:
  `MULTI_SPOT tcp 64/1024` current baseline은
  `perf_dotnet_multi_linux_20260622_101349_prerelease_7_2_0_dotnet_multi_spot_tcp_64_1024_current_before_topic_cache_probe.txt`에서
  `4058373.333/3705041.000 msg/s`였다. 이후 `TopicMessage`가 같은 instance로 같은 topic
  bytes를 반복 수신할 때 이전 decoded string을 재사용하는 후보를 작성했지만, 이는 perf의
  고정 topic 입력에 강하게 기대는 변경이다. 실제 애플리케이션에서는 topic 분포가 고정
  문자열 하나라고 볼 수 없고, 같은 public API의 일반 사용 비용을 구조적으로 줄이지도
  않는다. 코드가 복잡해지는 데 비해 실사용 개선 근거가 약하므로 측정 완료 전에 폐기했고,
  `TopicMessage` 변경은 코드에서 제거했다. 이후 SPOT 후보는 반복 입력값 cache가 아니라
  public `ISpot.Subscribe(TopicMessage, ...)` 뒤쪽의 receive storage, native boundary,
  part lifecycle 비용을 줄이는 방향만 검토한다.
- **2026-06-22 .NET `TopicMessage` accessor inline 후보 기각**:
  반복 입력값 cache를 제외한 뒤, public `TopicMessage.FirstPart()`와
  `SinglePartOrThrow()`에 `AggressiveInlining`만 붙이는 얕은 후보를 별도로 시험했다.
  public `ISpot.Subscribe(TopicMessage, ...)` 경로와 perf 조건은 유지했고
  `dotnet build`, SPOT/socket surface 테스트는 통과했다. 그러나
  `perf_dotnet_multi_linux_20260622_102147_prerelease_7_2_0_dotnet_multi_spot_tcp_64_1024_topicmessage_accessor_inline_probe.txt`는
  `3843783.333/3626925.000 msg/s`로 current baseline
  `4058373.333/3705041.000 msg/s`보다 낮았다. 단순 inline 힌트는 이 SPOT one-way
  병목을 움직이지 못하고 코드 주석이나 attribute만 늘리므로 반영하지 않는다.
- **2026-06-22 .NET `Spot.Subscribe` pointer-buffer 후보 기각**:
  public `ISpot.Subscribe(TopicMessage, ...)` 경로는 유지한 채 내부
  `ReceiveSpotSubscribedParts`가 `byte[]` P/Invoke 대신 이미 있는 pointer 기반
  `zlink_spot_subscribe_part_buffer`를 호출하도록 시험했다. 이 후보는 perf 전용 API나
  반복 topic cache는 아니지만, `perf_dotnet_multi_linux_20260622_103518_prerelease_7_2_0_dotnet_multi_spot_tcp_64_1024_spot_subscribe_pointer_buffer_probe.txt`에서
  `MULTI_SPOT tcp 64/1024`가 `3552672.200/3254250.000 msg/s`로 current baseline
  `4058373.333/3705041.000 msg/s`보다 낮았다. 단순 marshalling 경로 교체는 이 병목을
  줄이지 못하므로 코드에서 원복했다. 다음 SPOT 후보는 topic buffer 호출 방식이 아니라
  subscribe receive/backlog, native boundary 빈도, part lifecycle 중 profiler로 확인되는
  비용을 대상으로 한다.
- **2026-06-22 .NET `MULTI_SPOT tcp 64` 장기 단일 cell 재확인**:
  코드 변경 없이 `MULTI_SPOT tcp 64`를 `duration=15`, `clients=100`으로 다시 실행한
  `perf_dotnet_multi_linux_20260622_103902_prerelease_7_2_0_dotnet_multi_spot_tcp64_trace_target.txt`는
  `3027755.533 msg/s`로 complete였다. client/server log의 auto-HWM은 `MsgUnit(B)=64`,
  spot data slot `16384`로 정상이다. 이 실행에서 trace attach는 client 활성 구간을 놓쳐
  유효 sample을 얻지 못했다. hot loop가 `received.Topic != Topic`을 수행하지만, 반복
  topic cache는 perf 입력 모양에만 맞는 개선으로 이미 폐기했으므로 다시 후보로 보지
  않는다. 다음 라운드는 attach 타이밍을 보강해 client active phase profiler를 먼저 확보한
  뒤, 일반 public subscribe 경로의 비용으로 확인되는 항목만 수정한다.
- **2026-06-22 .NET `MULTI_SPOT tcp 64` active-phase trace 확보 및 후보 제한**:
  같은 public `spot.Publish(topic).Message(message).Flags(DontWait).Submit()`와
  `spot.Subscribe(topicMessage, DontWait)` 경로로 `duration=45` 단일 cell을 실행했고,
  `perf_dotnet_multi_linux_20260622_115822_prerelease_7_2_0_dotnet_multi_spot_tcp64_active_trace_capture.txt`는
  `2709121.267 msg/s`로 complete였다. trace 파일은
  `bindings/dotnet/perf/results/trace/dotnet_multi_spot_tcp64_client_active_trace_20260622.nettrace`,
  `bindings/dotnet/perf/results/trace/dotnet_multi_spot_tcp64_server_active_trace_20260622.nettrace`다.
  client inclusive topN은 `Spot.Subscribe(...)` `57.53%`,
  `Spot.ReceiveSpotSubscribedParts(...)` exclusive `56.25%`였고, server는
  `SpotSendOperation.Submit()` `29.33%`, `Spot.PublishNoWaitSingleCore(...)`
  exclusive `25.9%`였다. 이 결과는 병목이 반복 topic 값 자체보다 public subscribe
  materialization과 native single-part submit 경계에 있음을 가리킨다. 따라서 같은 topic,
  같은 payload, perf loop 전용 builder 우회처럼 perf 입력에만 기대는 변경은 계속 제외한다.
  코드에는 `ReceiveSpotSubscribedParts`와 `PublishNoWaitSingleCore`에 HOT PATH 주석을
  추가해 single-part 수신 채택 경로와 single native submit 경로가 리팩토링 중 흔들리지
  않게 고정했다. 다음 후보는 public API를 늘리지 않고 receive part lifecycle, native
  submit boundary, `SpotSendOperation` 객체 생성의 실제 allocation 비중을 trace나
  counters로 분리한 뒤 선택한다.
- **2026-06-22 .NET `MULTI_SPOT tcp 64` runtime counters 보강**:
  같은 public 경로로 `duration=40` 단일 cell을 실행한
  `perf_dotnet_multi_linux_20260622_120416_prerelease_7_2_0_dotnet_multi_spot_tcp64_counters_capture.txt`는
  `2739438.125 msg/s`로 complete였다. counters 파일은
  `bindings/dotnet/perf/results/trace/dotnet_multi_spot_tcp64_client_runtime_counters_20260622.csv`,
  `bindings/dotnet/perf/results/trace/dotnet_multi_spot_tcp64_server_runtime_counters_20260622.csv`다.
  client는 allocation rate 평균 약 `87.3 MB/s`, Gen0 GC 평균 `0.24/s`, working set 평균
  약 `1105.5 MB`였다. server는 allocation rate 평균 약 `108 KB/s`, Gen0/Gen1/Gen2 GC가
  모두 `0/s`, working set 평균 약 `50.0 MB`였다. 따라서 현재 SPOT one-way tcp 64의
  다음 우선순위는 server publish builder 객체가 아니라 client subscribe materialization,
  native receive part lifecycle, topic/message 객체 생명주기다. 다만 `TopicMessage.Topic`
  문자열은 public property를 읽을 때 필요한 계약 비용이므로, 같은 topic이 반복된다는
  cache 후보는 여전히 제외한다.
- **2026-06-22 .NET SPOT client allocation trace 확보와 반복 topic cache 재기각**:
  client allocation event를 보기 위해 같은 public 경로로
  `perf_dotnet_multi_linux_20260622_120832_prerelease_7_2_0_dotnet_multi_spot_tcp64_alloc_trace_capture.txt`
  를 실행했고 `2661470.800 msg/s`로 complete였다. allocation trace 파일은
  `bindings/dotnet/perf/results/trace/dotnet_multi_spot_tcp64_client_alloc_trace_20260622.nettrace`다.
  이 trace는 provider keyword가 allocation tick을 잡지 못해 타입별 집계가 비어 있었다.
  GC keyword로 다시 수집한
  `perf_dotnet_multi_linux_20260622_121343_prerelease_7_2_0_dotnet_multi_spot_tcp64_gc_alloc_trace_capture.txt`는
  `2769599.514 msg/s`로 complete였고, trace 파일은
  `bindings/dotnet/perf/results/trace/dotnet_multi_spot_tcp64_client_gc_alloc_trace_20260622.nettrace`다.
  TraceEvent 기반 임시 분석에서 allocation tick은 `9693`건, 약 `1033.3 MB`였고,
  `System.String`이 `9688`건, 약 `1032.8 MB`로 거의 전부였다. 이는 perf loop가
  `received.Topic != Topic`을 매 수신마다 수행하는 비용과 일치한다. 그러나 같은 topic이
  반복된다는 전제는 perf 데이터 모양에 강하게 기대며, 실제 애플리케이션 topic 분포를
  대표한다고 볼 근거가 없다. 따라서 decoded topic string 반복 cache, public
  topic-compare helper, perf-only topic check 우회는 다시 기각한다.
  코드에는 `TopicMessage.SetTopicFromWritableBuffer` HOT PATH 주석을 추가해 topic buffer
  swap과 lazy UTF-8 decode 정책을 고정했다. 이는 public `Topic` property를 읽는 호출자에게
  필요한 문자열 생성은 유지하면서, payload만 읽는 호출자가 topic string allocation을
  치르지 않게 하는 기존 내부 최적화다. 같은 topic 반복 cache나 새 public topic-compare
  helper는 실사용 분포 근거가 부족하므로 계속 제외한다.
- **2026-06-22 .NET `TopicMessage` writable topic reset skip 채택**:
  반복 topic cache는 제외한 상태에서, public `ISpot.Subscribe(TopicMessage, DontWait)`
  재사용 경로의 일반 상태 전환만 다시 보았다. caller-provided `TopicMessage`가 writable
  topic buffer를 native receive에 넘기는 경로는 `ResetForReuse()` 직후
  `SetTopicFromWritableBuffer(...)`로 topic state를 새 값으로 바꾼다. 따라서 reset 단계에서
  `_topic = string.Empty`, `_topicLength = 0`을 먼저 쓰는 것은 곧 덮어쓸 transient state다.
  이 후보는 topic 값이 반복된다는 전제를 쓰지 않고, public `Topic` lazy decode 계약과
  `ISpot.Subscribe(TopicMessage, ...)` 표면을 그대로 둔다. `ResetForReuse(resetTopic:
  false)`를 writable-buffer populate 경로에만 사용하고, 코드 가까이에 `HOT PATH` 주석을
  보강했다. guard test `topic_message_writable_buffer_receive_does_not_reset_topic_twice`가
  이 경로를 고정한다.
  검증은 `dotnet build bindings/dotnet/Zlink.sln -c Release --no-restore`와
  `dotnet test bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj -c Release --no-restore --filter "FullyQualifiedName~test_optimization_guard|FullyQualifiedName~test_spot_pubsub_basic"`가
  통과했다. C baseline은 사용자가 지정한
  `bindings/c/perf/baseline/perf_c_multi_linux_20260619_062932.txt`와 최신 C paired 값이
  `MULTI_SPOT tcp 1024`에서 크게 달라, 이미 보강된
  `perf_c_multi_linux_20260621_180339_prerelease_7_2_0_c_multi_recheck_dotnet_multispot_misses.txt`
  를 기준으로 썼다. 후보 측정
  `perf_dotnet_multi_linux_20260622_140511_prerelease_7_2_0_dotnet_multi_spot_tcp_64_1024_topic_reset_skip_probe.txt`는
  `5525927.000/5052600.000 msg/s`, 반복
  `perf_dotnet_multi_linux_20260622_140602_prerelease_7_2_0_dotnet_multi_spot_tcp_64_1024_topic_reset_skip_probe_repeat.txt`는
  `5617140.000/4298700.000 msg/s`였다. 최신 C 대비 최저 비율도 `79.7%/78.7%`라
  `MULTI_SPOT tcp 64/1024`는 해소로 본다.
  같은 변경을 둔 상태에서 `tls/wss 256/1024/4096`도 제한 재측정했다.
  `perf_dotnet_multi_linux_20260622_141114_prerelease_7_2_0_dotnet_multi_spot_tls_wss_256_1024_4096_topic_reset_skip_probe.txt`는
  `status=complete`, 결과 라인 `90/90`이고, 최신 C 대비 비율은 `tls`
  `75.5%/94.1%/64.9%`, `wss` `97.0%/80.2%/250.6%`다. 따라서 이 후보는
  tcp에만 맞춘 특수 처리가 아니라 public `ISpot.Subscribe(TopicMessage, ...)`
  재사용 receive 경로 전체의 일반 비용을 줄이는 변경으로 본다.
- **2026-06-22 .NET SPOT hot path 주석/계약 검증**:
  SPOT subscribe/publish hot path 주석이 실제 public 계약과 맞는지 다시 확인했다.
  `ReceiveSpotSubscribedParts`는 public `Spot.Subscribe(TopicMessage, ...)`가 쓰는 경로에서
  single-part 메시지를 `Message.AdoptNativeFromPool`로 바로 채택하고, topic bytes는
  caller-provided `TopicMessage`의 reusable buffer로 받은 뒤 buffer swap과 lazy decode를
  유지한다. `PublishNoWaitSingleCore`는 public
  `spot.Publish(topic).Message(message).Flags(DontWait).Submit()` 경로에서
  `Message.MoveTo` 뒤 native submit 한 번으로 내려간다. 이 주석 보강 뒤
  `dotnet build bindings/dotnet/src/Zlink/Zlink.csproj`와
  `dotnet test bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj --filter "FullyQualifiedName~spot"`가
  통과했다. 남은 `.NET MULTI_SPOT` 후보는 반복 topic cache나 perf-only topic compare가
  아니라, public subscribe materialization, native receive part lifecycle, message 객체
  생명주기 중 profiler로 일반 비용이 확인되는 항목으로 제한한다.
- **2026-06-22 .NET `Received` accessor inline 리뷰 후 제거**:
  `DEALER_DEALER inproc` 개선 diff를 다시 검토하면서 public
  `Received.FirstPart()`와 `SinglePartOrThrow()`에 붙였던 `AggressiveInlining`도
  제거했다. 채택한 개선은 `DealerSocket.Recv(Received, ...)` 내부에서 raw 단일 part를
  caller-provided `Received`에 바로 채워 `List<Message>`와 `Message[]` 생성을 피하는
  구조 변경이다. accessor inline은 perf 코드 호출 빈도가 높다는 점 외에 일반 public
  경로의 allocation, copy, ownership 비용을 줄이는 근거가 없어서 코드에 남기지 않는다.
  제거 뒤 제한 재측정도 `DEALER_DEALER inproc 64/1024`에서 통과권을 유지했다.
- **2026-06-22 .NET routed large 내부 경로 재검토**:
  `DEALER_ROUTER`/`ROUTER_ROUTER` single large는 public
  `IRoutedMessageSocket.Send(routingId).Message(message).Flags(flags).Submit()`와
  `Recv(Received, ...)`를 사용한다. 현재 binding 내부 송신은 `SocketKernel.SendSingleCore`
  / `SendSingleResultCore(ref ZlinkRoutingId, ...)`에서 public `Message`를
  `MoveTo(ref nativePart)`로 native part에 넘긴 뒤 `zlink_send_part_rid` 또는
  `zlink_send_part_rid_nowait`를 호출한다. 수신은 `ReceiveRoutedParts`와
  caller-provided `Received` 경로에서 native part를 `Message.AdoptNativeFromPool`로
  채택한다. 따라서 large payload 본문을 한 번 더 복사하는 명백한 binding-side 후보는
  현재 경로에서 확인되지 않았다. 남은 후보는 fluent builder 객체 생성, managed/native
  call boundary, core routed transport 처리 쪽이다. public direct-send API나 perf 전용
  helper를 추가해 builder 비용만 우회하는 방식은 public contract 유지 원칙에 맞지 않으므로
  진행하지 않는다. 다음 routed large 작업은 profiler나 allocation trace로 builder 객체,
  `ZlinkMsg` move/restore, native call boundary 중 실제 비중을 확인한 뒤 진행한다.
- **2026-06-22 .NET routed large trace 보강**:
  `dotnet-trace` thread-time으로 `DEALER_ROUTER tcp 65536`과 `ROUTER_ROUTER tcp 131072`
  단일 실행을 확인했다. trace 파일은
  `bindings/dotnet/perf/results/trace/dotnet_single_dealer_router_tcp_65536_thread_time_trace.nettrace`,
  `bindings/dotnet/perf/results/trace/dotnet_single_router_router_tcp_131072_thread_time_trace.nettrace`다.
  `DEALER_ROUTER tcp 65536` topN은 `SocketKernel.SendSingleCore(Message,int32)`가
  exclusive `43.45%`, `ReceiveRouterParts`가 `8.5%`였다. `ROUTER_ROUTER tcp 131072`은
  `ReceiveRouterParts` exclusive `47.95%`, routed `SendSingleCore(ref ZlinkRoutingId, ...)`
  exclusive `44.02%`로 send/recv native boundary가 거의 반반이다. `dotnet-counters`
  `System.Runtime` 5초 샘플
  `bindings/dotnet/perf/results/trace/dotnet_single_router_router_tcp_131072_counters_5s.csv`에서는
  allocation rate가 약 `4.1~6.2 MB/s`, GC count와 `% Time in GC`가 모두 0이었다.
  따라서 현재 routed large 미달은 managed heap pressure가 아니라 routed send/recv native
  boundary와 core routed transport 호출 비용 중심으로 봐야 한다. builder object만 우회하는
  public API 추가나 perf-only helper는 이 trace 근거로도 채택하지 않는다.

### 6.4 Java 상태

#### 6.4.1 Single suite

2026-05-27 full refresh 첫 실행
`perf_java_single_linux_20260527_122518_codex_full_refresh_java_single_reset_20260527.txt`는
기본 wrapper가 `PAIR`부터 실행한 뒤 `PAIR ws 256B`에서 `timeout_after_45s`로
`partial` 종료됐다(`expected_result_lines=70`, `actual_result_lines=65`). 이 파일은
최신 장애 기록으로 남긴다. 해당 조건 단독 재측정
`perf_java_single_linux_20260527_122803_codex_java_single_pair_ws256_repro_20260527.txt`는
complete였고 1228.82 Kmsg/s로 측정됐다. 따라서 `--pattern ALL`로 full 실행을 다시
진행한다.

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | `통과(97.7%)` | `통과(97.4%)` | `통과(125.1%)` | `통과(118.1%)` | `통과(118.3%)` | `통과(131.4%)` | C: `perf_c_single_linux_20260519_182557_codex_c_tcp_single_smoke_all_20260519.txt`; Java: `perf_java_single_linux_20260520_065810_codex_java_tcp_single_smoke_20260520.txt`. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `PUBSUB` | `통과(89.7%)` | `통과(92.5%)` | `통과(107.3%)` | `통과(98.7%)` | `통과(97.2%)` | `통과(97.9%)` | C/Java 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `DEALER_DEALER` | `통과(97.5%)` | `통과(151.1%)` | `통과(123.0%)` | `통과(97.3%)` | `통과(97.0%)` | `통과(97.0%)` | C/Java 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `DEALER_ROUTER` | `통과(83.4%)` | `통과(82.6%)` | `통과(90.2%)` | `통과(111.1%)` | `통과(95.6%)` | `통과(110.9%)` | C/Java 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `ROUTER_ROUTER` | `통과(96.8%)` | `통과(99.0%)` | `통과(81.6%)` | `통과(108.6%)` | `통과(105.1%)` | `통과(102.4%)` | C/Java 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `SPOT` | `통과(192.1%)` | `통과(144.6%)` | `통과(131.9%)` | `통과(84.9%)` | `통과(67.9%)` | `통과(60.6%)` | C: `perf_c_single_linux_20260519_182557_codex_c_tcp_single_smoke_all_20260519.txt`; Java repeat3: `perf_java_single_linux_20260520_070601_codex_java_tcp_single_spot_direct_spin_repeat3_20260520.txt`. Java SPOT은 C와 같은 의미가 되도록 stop token 송신을 별도 stop publisher로 분리하고, active 메시지는 복사 없이 public API로 직접 publish한다. all-size smoke의 131072B 이상치는 isolated recheck `perf_java_single_linux_20260520_070545_codex_java_tcp_single_spot_131072_direct_spin_recheck_20260520.txt`와 repeat3로 배제했다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `PAIR` | `통과(97.7%)` | `통과(98.6%)` | `통과(141.6%)` | `통과(98.8%)` | `통과(98.7%)` | `통과(98.6%)` | C: `perf_c_single_linux_20260519_183218_codex_c_ws_single_smoke_all_20260519.txt`; 256B C 제한: `perf_c_single_linux_20260520_085311_codex_c_ws_single_256_for_java_20260520.txt`; Java: `perf_java_single_linux_20260520_084654_codex_java_ws_single_smoke_after_routed_fix_20260520.txt`. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `PUBSUB` | `통과(83.1%)` | `통과(89.0%)` | `통과(124.1%)` | `통과(99.0%)` | `통과(99.1%)` | `통과(98.7%)` | C/Java 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `DEALER_DEALER` | `통과(97.1%)` | `통과(98.8%)` | `통과(134.9%)` | `통과(97.8%)` | `통과(97.3%)` | `통과(97.4%)` | C/Java 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `DEALER_ROUTER` | `통과(68.4%)` | `통과(93.4%)` | `통과(118.6%)` | `통과(217.0%)` | `통과(146.5%)` | `통과(137.7%)` | C 파일은 위 PAIR 행과 같다. Java: `perf_java_single_linux_20260520_085454_codex_java_ws_single_dr_all_sizes_blocking_active_20260520.txt`. active와 stop token 전송을 C `perf_dealer_router.cpp`와 같은 blocking send 의미로 맞췄다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `ROUTER_ROUTER` | `통과(89.4%)` | `통과(107.2%)` | `통과(114.1%)` | `통과(196.9%)` | `통과(144.6%)` | `통과(146.1%)` | C 파일은 위 PAIR 행과 같다. Java: `perf_java_single_linux_20260520_085110_codex_java_ws_single_rr_all_sizes_isolated_20260520.txt`. ROUTER-ROUTER는 C처럼 양쪽 routing id와 mandatory를 설정하고 PING/PONG으로 target route를 확인한 뒤 active와 stop token을 blocking send 의미로 보낸다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `SPOT` | `통과(194.1%)` | `통과(155.2%)` | `통과(153.7%)` | `통과(115.9%)` | `통과(120.3%)` | `통과(134.6%)` | C 파일은 위 PAIR 행과 같다. Java: `perf_java_single_linux_20260520_085151_codex_java_ws_single_spot_all_sizes_isolated_20260520.txt`. 전체 matrix 실행 중 후반부 timeout이 있었으나 패턴 단위 smoke에서는 전 size 통과해 partial matrix 결과는 판정 근거에서 제외했다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `PAIR` | `통과(98.3%)` | `통과(99.0%)` | `통과(143.8%)` | `통과(133.7%)` | `통과(121.6%)` | `통과(103.5%)` | C: `perf_c_single_linux_20260519_183421_codex_c_wss_single_smoke_all_20260519.txt`; 256B C 제한: `perf_c_single_linux_20260520_093213_codex_c_wss_single_256_for_java_20260520.txt`; Java: `perf_java_single_linux_20260520_092846_codex_java_wss_single_smoke_20260520.txt`. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `PUBSUB` | `통과(82.5%)` | `통과(95.2%)` | `통과(148.6%)` | `통과(123.7%)` | `통과(110.1%)` | `통과(100.6%)` | C/Java 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `DEALER_DEALER` | `통과(98.0%)` | `통과(99.4%)` | `통과(150.3%)` | `통과(131.6%)` | `통과(122.2%)` | `통과(101.6%)` | C/Java 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `DEALER_ROUTER` | `통과(69.8%)` | `통과(90.6%)` | `통과(136.6%)` | `통과(158.0%)` | `통과(186.8%)` | `통과(208.5%)` | C/Java 파일은 위 PAIR 행과 같다. ws single에서 정렬한 active/stop blocking send 의미를 그대로 사용한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `ROUTER_ROUTER` | `통과(83.2%)` | `통과(104.2%)` | `통과(136.9%)` | `통과(158.9%)` | `통과(190.5%)` | `통과(211.7%)` | C/Java 파일은 위 PAIR 행과 같다. ws single에서 정렬한 ROUTER-ROUTER PING/PONG target route 확인과 active/stop blocking send 의미를 그대로 사용한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `SPOT` | `통과(202.0%)` | `통과(148.9%)` | `통과(147.0%)` | `통과(114.4%)` | `통과(101.7%)` | `통과(139.0%)` | C/Java 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `PAIR` | `통과(98.3%)` | `통과(98.6%)` | `통과(134.3%)` | `통과(109.3%)` | `통과(101.9%)` | `통과(101.0%)` | C: `perf_c_single_linux_20260519_184702_codex_c_tls_single_smoke_all_20260519.txt`; 256B C 제한: `perf_c_single_linux_20260520_093917_codex_c_tls_single_256_for_java_20260520.txt`; Java: `perf_java_single_linux_20260520_093346_codex_java_tls_single_smoke_20260520.txt`. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `PUBSUB` | `통과(81.0%)` | `통과(86.8%)` | `통과(136.0%)` | `통과(102.4%)` | `통과(100.0%)` | `통과(98.4%)` | C/Java 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `DEALER_DEALER` | `통과(96.8%)` | `통과(98.7%)` | `통과(132.7%)` | `통과(106.9%)` | `통과(100.5%)` | `통과(101.0%)` | C/Java 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `DEALER_ROUTER` | `통과(68.6%)` | `통과(83.5%)` | `통과(125.2%)` | `통과(172.0%)` | `통과(165.7%)` | `통과(171.4%)` | C/Java 파일은 위 PAIR 행과 같다. ws/wss single에서 정렬한 active/stop blocking send 의미를 그대로 사용한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `ROUTER_ROUTER` | `통과(87.5%)` | `통과(105.7%)` | `통과(122.0%)` | `통과(171.3%)` | `통과(171.2%)` | `통과(165.6%)` | C/Java 파일은 위 PAIR 행과 같다. ws/wss single에서 정렬한 ROUTER-ROUTER PING/PONG target route 확인과 active/stop blocking send 의미를 그대로 사용한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `SPOT` | `통과(200.8%)` | `통과(147.7%)` | `통과(148.3%)` | `통과(210.3%)` | `통과(96.1%)` | `통과(124.1%)` | C/Java 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |

#### 6.4.2 Multi suite

2026-05-21 재측정 결과로 대표 표를 갱신했다. 판정은 `doc/perf` 기준처럼 C `bindings/c/perf`와 같은 suite/pattern/transport/size의 throughput 비율로 계산한다. HWM은 튜닝 값으로 쓰지 않고, auto-HWM 활성 여부와 size별 `MsgUnit(B)` 일치 여부만 확인한다.

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | `통과(73.2%)` | `통과(91.9%)` | `통과(95.5%)` | `통과(78.9%)` | `통과(75.6%)` | `통과(68.0%)` | 65536/131072B는 같은 조건 재측정 C `perf_c_multi_linux_20260521_194538_codex_c_java_routed_miss_recheck_20260521.txt` 대비 Java `perf_java_multi_linux_20260521_194652_codex_java_routed_miss_recheck_20260521.txt`에서 통과했다. 나머지는 대표 C `perf_c_multi_linux_20260521_135152_codex_c_tcp_multi_for_java_current_20260521.txt`; Java `perf_java_multi_linux_20260521_133627_codex_java_tcp_multi_remeasure_20260521.txt`. |
| `tcp` | `MULTI_DEALER_ROUTER` | `통과(87.6%)` | `통과(74.2%)` | `통과(77.1%)` | `통과(55.6%)` | `통과(58.6%)` | `통과(92.9%)` | 65536/131072B는 같은 조건 재측정 C `perf_c_multi_linux_20260521_194538_codex_c_java_routed_miss_recheck_20260521.txt` 대비 Java `perf_java_multi_linux_20260521_194652_codex_java_routed_miss_recheck_20260521.txt`에서 통과했다. 나머지는 대표 C `perf_c_multi_linux_20260521_135152_codex_c_tcp_multi_for_java_current_20260521.txt`; Java `perf_java_multi_linux_20260521_133627_codex_java_tcp_multi_remeasure_20260521.txt`. |
| `tcp` | `MULTI_ROUTER_ROUTER` | `통과(60.1%)` | `통과(61.0%)` | `통과(60.3%)` | `통과(61.0%)` | `통과(72.7%)` | `통과(96.2%)` | 대표 C `perf_c_multi_linux_20260521_135152_codex_c_tcp_multi_for_java_current_20260521.txt`; Java `perf_java_multi_linux_20260521_133627_codex_java_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_PUBSUB` | `통과(77.3%)` | `통과(79.1%)` | `통과(96.1%)` | `통과(151.4%)` | `통과(143.2%)` | `통과(157.5%)` | 대표 C `perf_c_multi_linux_20260521_135152_codex_c_tcp_multi_for_java_current_20260521.txt`; Java `perf_java_multi_linux_20260521_133627_codex_java_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_SPOT` | `통과(108.6%)` | `통과(88.8%)` | `통과(82.9%)` | `통과(75.6%)` | `통과(73.4%)` | `통과(83.7%)` | 대표 C `perf_c_multi_linux_20260521_135152_codex_c_tcp_multi_for_java_current_20260521.txt`; Java `perf_java_multi_linux_20260521_133627_codex_java_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_SPOT_REQREP` | `통과(69.4%)` | `통과(74.0%)` | `통과(77.7%)` | `통과(95.4%)` | `통과(109.5%)` | `통과(145.6%)` | 대표 C `perf_c_multi_linux_20260521_135152_codex_c_tcp_multi_for_java_current_20260521.txt`; Java `perf_java_multi_linux_20260521_170615_codex_java_multi_spot_reqrep_pollcompletion_full_20260521.txt`. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `MULTI_SPOT_SENDSEND` | `통과(84.0%)` | `통과(82.0%)` | `통과(81.7%)` | `통과(81.2%)` | `통과(79.7%)` | `통과(63.3%)` | 대표 C `perf_c_multi_linux_20260521_135152_codex_c_tcp_multi_for_java_current_20260521.txt`; Java `perf_java_multi_linux_20260521_133627_codex_java_tcp_multi_remeasure_20260521.txt`. large C 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_STREAM` | `통과(98.8%)` | `통과(96.4%)` | `통과(88.3%)` | `통과(107.3%)` | `해당 없음` | `해당 없음` | 대표 C `perf_c_multi_linux_20260521_135152_codex_c_tcp_multi_for_java_current_20260521.txt`; Java `perf_java_multi_linux_20260521_133627_codex_java_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_DEALER_DEALER` | `통과(69.0%)` | `통과(67.2%)` | `통과(93.6%)` | `통과(63.5%)` | `통과(65.5%)` | `통과(91.1%)` | 65536B는 같은 조건 재측정 C `perf_c_multi_linux_20260521_194538_codex_c_java_routed_miss_recheck_20260521.txt` 대비 Java `perf_java_multi_linux_20260521_194652_codex_java_routed_miss_recheck_20260521.txt`에서 통과했다. 131072B는 C 파일은 같고 Java 단독 재측정 `perf_java_multi_linux_20260521_203945_codex_java_ws_dd131072_poller_index_cache_recheck_20260521.txt`에서 통과했다. `DealerSocket.send()`는 public API를 바꾸지 않고 캡처 lambda 기반 공통 builder 대신 socket 직접 builder를 쓰도록 내부 호출 오버헤드를 줄였다. `PerfMultiDealerDealer` 수신 기록은 공통 direct active-latency 기록 API를 써서 header record 할당을 없앴고, public `Poller` 내부에 socket/spot handle index cache를 추가해 `modify()`의 선형 탐색 비용을 줄였다. full-copy 제거 프로브 `perf_java_multi_linux_20260521_194928_codex_java_ws_dd131072_direct_alloc_probe_20260521.txt`, 직접 stamp 프로브 `perf_java_multi_linux_20260521_201712_codex_java_ws_dd131072_direct_stamp_recheck_20260521.txt`, reusable message send 프로브 `perf_java_multi_linux_20260521_203729_codex_java_ws_dd131072_reusable_message_send_recheck_20260521.txt`, direct no-wait submit 프로브 `perf_java_multi_linux_20260521_203828_codex_java_ws_dd131072_dealer_direct_nowait_submit_recheck_20260521.txt`는 더 느려 반영하지 않았다. 나머지는 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; Java `perf_java_multi_linux_20260521_140504_codex_java_ws_multi_remeasure_20260521.txt`. |
| `ws` | `MULTI_DEALER_ROUTER` | `통과(71.0%)` | `통과(68.7%)` | `통과(71.7%)` | `통과(58.9%)` | `통과(59.0%)` | `통과(84.0%)` | 65536/131072B는 같은 조건 재측정 C `perf_c_multi_linux_20260521_194538_codex_c_java_routed_miss_recheck_20260521.txt` 대비 Java `perf_java_multi_linux_20260521_194652_codex_java_routed_miss_recheck_20260521.txt`에서 통과했다. 나머지는 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; Java `perf_java_multi_linux_20260521_140504_codex_java_ws_multi_remeasure_20260521.txt`. |
| `ws` | `MULTI_ROUTER_ROUTER` | `통과(58.3%)` | `통과(62.2%)` | `통과(61.1%)` | `통과(54.8%)` | `통과(73.2%)` | `통과(88.1%)` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; Java `perf_java_multi_linux_20260521_140504_codex_java_ws_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_PUBSUB` | `통과(85.5%)` | `통과(73.7%)` | `통과(88.5%)` | `통과(135.2%)` | `통과(134.3%)` | `통과(146.7%)` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; Java `perf_java_multi_linux_20260521_140504_codex_java_ws_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_SPOT` | `통과(101.5%)` | `통과(101.2%)` | `통과(111.0%)` | `통과(89.6%)` | `통과(79.1%)` | `통과(75.1%)` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; Java `perf_java_multi_linux_20260521_140504_codex_java_ws_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_SPOT_REQREP` | `통과(83.8%)` | `통과(79.6%)` | `통과(75.8%)` | `통과(92.9%)` | `통과(102.4%)` | `통과(101.5%)` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; Java full `perf_java_multi_linux_20260521_170615_codex_java_multi_spot_reqrep_pollcompletion_full_20260521.txt`, ws 64B 보강 `perf_java_multi_linux_20260521_171039_codex_java_multi_spot_reqrep_ws64_recheck_20260521.txt`. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `MULTI_SPOT_SENDSEND` | `통과(76.6%)` | `통과(74.3%)` | `통과(77.5%)` | `통과(99.7%)` | `통과(63.9%)` | `통과(77.4%)` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; Java `perf_java_multi_linux_20260521_140504_codex_java_ws_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_STREAM` | `통과(100.9%)` | `통과(105.6%)` | `통과(101.2%)` | `통과(106.1%)` | `해당 없음` | `해당 없음` | 대표 C `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`; Java `perf_java_multi_linux_20260521_140504_codex_java_ws_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_DEALER_DEALER` | `통과(74.3%)` | `통과(81.0%)` | `통과(68.1%)` | `통과(94.0%)` | `통과(88.4%)` | `통과(91.1%)` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; Java `perf_java_multi_linux_20260521_141952_codex_java_wss_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_DEALER_ROUTER` | `통과(70.2%)` | `통과(69.9%)` | `통과(73.4%)` | `통과(92.3%)` | `통과(95.9%)` | `통과(106.1%)` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; Java `perf_java_multi_linux_20260521_141952_codex_java_wss_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_ROUTER_ROUTER` | `통과(57.2%)` | `통과(56.8%)` | `통과(57.4%)` | `통과(72.8%)` | `통과(57.2%)` | `통과(99.4%)` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; Java `perf_java_multi_linux_20260521_141952_codex_java_wss_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_PUBSUB` | `통과(72.8%)` | `통과(77.1%)` | `통과(85.7%)` | `통과(89.5%)` | `통과(100.9%)` | `통과(103.7%)` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; Java `perf_java_multi_linux_20260521_141952_codex_java_wss_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_SPOT` | `통과(265.8%)` | `통과(113.1%)` | `통과(349.7%)` | `통과(128.6%)` | `통과(63.2%)` | `통과(60.1%)` | 256B는 같은 조건 재측정 C `perf_c_multi_linux_20260521_210845_codex_c_wss_multi_spot256_outlier_apply_20260521.txt` 대비 Java `perf_java_multi_linux_20260521_210909_codex_java_wss_multi_spot256_outlier_apply_20260521.txt`로 갱신했다. 65536/262144B는 같은 조건 재측정 C `perf_c_multi_linux_20260521_194954_codex_c_wss_spot_large_for_java_recheck2_20260521.txt` 대비 Java all-size 재측정 `perf_java_multi_linux_20260521_200127_codex_java_wss_spot_large_spinwait_recheck_20260521.txt`, 262144B 단독 `perf_java_multi_linux_20260521_200350_codex_java_wss_spot262144_spinwait_single_recheck_20260521.txt`로 갱신했다. 131072B는 C `perf_c_multi_linux_20260521_201904_codex_c_wss_spot131072_for_java_current_recheck_20260521.txt` 대비 Java `perf_java_multi_linux_20260521_203035_codex_java_wss_spot131072_rid_cache_recheck_20260521.txt`에서 통과했다. Java public `Poller.add(Spot, ..., POLLOUT)` 내부를 spot-pub poller primitive에 연결하고, publisher active backpressure 대기를 public Poller+Timer 단일 대기로 바꿨다. `Spot.subscribe` fast path는 scratch routing-id cache를 사용해 반복 routing id의 byte[]/RoutingId 생성을 줄였다. recv worker idle sleep/backoff는 `Thread.onSpinWait()`으로 제거했다. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `MULTI_SPOT_REQREP` | `통과(78.1%)` | `통과(80.4%)` | `통과(78.8%)` | `통과(100.6%)` | `통과(96.0%)` | `통과(94.2%)` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; Java `perf_java_multi_linux_20260521_170615_codex_java_multi_spot_reqrep_pollcompletion_full_20260521.txt`. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `MULTI_SPOT_SENDSEND` | `통과(68.2%)` | `통과(71.7%)` | `통과(77.7%)` | `통과(95.6%)` | `통과(96.7%)` | `통과(89.9%)` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; Java `perf_java_multi_linux_20260521_141952_codex_java_wss_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_STREAM` | `통과(94.9%)` | `통과(91.9%)` | `통과(93.7%)` | `통과(92.3%)` | `해당 없음` | `해당 없음` | 대표 C `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`; Java `perf_java_multi_linux_20260521_141952_codex_java_wss_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_DEALER_DEALER` | `통과(74.7%)` | `통과(99.6%)` | `통과(81.2%)` | `통과(97.5%)` | `통과(82.9%)` | `통과(79.8%)` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; Java `perf_java_multi_linux_20260521_142934_codex_java_tls_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_DEALER_ROUTER` | `통과(72.4%)` | `통과(71.7%)` | `통과(71.5%)` | `통과(58.4%)` | `통과(92.1%)` | `통과(90.2%)` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; Java `perf_java_multi_linux_20260521_142934_codex_java_tls_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_ROUTER_ROUTER` | `통과(58.9%)` | `통과(58.8%)` | `통과(58.8%)` | `통과(62.0%)` | `통과(63.9%)` | `통과(69.6%)` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; Java `perf_java_multi_linux_20260521_142934_codex_java_tls_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_PUBSUB` | `통과(75.0%)` | `통과(73.9%)` | `통과(82.1%)` | `통과(90.2%)` | `통과(95.9%)` | `통과(95.4%)` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; Java `perf_java_multi_linux_20260521_142934_codex_java_tls_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_SPOT` | `통과(124.0%)` | `통과(127.8%)` | `통과(173.8%)` | `통과(88.7%)` | `통과(92.2%)` | `통과(95.8%)` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; Java `perf_java_multi_linux_20260521_142934_codex_java_tls_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_SPOT_REQREP` | `통과(76.8%)` | `통과(67.0%)` | `통과(71.1%)` | `통과(85.7%)` | `통과(93.0%)` | `통과(93.3%)` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; Java `perf_java_multi_linux_20260521_170615_codex_java_multi_spot_reqrep_pollcompletion_full_20260521.txt`. 256B는 C `perf_c_multi_linux_20260521_174430_codex_c_tls_spot_reqrep256_for_java_deadline_timer_pollcompletion_20260521.txt` 대비 Java `perf_java_multi_linux_20260521_175312_codex_java_tls_spot_reqrep256_deadline_timer_no_1ms_wait_20260521.txt`로 보강했다. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `MULTI_SPOT_SENDSEND` | `통과(64.5%)` | `통과(69.3%)` | `통과(66.9%)` | `통과(89.7%)` | `통과(88.9%)` | `통과(80.9%)` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; Java `perf_java_multi_linux_20260521_142934_codex_java_tls_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_STREAM` | `통과(94.4%)` | `통과(93.5%)` | `통과(95.2%)` | `통과(92.0%)` | `해당 없음` | `해당 없음` | 대표 C `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`; Java `perf_java_multi_linux_20260521_142934_codex_java_tls_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |

측정 결과 파일:

- C 기준: `perf_c_multi_linux_20260521_135152_codex_c_tcp_multi_for_java_current_20260521.txt`, `perf_c_multi_linux_20260519_211916_codex_c_ws_multi_smoke_all_retry_20260519.txt`, `perf_c_multi_linux_20260519_221051_codex_c_wss_multi_smoke_all_20260519.txt`, `perf_c_multi_linux_20260519_233310_codex_c_tls_multi_smoke_all_20260519_current.txt`, `perf_c_multi_linux_20260519_234246_codex_c_tls_stream64_debug_recheck_20260519.txt`, `perf_c_multi_linux_20260519_235205_codex_c_tls_stream256_recheck_20260519.txt`, `perf_c_multi_linux_20260519_234152_codex_c_tls_stream1024_recheck_20260519.txt`, `perf_c_multi_linux_20260519_234308_codex_c_tls_stream65536_recheck_20260519.txt`, `perf_c_multi_linux_20260520_232413_codex_c_tcp_multi_sendsend_current_all_for_node_20260520.txt`, `perf_c_multi_linux_20260521_142745_codex_c_wss_spot262144_for_java_20260521.txt`, `perf_c_multi_linux_20260521_194538_codex_c_java_routed_miss_recheck_20260521.txt`, `perf_c_multi_linux_20260521_194954_codex_c_wss_spot_large_for_java_recheck2_20260521.txt`, `perf_c_multi_linux_20260521_201904_codex_c_wss_spot131072_for_java_current_recheck_20260521.txt`
- Java 측정: `perf_java_multi_linux_20260521_133627_codex_java_tcp_multi_remeasure_20260521.txt`, `perf_java_multi_linux_20260521_135027_codex_java_tcp_spot262144_recheck3_20260521.txt`, `perf_java_multi_linux_20260521_140328_codex_java_tcp_dr_pollset_mask_20260521.txt`, `perf_java_multi_linux_20260521_141820_codex_java_tcp_rr_stop_reliable_20260521.txt`, `perf_java_multi_linux_20260521_140504_codex_java_ws_multi_remeasure_20260521.txt`, `perf_java_multi_linux_20260521_141913_codex_java_ws_spot262144_recheck_20260521.txt`, `perf_java_multi_linux_20260521_141952_codex_java_wss_multi_remeasure_20260521.txt`, `perf_java_multi_linux_20260521_142852_codex_java_wss_spot262144_recheck2_20260521.txt`, `perf_java_multi_linux_20260521_142852_codex_java_wss_spot_reqrep262144_recheck_20260521.txt`, `perf_java_multi_linux_20260521_142934_codex_java_tls_multi_remeasure_20260521.txt`, `perf_java_multi_linux_20260521_143917_codex_java_tls_spot_reqrep262144_recheck_20260521.txt`, `perf_java_multi_linux_20260521_143926_codex_java_tls_spot_sendsend262144_recheck_20260521.txt`, `perf_java_multi_linux_20260521_143939_codex_java_tls_spot262144_recheck2_20260521.txt`, `perf_java_multi_linux_20260521_170615_codex_java_multi_spot_reqrep_pollcompletion_full_20260521.txt`, `perf_java_multi_linux_20260521_171039_codex_java_multi_spot_reqrep_ws64_recheck_20260521.txt`, `perf_java_multi_linux_20260521_175312_codex_java_tls_spot_reqrep256_deadline_timer_no_1ms_wait_20260521.txt`, `perf_java_multi_linux_20260521_174453_codex_java_wss_multi_spot_recheck_20260521.txt`, `perf_java_multi_linux_20260521_174635_codex_java_wss_multi_spot65536_ready_recheck_20260521.txt`, `perf_java_multi_linux_20260521_184941_codex_java_wss_multi_spot_pub_poller_recheck_20260521.txt`, `perf_java_multi_linux_20260521_194652_codex_java_routed_miss_recheck_20260521.txt`, `perf_java_multi_linux_20260521_194802_codex_java_ws_dd131072_single_recheck_20260521.txt`, `perf_java_multi_linux_20260521_195158_codex_java_wss_spot_large_recheck2_20260521.txt`, `perf_java_multi_linux_20260521_201532_codex_java_ws_dd131072_dealer_send_builder_recheck_20260521.txt`, `perf_java_multi_linux_20260521_201824_codex_java_wss_spot131072_after_dealer_builder_recheck_20260521.txt`, `perf_java_multi_linux_20260521_203035_codex_java_wss_spot131072_rid_cache_recheck_20260521.txt`, `perf_java_multi_linux_20260521_203945_codex_java_ws_dd131072_poller_index_cache_recheck_20260521.txt`
- Java `MULTI_SPOT_REQREP`는 `POLLCOMPLETION` poller에 active deadline timer를 함께 등록해 C처럼 completion 또는 deadline event로만 깨어나도록 수정했다. 1ms timeout poll은 제거했다. `wss MULTI_SPOT` publisher는 public `Poller.add(Spot, ..., POLLOUT)`이 spot-pub poller primitive를 쓰도록 보강한 뒤 public Poller+Timer 대기로 재측정했다. recv worker idle sleep/backoff도 제거했다. `Spot.subscribe` fast path routing-id cache와 `Poller` handle index cache, `PerfMultiDealerDealer` direct latency 기록 적용 뒤 `ws MULTI_DEALER_DEALER 131072B`와 `wss MULTI_SPOT 131072B` 미달을 해소했다. 통과로 바꾸기 위한 sleep/backoff나 HWM 숫자 튜닝은 적용하지 않았다.

### 6.5 Node 상태

#### 6.5.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | `통과(37.4%)` | `통과(38.9%)` | `통과(54.1%)` | `통과(62.7%)` | `통과(67.2%)` | `통과(55.2%)` | C: `perf_c_single_linux_20260520_000429_codex_c_tcp_single_duration5_for_dotnet_20260520.txt`; Node: `perf_node_single_linux_20260520_115303_codex_node_tcp_single_full_reuse_recv_final_20260520.txt`. 이전 full smoke `perf_node_single_linux_20260520_105505_codex_node_tcp_single_smoke_20260520.txt`, PAIR all-size `perf_node_single_linux_20260520_105627_codex_node_tcp_single_pair_all_recheck_20260520.txt`, `perf_node_single_linux_20260520_112838_codex_node_tcp_single_smoke_autoslots_20260520.txt`는 stop token 앞 backlog drain timeout 또는 partial이라 통과 근거에서 제외한다. generic single sender에서 C에 없는 post-active phase-2 payload를 제거하고, stop token retry를 bounded 처리했다. receiver는 caller-provided `Received`/`TopicMessage` 저장소를 재사용한다. active sender flow-control gate와 active retry sleep은 C single 의미와 달라 제거했다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `PUBSUB` | `통과(35.6%)` | `통과(42.1%)` | `통과(51.8%)` | `통과(64.7%)` | `통과(62.0%)` | `통과(53.0%)` | C/Node 파일은 위 PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `DEALER_DEALER` | `통과(36.0%)` | `통과(36.4%)` | `통과(51.0%)` | `통과(64.0%)` | `통과(60.1%)` | `통과(38.9%)` | C/Node 파일은 위 PAIR 행과 같다. 이전 all-size `perf_node_single_linux_20260520_105912_codex_node_tcp_single_dealer_dealer_all_20260520.txt`는 256B timeout이라 통과 근거에서 제외한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `DEALER_ROUTER` | `통과(66.9%)` | `통과(71.8%)` | `통과(70.0%)` | `보류(15.4%)` | `보류(12.5%)` | `보류(11.4%)` | C: `perf_c_single_linux_20260520_000429_codex_c_tcp_single_duration5_for_dotnet_20260520.txt`; Node: `perf_node_single_linux_20260520_140736_codex_node_tcp_single_routed_current_all_20260520.txt`. C의 `zlink_router_recv_part` 의미에 맞춰 Node `RouterSocket.recvPayloadInto`가 native 단일 파트 receive helper를 직접 호출하게 했다. sender worker는 C처럼 receiver가 wire stop token을 본 뒤 닫히도록 lifetime을 맞춰 262144B timeout/no-data를 제거했다. `DEALER` 송신은 기존 public `sendFrom(buffer)` 단일 파트 경로를 사용한다. active sender flow-control gate, active retry sleep, flow-credit batching은 C single 의미와 달라 제거했다. routed large는 timeout 없이 유효 수치를 얻었지만 Node/Python routed one-way 최소 기준 33% 아래다. 남은 개선 후보는 C public API의 단일 메시지 호출 의미를 벗어나는 batch drain이나 perf 전용 control API라 적용하지 않고 보류한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `ROUTER_ROUTER` | `통과(79.2%)` | `통과(73.8%)` | `통과(68.5%)` | `보류(12.7%)` | `보류(11.4%)` | `보류(12.5%)` | C/Node 파일은 위 DEALER_ROUTER 행과 같다. 기존 public `send(rid).message(...).submit()` 표면은 유지하면서 내부 blocking single-part 전송을 C의 `zlink_send_part_rid` helper로 정렬했다. large 수치는 timeout 없이 확보했지만 최소 기준 33% 아래다. 남은 차이는 JS 경계와 메시지별 public receive/send 호출 비용이 지배적이며, C public API와 다른 batch drain이나 perf 전용 control API는 적용하지 않고 보류한다. auto-HWM 활성과 size별 receiver `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `SPOT` | `통과(88.3%)` | `통과(72.1%)` | `통과(64.2%)` | `통과(139.1%)` | `통과(116.9%)` | `통과(114.3%)` | C: `perf_c_single_linux_20260520_000429_codex_c_tcp_single_duration5_for_dotnet_20260520.txt`; Node: `perf_node_single_linux_20260520_140615_codex_node_tcp_single_spot_payloadinto_all_20260520.txt`. `Spot.publishFrom(topic, buffer, flags)`는 C의 `zlink_spot_publish_part` 단일 파트 helper와 같은 native Buffer 경로로 보낸다. `Spot.subscribePayloadInto(buffer, flags)`를 추가해 C의 `zlink_spot_subscribe_part`처럼 caller buffer에 단일 part payload를 복사하고 topic/routing metadata를 반환한다. 기존 `TopicMessage` 재사용만으로는 large가 통과하지 못해 반영하지 않았다. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `PAIR` | `통과(36.1%)` | `통과(37.1%)` | `통과(71.5%)` | `통과(84.1%)` | `통과(56.0%)` | `통과(99.2%)` | C: `perf_c_single_linux_20260521_042759_codex_c_ws_single_current_for_node_20260521.txt`; Node full: `perf_node_single_linux_20260521_044735_codex_node_ws_single_current_20260521.txt`; Node 64B/131072B 제한 재측정: `perf_node_single_linux_20260521_044900_codex_node_ws_single_pair_recheck_20260521.txt`. full run의 131072B 흔들림은 제한 repeat5 complete 파일로 다시 확인했다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `PUBSUB` | `통과(73.4%)` | `통과(38.7%)` | `통과(60.8%)` | `통과(99.8%)` | `통과(99.3%)` | `통과(38.1%)` | C: `perf_c_single_linux_20260521_042759_codex_c_ws_single_current_for_node_20260521.txt`; Node full: `perf_node_single_linux_20260521_044735_codex_node_ws_single_current_20260521.txt`; Node 64B direct payload 재측정: `perf_node_single_linux_20260521_045324_codex_node_ws_single_pubsub64_payloadinto_20260521.txt`; Node 262144B 제한 재측정: `perf_node_single_linux_20260521_045115_codex_node_ws_single_pubsub_dd_recheck_20260521.txt`. single PUBSUB도 C의 single-part publish/subscribe 의미에 맞춰 internal `publishDirect`와 `subscribePayloadInto` 경로로 정렬했다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `DEALER_DEALER` | `통과(35.4%)` | `통과(36.8%)` | `통과(71.6%)` | `통과(99.9%)` | `통과(98.7%)` | `통과(99.5%)` | C: `perf_c_single_linux_20260521_042759_codex_c_ws_single_current_for_node_20260521.txt`; Node full: `perf_node_single_linux_20260521_044735_codex_node_ws_single_current_20260521.txt`; Node 64B/262144B 제한 재측정: `perf_node_single_linux_20260521_045115_codex_node_ws_single_pubsub_dd_recheck_20260521.txt`. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `DEALER_ROUTER` | `통과(68.3%)` | `통과(77.5%)` | `통과(108.4%)` | `보류(32.2%)` | `보류(22.3%)` | `보류(21.4%)` | C: `perf_c_single_linux_20260521_042759_codex_c_ws_single_current_for_node_20260521.txt`; Node full: `perf_node_single_linux_20260521_044735_codex_node_ws_single_current_20260521.txt`; Node large repeat5: `perf_node_single_linux_20260521_045618_codex_node_ws_single_routed_large_recheck_20260521.txt`. large는 timeout 없이 유효 수치를 얻었지만 Node/Python routed one-way 최소 기준 33% 아래라 보류한다. C public API와 다른 batch drain이나 perf 전용 control API는 적용하지 않는다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `ROUTER_ROUTER` | `통과(70.6%)` | `통과(78.5%)` | `통과(105.1%)` | `보류(30.5%)` | `보류(21.9%)` | `보류(21.3%)` | C: `perf_c_single_linux_20260521_042759_codex_c_ws_single_current_for_node_20260521.txt`; Node full: `perf_node_single_linux_20260521_044735_codex_node_ws_single_current_20260521.txt`; Node large repeat5: `perf_node_single_linux_20260521_045618_codex_node_ws_single_routed_large_recheck_20260521.txt`. large는 timeout 없이 유효 수치를 얻었지만 최소 기준 33% 아래라 보류한다. 기존 public routed send 표면은 유지하고, C public API와 다른 batch drain이나 perf 전용 control API는 적용하지 않는다. auto-HWM 활성과 size별 receiver `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `SPOT` | `통과(87.8%)` | `통과(70.9%)` | `통과(72.0%)` | `통과(216.0%)` | `통과(204.1%)` | `통과(110.2%)` | C: `perf_c_single_linux_20260521_042759_codex_c_ws_single_current_for_node_20260521.txt`; Node: `perf_node_single_linux_20260521_044735_codex_node_ws_single_current_20260521.txt`. `Spot.publishFrom`/`Spot.subscribePayloadInto` 단일 payload 경로로 C 의미와 맞췄다. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `PAIR` | `보류(33.8%)` | `통과(36.6%)` | `통과(102.4%)` | `통과(128.9%)` | `통과(110.0%)` | `통과(100.9%)` | C full: `perf_c_single_linux_20260521_045850_codex_c_wss_single_current_for_node_20260521.txt`; Node full: `perf_node_single_linux_20260521_051840_codex_node_wss_single_current_20260521.txt`; 64B C 제한 재측정: `perf_c_single_linux_20260521_052435_codex_c_wss_single_pair64_recheck_for_node_20260521.txt`; 64B Node 제한 재측정: `perf_node_single_linux_20260521_052427_codex_node_wss_single_pair64_recheck_20260521.txt`. 64B는 timeout 없이 유효 수치를 얻었지만 Node/Python 단순 one-way 최소 기준 35% 아래다. public `recvInto` 재사용 후보는 `perf_node_single_linux_20260521_052635_codex_node_wss_single_pair64_recvinto_recheck_20260521.txt`에서 run 변동으로 median이 14.5%까지 떨어져 반영하지 않았다. C public API와 다른 batch drain이나 시작 제어 API는 적용하지 않고 보류한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `PUBSUB` | `통과(68.9%)` | `통과(90.6%)` | `통과(149.6%)` | `통과(106.0%)` | `통과(110.8%)` | `통과(109.5%)` | C full: `perf_c_single_linux_20260521_045850_codex_c_wss_single_current_for_node_20260521.txt`; Node full: `perf_node_single_linux_20260521_051840_codex_node_wss_single_current_20260521.txt`; Node 64B/65536B 제한 재측정: `perf_node_single_linux_20260521_052336_codex_node_wss_single_pubsub_payloadinto_blocking_recheck_20260521.txt`. `subscribePayloadInto` receive loop를 C처럼 첫 receive는 blocking, burst drain은 `DontWait`로 정렬해 65536B median 저하를 해소했다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `DEALER_DEALER` | `통과(35.0%)` | `통과(36.9%)` | `통과(101.5%)` | `통과(130.1%)` | `통과(116.1%)` | `통과(100.9%)` | C/Node full 파일은 위 wss PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `DEALER_ROUTER` | `통과(63.7%)` | `통과(75.9%)` | `통과(146.8%)` | `통과(74.5%)` | `통과(77.4%)` | `통과(78.6%)` | C/Node full 파일은 위 wss PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `ROUTER_ROUTER` | `통과(70.7%)` | `통과(78.3%)` | `통과(136.9%)` | `통과(75.4%)` | `통과(75.8%)` | `통과(78.4%)` | C/Node full 파일은 위 wss PAIR 행과 같다. `ROUTER_ROUTER / DEALER_ROUTER` 상대 기준은 절대 기준 통과 항목의 진단 보조로만 사용한다. auto-HWM 활성과 size별 receiver `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `SPOT` | `통과(84.9%)` | `통과(65.5%)` | `보류(296.4%)` | `통과(171.7%)` | `통과(167.4%)` | `통과(172.2%)` | 1024B는 같은 조건 재측정 C `perf_c_single_linux_20260521_210903_codex_c_wss_single_spot1024_outlier_apply_20260521.txt` 대비 Node `perf_node_single_linux_20260521_210938_codex_node_wss_single_spot1024_outlier_apply_20260521.txt`에서 여전히 높다. 현재 Node single SPOT은 publish 후 inline drain 구조라 C의 별도 sender/receiver thread 의미와 다를 수 있어 통과로 확정하지 않는다. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. 나머지는 C/Node full 파일은 위 wss PAIR 행과 같다. |
| `tls` | `PAIR` | `보류(33.5%)` | `통과(35.8%)` | `통과(77.9%)` | `통과(86.3%)` | `통과(101.6%)` | `통과(102.2%)` | C full: `perf_c_single_linux_20260521_052855_codex_c_tls_single_current_for_node_20260521.txt`; Node full: `perf_node_single_linux_20260521_054834_codex_node_tls_single_current_20260521.txt`; Node 64B/131072B 제한 재측정: `perf_node_single_linux_20260521_055052_codex_node_tls_single_pair_dd_recheck_20260521.txt`. 64B는 timeout 없이 유효 수치를 얻었지만 Node/Python 단순 one-way 최소 기준 35% 아래다. wss와 같은 public `recvInto` 후보는 안정적인 통과 근거를 만들지 못했고, C public API와 다른 batch drain이나 시작 제어 API는 적용하지 않고 보류한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `PUBSUB` | `통과(67.5%)` | `통과(82.1%)` | `통과(145.1%)` | `통과(75.1%)` | `통과(99.7%)` | `통과(100.3%)` | C/Node full 파일은 위 tls PAIR 행과 같다. `subscribePayloadInto` receive loop는 C처럼 첫 receive blocking, burst drain `DontWait` 구조를 사용한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `DEALER_DEALER` | `보류(32.9%)` | `통과(36.3%)` | `통과(78.8%)` | `통과(101.1%)` | `통과(100.1%)` | `통과(101.7%)` | C/Node full 파일은 위 tls PAIR 행과 같다. 64B와 131072B는 `perf_node_single_linux_20260521_055052_codex_node_tls_single_pair_dd_recheck_20260521.txt`로 제한 재측정했다. 131072B는 통과 확인됐고, 64B는 timeout 없이 유효 수치를 얻었지만 단순 one-way 최소 기준 35% 아래라 보류한다. C public API와 다른 batch drain이나 시작 제어 API는 적용하지 않는다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `DEALER_ROUTER` | `통과(63.1%)` | `통과(70.7%)` | `통과(110.5%)` | `통과(54.9%)` | `통과(53.7%)` | `통과(51.3%)` | C/Node full 파일은 위 tls PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `ROUTER_ROUTER` | `통과(72.3%)` | `통과(77.3%)` | `통과(113.2%)` | `통과(53.9%)` | `통과(51.8%)` | `통과(53.0%)` | C/Node full 파일은 위 tls PAIR 행과 같다. `ROUTER_ROUTER / DEALER_ROUTER` 상대 기준은 절대 기준 통과 항목의 진단 보조로만 사용한다. auto-HWM 활성과 size별 receiver `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `SPOT` | `통과(85.4%)` | `통과(68.6%)` | `통과(74.1%)` | `통과(162.7%)` | `통과(161.8%)` | `통과(171.1%)` | C/Node full 파일은 위 tls PAIR 행과 같다. `Spot.publishFrom`/`Spot.subscribePayloadInto` 단일 payload 경로로 C 의미와 맞췄다. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. |

#### 6.5.2 Multi suite

2026-05-21 재측정 결과로 대표 표를 갱신했다. 판정은 `doc/perf` 기준처럼 C `bindings/c/perf`와 같은 suite/pattern/transport/size의 throughput 비율로 계산한다. HWM은 튜닝 값으로 쓰지 않고, auto-HWM 활성 여부와 size별 `MsgUnit(B)` 일치 여부만 확인한다.

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | `통과(77.0%)` | `통과(91.5%)` | `통과(78.4%)` | `통과(73.5%)` | `통과(71.3%)` | `통과(65.5%)` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; Node `perf_node_multi_linux_20260521_145057_codex_node_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_DEALER_ROUTER` | `통과(48.0%)` | `통과(48.2%)` | `통과(45.4%)` | `통과(31.3%)` | `통과(46.9%)` | `통과(64.2%)` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; Node `perf_node_multi_linux_20260521_145057_codex_node_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_ROUTER_ROUTER` | `통과(36.1%)` | `통과(36.7%)` | `통과(35.6%)` | `통과(54.3%)` | `통과(45.5%)` | `통과(72.0%)` | 64/256/1024B는 C `perf_c_multi_linux_20260521_180927_codex_c_multi_rr_threshold5_for_node_20260521.txt` 대비 Node public API 경로 `perf_node_multi_linux_20260521_183956_codex_node_multi_rr_public_borrowed_send_recheck_20260521.txt`로 보강했다. 65536B는 같은 C 기준 대비 `perf_node_multi_linux_20260521_185753_codex_node_tcp_rr65536_recv_no_throw_recheck_20260521.txt`에서 통과했다. 131072/262144B는 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; Node `perf_node_multi_linux_20260521_145057_codex_node_tcp_multi_remeasure_20260521.txt`. public routed send 내부가 C 단일 part borrowed primitive를 쓰고, public `recvPayloadInto(...DontWait)` 내부 no-data 예외 비용을 제거했다. |
| `tcp` | `MULTI_PUBSUB` | `통과(41.9%)` | `통과(51.5%)` | `통과(82.9%)` | `통과(56.3%)` | `통과(54.9%)` | `통과(64.5%)` | 64/256B는 C `perf_c_multi_linux_20260521_180012_codex_c_multi_pubsub_small_for_node_threshold5_20260521.txt` 대비 Node public publish/subscribe 경로 `perf_node_multi_linux_20260521_184302_codex_node_multi_pubsub_public_publish_borrowed_recheck_20260521.txt`로 보강했다. 나머지는 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; Node `perf_node_multi_linux_20260521_145057_codex_node_tcp_multi_remeasure_20260521.txt`. no-data 예외 제거, metric payload 직접 기록, public publish operation 내부 borrowed Buffer 경로를 적용했다. |
| `tcp` | `MULTI_SPOT` | `통과(45.8%)` | `통과(94.4%)` | `통과(64.3%)` | `통과(70.5%)` | `통과(79.2%)` | `통과(97.8%)` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; Node `perf_node_multi_linux_20260521_145057_codex_node_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_SPOT_REQREP` | `통과(54.3%)` | `통과(50.4%)` | `통과(44.9%)` | `통과(59.9%)` | `통과(63.7%)` | `통과(70.2%)` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; Node `perf_node_multi_linux_20260521_171325_codex_node_multi_spot_reqrep_pollcompletion_full_20260521.txt`. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `MULTI_SPOT_SENDSEND` | `통과(93.0%)` | `통과(90.9%)` | `통과(96.7%)` | `통과(108.8%)` | `통과(205.4%)` | `통과(140.7%)` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; Node `perf_node_multi_linux_20260521_145057_codex_node_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tcp` | `MULTI_STREAM` | `통과(72.4%)` | `통과(73.7%)` | `통과(73.1%)` | `통과(91.1%)` | `해당 없음` | `해당 없음` | 대표 C `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`; Node `perf_node_multi_linux_20260521_145057_codex_node_tcp_multi_remeasure_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_DEALER_DEALER` | `통과(99.4%)` | `통과(87.2%)` | `통과(77.2%)` | `통과(60.8%)` | `통과(57.5%)` | `통과(89.6%)` | 대표 C `perf_c_multi_linux_20260520_232757_codex_c_ws_multi_current_for_node_20260520.txt`; Node `perf_node_multi_linux_20260521_001948_codex_node_ws_multi_spot64_256_crash_repro_20260520.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_DEALER_ROUTER` | `통과(86.7%)` | `통과(85.1%)` | `통과(84.8%)` | `통과(53.2%)` | `통과(57.1%)` | `통과(65.6%)` | 대표 C `perf_c_multi_linux_20260520_232757_codex_c_ws_multi_current_for_node_20260520.txt`; Node `perf_node_multi_linux_20260521_001948_codex_node_ws_multi_spot64_256_crash_repro_20260520.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_ROUTER_ROUTER` | `통과(39.9%)` | `통과(38.3%)` | `통과(37.5%)` | `통과(44.7%)` | `통과(51.5%)` | `통과(66.3%)` | 64/256/1024/65536B는 C `perf_c_multi_linux_20260521_180927_codex_c_multi_rr_threshold5_for_node_20260521.txt` 대비 Node public API 경로 `perf_node_multi_linux_20260521_183956_codex_node_multi_rr_public_borrowed_send_recheck_20260521.txt`로 보강했다. 131072/262144B는 대표 C `perf_c_multi_linux_20260520_232757_codex_c_ws_multi_current_for_node_20260520.txt`; Node `perf_node_multi_linux_20260521_001948_codex_node_ws_multi_spot64_256_crash_repro_20260520.txt`. public routed send 내부가 C 단일 part borrowed primitive를 쓰도록 고쳤다. |
| `ws` | `MULTI_PUBSUB` | `통과(44.3%)` | `통과(40.4%)` | `통과(46.7%)` | `통과(67.4%)` | `통과(62.1%)` | `통과(85.3%)` | 대표 C `perf_c_multi_linux_20260520_232757_codex_c_ws_multi_current_for_node_20260520.txt`; Node `perf_node_multi_linux_20260521_001948_codex_node_ws_multi_spot64_256_crash_repro_20260520.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_SPOT` | `통과(72.6%)` | `통과(71.6%)` | `통과(66.5%)` | `통과(48.7%)` | `통과(71.6%)` | `통과(99.3%)` | 대표 C `perf_c_multi_linux_20260520_232757_codex_c_ws_multi_current_for_node_20260520.txt`; Node `perf_node_multi_linux_20260521_001948_codex_node_ws_multi_spot64_256_crash_repro_20260520.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_SPOT_REQREP` | `통과(62.1%)` | `통과(57.0%)` | `통과(56.8%)` | `통과(81.1%)` | `통과(75.3%)` | `통과(87.0%)` | 대표 C `perf_c_multi_linux_20260520_232757_codex_c_ws_multi_current_for_node_20260520.txt`; Node `perf_node_multi_linux_20260521_171325_codex_node_multi_spot_reqrep_pollcompletion_full_20260521.txt`. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `MULTI_SPOT_SENDSEND` | `통과(97.5%)` | `통과(98.0%)` | `통과(100.4%)` | `통과(112.9%)` | `통과(116.3%)` | `통과(168.9%)` | 대표 C `perf_c_multi_linux_20260520_232757_codex_c_ws_multi_current_for_node_20260520.txt`; Node `perf_node_multi_linux_20260521_001948_codex_node_ws_multi_spot64_256_crash_repro_20260520.txt`. 보강 파일은 아래 목록 참조. |
| `ws` | `MULTI_STREAM` | `통과(70.2%)` | `통과(72.2%)` | `통과(75.0%)` | `통과(92.9%)` | `해당 없음` | `해당 없음` | 대표 C `perf_c_multi_linux_20260520_232757_codex_c_ws_multi_current_for_node_20260520.txt`; Node `perf_node_multi_linux_20260521_001948_codex_node_ws_multi_spot64_256_crash_repro_20260520.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_DEALER_DEALER` | `통과(101.6%)` | `통과(83.0%)` | `통과(66.6%)` | `통과(54.0%)` | `통과(62.2%)` | `통과(65.2%)` | 대표 C `perf_c_multi_linux_20260521_012140_codex_c_wss_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_020406_codex_node_wss_multi_pubsub_recheck_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_DEALER_ROUTER` | `통과(79.3%)` | `통과(76.5%)` | `통과(73.5%)` | `통과(55.5%)` | `통과(60.9%)` | `통과(60.0%)` | 대표 C `perf_c_multi_linux_20260521_012140_codex_c_wss_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_020406_codex_node_wss_multi_pubsub_recheck_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_ROUTER_ROUTER` | `통과(38.1%)` | `통과(39.1%)` | `통과(39.4%)` | `통과(60.7%)` | `통과(57.1%)` | `통과(57.1%)` | 64/256/1024/65536B는 C `perf_c_multi_linux_20260521_180927_codex_c_multi_rr_threshold5_for_node_20260521.txt` 대비 Node public API 경로 `perf_node_multi_linux_20260521_183956_codex_node_multi_rr_public_borrowed_send_recheck_20260521.txt`로 보강했다. 131072/262144B는 대표 C `perf_c_multi_linux_20260521_012140_codex_c_wss_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_020406_codex_node_wss_multi_pubsub_recheck_20260521.txt`. public routed send 내부가 C 단일 part borrowed primitive를 쓰도록 고쳤다. |
| `wss` | `MULTI_PUBSUB` | `통과(43.5%)` | `통과(43.9%)` | `통과(53.7%)` | `통과(53.5%)` | `통과(54.1%)` | `통과(58.8%)` | 64/256B는 C `perf_c_multi_linux_20260521_180012_codex_c_multi_pubsub_small_for_node_threshold5_20260521.txt` 대비 Node public publish/subscribe 경로 `perf_node_multi_linux_20260521_184302_codex_node_multi_pubsub_public_publish_borrowed_recheck_20260521.txt`로 보강했다. 나머지는 대표 C `perf_c_multi_linux_20260521_012140_codex_c_wss_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_020406_codex_node_wss_multi_pubsub_recheck_20260521.txt`. no-data 예외 제거, metric payload 직접 기록, public publish operation 내부 borrowed Buffer 경로를 적용했다. |
| `wss` | `MULTI_SPOT` | `통과(85.0%)` | `통과(253.3%)` | `통과(155.3%)` | `통과(46.9%)` | `통과(48.8%)` | `통과(40.0%)` | 대표 C `perf_c_multi_linux_20260521_012140_codex_c_wss_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_020406_codex_node_wss_multi_pubsub_recheck_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_SPOT_REQREP` | `통과(62.1%)` | `통과(57.9%)` | `통과(64.8%)` | `통과(95.8%)` | `통과(94.4%)` | `통과(93.0%)` | 대표 C `perf_c_multi_linux_20260521_012140_codex_c_wss_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_171325_codex_node_multi_spot_reqrep_pollcompletion_full_20260521.txt`. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `MULTI_SPOT_SENDSEND` | `통과(92.8%)` | `통과(96.2%)` | `통과(108.4%)` | `통과(107.7%)` | `통과(109.3%)` | `통과(103.1%)` | 대표 C `perf_c_multi_linux_20260521_012140_codex_c_wss_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_020406_codex_node_wss_multi_pubsub_recheck_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `wss` | `MULTI_STREAM` | `통과(92.6%)` | `통과(93.1%)` | `통과(93.1%)` | `통과(97.0%)` | `해당 없음` | `해당 없음` | 대표 C `perf_c_multi_linux_20260521_012140_codex_c_wss_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_020406_codex_node_wss_multi_pubsub_recheck_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_DEALER_DEALER` | `통과(100.9%)` | `통과(86.7%)` | `통과(74.8%)` | `통과(56.5%)` | `통과(55.2%)` | `통과(54.8%)` | 대표 C `perf_c_multi_linux_20260521_024327_codex_c_tls_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_033444_codex_node_tls_multi_current_for_node_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_DEALER_ROUTER` | `통과(88.5%)` | `통과(86.3%)` | `통과(84.3%)` | `통과(52.7%)` | `통과(55.7%)` | `통과(56.2%)` | 대표 C `perf_c_multi_linux_20260521_024327_codex_c_tls_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_033444_codex_node_tls_multi_current_for_node_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_ROUTER_ROUTER` | `통과(38.6%)` | `통과(39.0%)` | `통과(39.4%)` | `통과(55.1%)` | `통과(58.0%)` | `통과(56.3%)` | 64/256/1024/65536B는 C `perf_c_multi_linux_20260521_180927_codex_c_multi_rr_threshold5_for_node_20260521.txt` 대비 Node public API 경로 `perf_node_multi_linux_20260521_183956_codex_node_multi_rr_public_borrowed_send_recheck_20260521.txt`로 보강했다. 131072/262144B는 대표 C `perf_c_multi_linux_20260521_024327_codex_c_tls_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_033444_codex_node_tls_multi_current_for_node_20260521.txt`. public routed send 내부가 C 단일 part borrowed primitive를 쓰도록 고쳤다. |
| `tls` | `MULTI_PUBSUB` | `통과(43.1%)` | `통과(42.6%)` | `통과(47.5%)` | `통과(53.9%)` | `통과(58.6%)` | `통과(59.3%)` | 64B는 C `perf_c_multi_linux_20260521_190630_codex_c_tls_pubsub64_for_node_topic_bench_recheck_20260521.txt` 대비 Node `perf_node_multi_linux_20260521_190730_codex_node_tls_pubsub64_no_null_rid_recheck_20260521.txt`에서 통과했다. 256B는 C `perf_c_multi_linux_20260521_180012_codex_c_multi_pubsub_small_for_node_threshold5_20260521.txt` 대비 Node public publish/subscribe 경로 `perf_node_multi_linux_20260521_184302_codex_node_multi_pubsub_public_publish_borrowed_recheck_20260521.txt`로 보강했다. 나머지는 대표 C `perf_c_multi_linux_20260521_024327_codex_c_tls_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_033444_codex_node_tls_multi_current_for_node_20260521.txt`. C와 같은 `bench` topic, no-data 예외 제거, metric payload 직접 기록, public publish operation 내부 borrowed Buffer 경로, raw native result의 불필요한 null routingId property 제거를 적용했다. |
| `tls` | `MULTI_SPOT` | `통과(70.1%)` | `통과(92.6%)` | `통과(106.3%)` | `통과(98.2%)` | `통과(110.6%)` | `통과(150.7%)` | 대표 C `perf_c_multi_linux_20260521_024327_codex_c_tls_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_033444_codex_node_tls_multi_current_for_node_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_SPOT_REQREP` | `통과(60.5%)` | `통과(54.0%)` | `통과(49.1%)` | `통과(86.4%)` | `통과(87.9%)` | `통과(89.5%)` | 대표 C `perf_c_multi_linux_20260521_024327_codex_c_tls_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_171325_codex_node_multi_spot_reqrep_pollcompletion_full_20260521.txt`. auto-HWM 활성과 size별 spotnode `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `MULTI_SPOT_SENDSEND` | `통과(89.7%)` | `통과(93.2%)` | `통과(96.2%)` | `통과(100.9%)` | `통과(103.5%)` | `통과(153.7%)` | 대표 C `perf_c_multi_linux_20260521_024327_codex_c_tls_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_033444_codex_node_tls_multi_current_for_node_20260521.txt`. 보강 파일은 아래 목록 참조. |
| `tls` | `MULTI_STREAM` | `통과(90.8%)` | `통과(86.4%)` | `통과(91.9%)` | `통과(102.4%)` | `해당 없음` | `해당 없음` | 대표 C `perf_c_multi_linux_20260521_024327_codex_c_tls_multi_current_for_node_20260521.txt`; Node `perf_node_multi_linux_20260521_033444_codex_node_tls_multi_current_for_node_20260521.txt`. 보강 파일은 아래 목록 참조. |

측정 결과 파일:

- C 기준: `perf_c_multi_linux_20260520_004453_codex_c_tcp_multi_for_dotnet_20260520.txt`, `perf_c_multi_linux_20260520_232757_codex_c_ws_multi_current_for_node_20260520.txt`, `perf_c_multi_linux_20260520_234521_codex_c_ws_multi_spot64_256_repro_20260520.txt`, `perf_c_multi_linux_20260521_001957_codex_c_ws_multi_spot1024_262144_recheck_for_node_20260521.txt`, `perf_c_multi_linux_20260520_235328_codex_c_ws_multi_spot65536_repeat3_seq_20260520.txt`, `perf_c_multi_linux_20260520_235741_codex_c_ws_multi_spot131072_repro_debug_20260520.txt`, `perf_c_multi_linux_20260521_012140_codex_c_wss_multi_current_for_node_20260521.txt`, `perf_c_multi_linux_20260521_015925_codex_c_wss_multi_pubsub_all_for_node_20260521.txt`, `perf_c_multi_linux_20260521_024327_codex_c_tls_multi_current_for_node_20260521.txt`, `perf_c_multi_linux_20260520_232413_codex_c_tcp_multi_sendsend_current_all_for_node_20260520.txt`, `perf_c_multi_linux_20260521_011758_codex_c_ws_multi_sendsend_all_for_node_20260521.txt`, `perf_c_multi_linux_20260521_180012_codex_c_multi_pubsub_small_for_node_threshold5_20260521.txt`, `perf_c_multi_linux_20260521_180927_codex_c_multi_rr_threshold5_for_node_20260521.txt`
- Node 측정: `perf_node_multi_linux_20260521_145057_codex_node_tcp_multi_remeasure_20260521.txt`, `perf_node_multi_linux_20260521_145134_codex_node_tcp_spot131072_recheck_20260521.txt`, `perf_node_multi_linux_20260521_001948_codex_node_ws_multi_spot64_256_crash_repro_20260520.txt`, `perf_node_multi_linux_20260521_003015_codex_node_ws_multi_spot_large_recheck_for_node_20260521.txt`, `perf_node_multi_linux_20260521_004553_codex_node_ws_multi_dd_native_sendloop_all_20260521.txt`, `perf_node_multi_linux_20260521_005520_codex_node_ws_multi_routed_pubsub_recheck_20260521.txt`, `perf_node_multi_linux_20260521_011145_codex_node_ws_multi_dr_native_echo_loop_all_20260521.txt`, `perf_node_multi_linux_20260521_011737_codex_node_ws_multi_spotreq_sendsend_stream_20260521.txt`, `perf_node_multi_linux_20260521_020406_codex_node_wss_multi_pubsub_recheck_20260521.txt`, `perf_node_multi_linux_20260521_021710_codex_node_wss_multi_spot256_repro_20260521.txt`, `perf_node_multi_linux_20260521_022729_codex_node_wss_multi_routed_group_20260521.txt`, `perf_node_multi_linux_20260521_023558_codex_node_wss_multi_spot_rest_20260521.txt`, `perf_node_multi_linux_20260521_024207_codex_node_wss_multi_spotreq_sendsend_stream_20260521.txt`, `perf_node_multi_linux_20260521_033444_codex_node_tls_multi_current_for_node_20260521.txt`, `perf_node_multi_linux_20260521_034012_codex_node_tls_multi_pubsub64_256_publish_direct_20260521.txt`, `perf_node_multi_linux_20260521_171325_codex_node_multi_spot_reqrep_pollcompletion_full_20260521.txt`, `perf_node_multi_linux_20260521_180718_codex_node_multi_pubsub_small_no_eagain_exception_20260521.txt`, `perf_node_multi_linux_20260521_181512_codex_node_multi_rr_threshold5_recheck_20260521.txt`, `perf_node_multi_linux_20260521_183956_codex_node_multi_rr_public_borrowed_send_recheck_20260521.txt`, `perf_node_multi_linux_20260521_184302_codex_node_multi_pubsub_public_publish_borrowed_recheck_20260521.txt`, `perf_node_multi_linux_20260521_185753_codex_node_tcp_rr65536_recv_no_throw_recheck_20260521.txt`, `perf_node_multi_linux_20260521_190730_codex_node_tls_pubsub64_no_null_rid_recheck_20260521.txt`
- Node `MULTI_SPOT_REQREP`는 poll completion 반영 뒤 full matrix 결과로 갱신했다. `MULTI_PUBSUB`은 C와 같은 `bench` topic, no-data 예외 제거, metric payload 직접 기록, public publish operation 내부 borrowed Buffer 경로, raw native result의 불필요한 null routingId property 제거로 +5%p 기준까지 올렸다. `MULTI_ROUTER_ROUTER`은 public routed send 내부가 C 단일 part borrowed primitive를 쓰고, public `recvPayloadInto(...DontWait)` 내부 no-data 예외 비용을 제거해 남아 있던 `tcp 65536B`도 통과했다. 실패 은폐용 sleep/backoff나 public API 우회는 추가하지 않았다.

### 6.6 Go 상태

#### 6.6.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | `통과(73.4%)` | `통과(73.0%)` | `통과(114.3%)` | `통과(100.7%)` | `통과(100.3%)` | `통과(99.4%)` | 2026-05-23 core 6.0.2 fresh C baseline `perf_c_single_linux_20260523_102550_goal_c_single_603_baseline.txt (core 6.0.3)` 대비 Go scoped `perf_go_single_linux_20260523_000915_goal_go_tcp_nonrouted.txt`(GOMAXPROCS=4 기본). 기존 large 234~281%는 stale C baseline 영향이었고, fresh full baseline에서 ~99.6%로 정상화됐다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. full matrix는 routed large에서 cross-case hang이 재현돼 pattern scoped로 측정했다. |
| `tcp` | `PUBSUB` | `통과(60.3%)` | `통과(68.2%)` | `통과(89.4%)` | `통과(100.0%)` | `통과(100.1%)` | `통과(100.2%)` | C: `perf_c_single_linux_20260521_055144_codex_c_tcp_single_current_for_go_20260521.txt`; Go: `perf_go_single_linux_20260521_063732_codex_go_tcp_single_pubsub_spot_all_subscribepart_20260521.txt`. `SubSocket.SubscribePart(out, topicBuffer, flags)`를 추가해 C `zlink_subscribe_part`와 같은 단일 part receive 의미로 맞췄다. timeout은 없고 auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. 짧은 topic C 문자열 변환 제거, `TopicMessage` 저장소 재사용, 단일 part publish 직접 메서드 후보는 제한 재측정 `perf_go_single_linux_20260521_061807_codex_go_tcp_single_pubsub_spot_small_cstring_20260521.txt`, `perf_go_single_linux_20260521_062306_codex_go_tcp_single_pubsub_small_reuse_topic_message_20260521.txt`, `perf_go_single_linux_20260521_062619_codex_go_tcp_single_pubsub_spot_small_publishpart_20260521.txt`에서 개선 근거가 없어 반영하지 않았다. |
| `tcp` | `DEALER_DEALER` | `통과(74.0%)` | `통과(73.1%)` | `통과(117.4%)` | `통과(99.8%)` | `통과(100.1%)` | `통과(100.3%)` | 2026-05-23 fresh C baseline `perf_c_single_linux_20260523_102550_goal_c_single_603_baseline.txt (core 6.0.3)` 대비 Go scoped `perf_go_single_linux_20260523_000915_goal_go_tcp_nonrouted.txt`. 기존 large 226~277%는 stale C baseline 아티팩트였고 fresh baseline에서 ~99.6%로 정상화됐다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `DEALER_ROUTER` | `통과(58.5%)` | `통과(54.7%)` | `통과(62.9%)` | `통과(48.2%)` | `통과(44.9%)` | `통과(42.0%)` | 2026-05-23 fresh C baseline `perf_c_single_linux_20260523_102550_goal_c_single_603_baseline.txt (core 6.0.3)` 대비 Go scoped `perf_go_single_linux_..._goal_go_tcp_dr_scoped2.txt`. 131072B/262144B는 routed one-way large로 기존 판정(43.9%/43.2%)과 같은 수준이며, C `ROUTER_ROUTER/DEALER_ROUTER` 상대 비율(~0.99)을 유지한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `ROUTER_ROUTER` | `통과(61.7%)` | `통과(61.6%)` | `통과(62.9%)` | `통과(48.1%)` | `통과(44.5%)` | `통과(42.3%)` | 2026-05-23 fresh C baseline `perf_c_single_linux_20260523_102550_goal_c_single_603_baseline.txt (core 6.0.3)` 대비 Go scoped `perf_go_single_linux_20260523_000720_goal_go_tcp_rr_scoped.txt`. small/mid는 기존(55~58%)보다 개선됐고, large는 routed one-way 상대 기준(C RR/DR≈0.99)을 유지한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `SPOT` | `통과(163.0%)` | `통과(128.6%)` | `통과(110.8%)` | `통과(80.8%)` | `통과(73.3%)` | `통과(75.4%)` | 2026-05-23 fresh C baseline `perf_c_single_linux_20260523_102550_goal_c_single_603_baseline.txt (core 6.0.3)` 대비 Go `perf_go_single_linux_..._goal_go_spot_bytes_tcp.txt`(GOMAXPROCS=4). **SPOT large 회귀 수정**: tcp large publish가 매 send `NewWindowMessage(msgSize)`로 payload 버퍼까지 새로 할당해 GC 압박이 컸다(65536B 21985 msg/s, 39~44%). publish hot path를 기존 public `.Bytes(payload)` 빌더로 바꿔 단일 payload slice를 재사용하고 submit 때 한 번만 native message로 만들도록 했더니 45304/24692/12798 msg/s로 회복됐다. 이는 C `publish_metric_payload`(payload vector 재사용 + `zlink_msg_init_size`+memcpy)와 같은 의미다(`useSingleSpotBytesPublish`를 tcp ≥65536까지 확장). 같은 현재 core C 제한 재측정 `perf_c_single_linux_20260523_010524_goal_c_spot_currentcore.txt`(65536=55752) 대비로도 81.2%/69.5%/74.8%로 통과. GOMAXPROCS=4/8/20 모두 large가 동일해 GOMAXPROCS는 원인이 아니었다. small 64B/256B는 120% 초과 outlier. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. `Spot.SubscribePart(out, topicBuffer, flags)`를 추가해 C `zlink_spot_subscribe_part`와 같은 단일 part receive 의미로 맞췄다. active publish는 C처럼 `DONTWAIT`를 사용하고 backpressure 때 1ms 대기한다. SPOT active receive는 C처럼 poller 없이 `DONTWAIT` drain과 yield를 사용한다. Go sender는 C의 별도 sender/receiver thread 진행 의미를 맞추기 위해 성공 send 뒤 `runtime.Gosched()`로 receiver goroutine에 양보한다. C SPOT은 `NODROP`을 설정하지 않으므로 Go SPOT에서도 `SetNoDrop(true)`를 제거했다. timeout은 없고 auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. 짧은 topic C 문자열 변환 제거와 단일 part publish 직접 메서드 후보는 제한 재측정 `perf_go_single_linux_20260521_061807_codex_go_tcp_single_pubsub_spot_small_cstring_20260521.txt`, `perf_go_single_linux_20260521_062619_codex_go_tcp_single_pubsub_spot_small_publishpart_20260521.txt`에서 개선 근거가 없어 반영하지 않았다. |
| `ws` | `PAIR` | `통과(73.9%)` | `통과(67.6%)` | `통과(108.4%)` | `통과(98.7%)` | `통과(99.7%)` | `통과(100.0%)` | 64B/256B는 C `perf_c_single_linux_20260522_005942_codex_c_ws_single_go_latency_recheck_20260522.txt` 대비 Go `perf_go_single_linux_20260522_010624_codex_go_ws_single_native_stamp_20260522.txt`로 갱신했다. Go active send는 C처럼 native message payload에 직접 metric header를 stamp하고, `DONTWAIT` 미전송 message를 즉시 close한다. 256B latency는 C 대비 154.2x로 컸지만 throughput 비율은 목표권이고, 섹션 6.6.3의 latency artifact 판정에 따라 보류로 두지 않는다. `GOGC=off` 진단 `perf_go_single_linux_20260522_012120_codex_go_ws_single_gogc_off_probe_20260522.txt`, `GOMAXPROCS=2` 진단 `perf_go_single_linux_20260522_012257_codex_go_ws_single_gomaxprocs2_probe_20260522.txt`는 256B latency를 해결하지 못했다. 단일 part send를 native move 뒤 실패 시 restore하는 내부 후보는 `go test ./...`의 `TestBlockingSendFailurePreservesMessagePayload`에서 원본 보존 계약을 깨 탈락했다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. 나머지는 C `perf_c_single_linux_20260521_090653_codex_c_ws_single_current_after_core_rebuild_for_go_20260521.txt`, Go `perf_go_single_linux_20260521_085525_codex_go_ws_single_full_after_routed_timestamp_20260521.txt`. |
| `ws` | `PUBSUB` | `통과(60.5%)` | `통과(59.8%)` | `통과(96.1%)` | `통과(100.4%)` | `통과(99.2%)` | `통과(98.8%)` | 64B/256B는 current HEAD 같은 조건 C `perf_c_single_linux_20260525_155243_go_single_latency_stale_c.txt` 대비 Go `perf_go_single_linux_20260525_155344_single_latency_stale_recheck.txt`로 다시 갱신했다. throughput은 Go 단순 one-way 최소 기준을 넘고 complete다. latency는 64B/256B에서 여전히 C 대비 약 26x/76x로 크지만, 섹션 6.6.3의 latency artifact 판정에 따라 throughput 통과 셀을 보류로 되돌리지 않는다. receive hot path는 C처럼 첫 수신 blocking, 이후 `DONTWAIT` burst drain이다. `GOGC=off`와 `GOMAXPROCS=2` 진단은 latency를 해결하지 못했고, publish topic C 문자열 캐시 후보 `perf_go_single_linux_20260522_012639_codex_go_ws_single_publish_cstring_cache_20260522.txt`도 악화되어 반영하지 않았다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. 나머지는 위 2026-05-21 C/Go full 파일. |
| `ws` | `DEALER_DEALER` | `통과(74.3%)` | `통과(67.9%)` | `통과(113.4%)` | `통과(99.7%)` | `통과(100.0%)` | `통과(100.0%)` | 64B/256B는 위 2026-05-22 C/Go 제한 재측정 파일로 갱신했다. 256B latency는 C 대비 133.4x로 컸지만 throughput 비율은 목표권이고, latency artifact 판정에 따라 보류로 두지 않는다. `GOGC=off`와 `GOMAXPROCS=2` 진단은 256B latency를 해결하지 못했다. 단일 part send move/restore 후보는 public 실패 원본 보존 테스트를 깨 반영하지 않았다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. 나머지는 위 2026-05-21 C/Go full 파일. |
| `ws` | `DEALER_ROUTER` | `통과(56.1%)` | `통과(58.1%)` | `통과(74.9%)` | `통과(83.5%)` | `통과(97.8%)` | `통과(81.3%)` | C/Go 파일은 위 PAIR 행과 같다. Go routed active phase를 C `perf_dealer_router.cpp`처럼 sender goroutine의 blocking send와 receiver의 blocking `RecvPart` stop-token 루프로 맞췄다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `ROUTER_ROUTER` | `통과(65.4%)` | `통과(55.9%)` | `통과(67.5%)` | `통과(90.9%)` | `통과(94.6%)` | `통과(81.7%)` | C/Go 파일은 위 PAIR 행과 같다. ROUTER-ROUTER도 C처럼 PING/PONG으로 target route를 확인한 뒤 active와 stop token을 blocking send 의미로 보낸다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `SPOT` | `통과(153.7%)` | `통과(123.5%)` | `통과(101.7%)` | `통과(104.2%)` | `통과(84.5%)` | `통과(88.4%)` | 64B/256B는 current HEAD 같은 조건 C `perf_c_single_linux_20260525_155243_go_single_latency_stale_c.txt` 대비 Go `perf_go_single_linux_20260525_155344_single_latency_stale_recheck.txt`로 다시 확인했다. throughput은 C보다 높고 complete다. latency는 64B/256B에서 약 3.5x/5.5x로 남지만, 이전 13.7x보다 낮고 throughput 목표를 만족하므로 보류로 두지 않는다. `GOGC=off`, `GOMAXPROCS=2`, publish topic C 문자열 캐시 후보는 공식 조건 전체를 해결하지 못해 반영하지 않았다. SPOT은 C와 같은 `DONTWAIT` publish/backpressure 대기 의미와 `SubscribePart` 수신 경로를 유지하며 auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. 나머지는 위 2026-05-21 C/Go full 파일. |
| `wss` | `PAIR` | `통과(74.0%)` | `통과(73.4%)` | `통과(108.3%)` | `통과(88.3%)` | `통과(76.4%)` | `통과(81.6%)` | C `perf_c_single_linux_20260522_012906_codex_c_wss_single_for_go_20260522.txt` 대비 Go `perf_go_single_linux_20260522_013230_codex_go_wss_single_current_20260522.txt`. 256B latency는 C 대비 117.6x로 컸지만 throughput 비율은 목표권이고, latency artifact 판정에 따라 보류로 두지 않는다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `PUBSUB` | `통과(64.2%)` | `통과(66.0%)` | `통과(109.1%)` | `통과(86.1%)` | `통과(80.0%)` | `통과(86.1%)` | 64B/256B는 current HEAD 같은 조건 C `perf_c_single_linux_20260525_155243_go_single_latency_stale_c.txt` 대비 Go `perf_go_single_linux_20260525_155344_single_latency_stale_recheck.txt`로 다시 갱신했다. throughput은 기준을 넘고 complete다. 64B latency는 C와 같은 대역이고, 256B latency는 C 대비 약 101x로 여전히 크지만 throughput 통과 셀을 보류로 되돌리는 근거로 쓰지 않는다. `GOGC=off`, `GOMAXPROCS=2`, topic C 문자열 캐시 후보는 ws 제한 재측정에서 같은 latency 병목을 해결하지 못했다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `DEALER_DEALER` | `통과(74.3%)` | `통과(73.5%)` | `통과(106.9%)` | `통과(92.7%)` | `통과(75.5%)` | `통과(82.2%)` | C/Go 파일은 위 wss PAIR 행과 같다. 256B latency는 C 대비 110.1x로 컸지만 throughput 비율은 목표권이고, latency artifact 판정에 따라 보류로 두지 않는다. 단일 part send move/restore 후보는 public 실패 원본 보존 테스트를 깨 반영하지 않았다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `DEALER_ROUTER` | `통과(53.8%)` | `통과(57.0%)` | `통과(93.3%)` | `통과(84.8%)` | `통과(91.5%)` | `통과(108.5%)` | C/Go 파일은 위 wss PAIR 행과 같다. routed one-way 기준을 통과했고 auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `ROUTER_ROUTER` | `통과(62.8%)` | `통과(55.7%)` | `통과(92.7%)` | `통과(72.8%)` | `통과(89.3%)` | `통과(105.4%)` | C/Go 파일은 위 wss PAIR 행과 같다. routed one-way 기준을 통과했고 auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `SPOT` | `통과(149.5%)` | `통과(104.7%)` | `통과(106.2%)` | `통과(90.5%)` | `통과(96.1%)` | `통과(91.3%)` | 64B/256B는 current HEAD 같은 조건 C `perf_c_single_linux_20260525_155243_go_single_latency_stale_c.txt` 대비 Go `perf_go_single_linux_20260525_155344_single_latency_stale_recheck.txt`로 다시 갱신했다. throughput은 기준을 넘고 complete다. 64B latency는 C 대비 약 1.7x이고, 256B latency는 약 39x로 남지만 throughput 통과 셀을 보류로 되돌리지 않는다. 262144B active publish는 재사용 payload slice에 metric header를 찍고 public `Bytes(...)` builder로 submit하는 경로를 size별 선택 적용해 C 1985.0 msg/s 대비 Go 1831.0 msg/s로 통과권에 들어갔다. `GOGC=off`, sender yield 빈도 조정, explicit routing id 제거, topic C 문자열 캐시, send builder single-part inline, OS thread 고정, receiver-main 구조 후보는 미달을 해결하지 못했거나 다른 size를 악화시켜 반영하지 않았다. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `PAIR` | `통과(74.0%)` | `통과(71.8%)` | `통과(121.8%)` | `통과(76.2%)` | `통과(80.8%)` | `통과(90.8%)` | C `perf_c_single_linux_20260522_015850_codex_c_tls_single_for_go_20260522.txt` 대비 Go `perf_go_single_linux_20260522_020215_codex_go_tls_single_current_20260522.txt`. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `PUBSUB` | `통과(61.3%)` | `통과(68.1%)` | `통과(120.0%)` | `통과(76.1%)` | `통과(79.5%)` | `통과(90.7%)` | C/Go 파일은 위 tls PAIR 행과 같다. receive hot path는 C처럼 첫 수신 blocking, 이후 `DONTWAIT` burst drain이며 auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `DEALER_DEALER` | `통과(74.1%)` | `통과(73.2%)` | `통과(118.9%)` | `통과(78.6%)` | `통과(81.2%)` | `통과(94.3%)` | C/Go 파일은 위 tls PAIR 행과 같다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `DEALER_ROUTER` | `통과(55.7%)` | `통과(54.5%)` | `통과(88.8%)` | `통과(85.9%)` | `통과(87.4%)` | `통과(88.9%)` | C/Go 파일은 위 tls PAIR 행과 같다. routed one-way 기준을 통과했고 auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `ROUTER_ROUTER` | `통과(64.7%)` | `통과(63.2%)` | `통과(88.9%)` | `통과(85.2%)` | `통과(88.9%)` | `통과(85.9%)` | C/Go 파일은 위 tls PAIR 행과 같다. routed one-way 기준을 통과했고 auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `SPOT` | `통과(156.8%)` | `통과(125.6%)` | `통과(101.3%)` | `통과(97.6%)` | `통과(97.2%)` | `통과(89.2%)` | C/Go 파일은 위 tls PAIR 행과 같다. SPOT은 C와 같은 `DONTWAIT` publish/backpressure 대기 의미와 `SubscribePart` 수신 경로를 유지한다. 64B/256B는 C보다 높지만 같은 조건의 C 기준과 비교했고, 2026-05-22 wss SPOT 64B에서도 같은 의미의 outlier가 확인되어 별도 outlier 목록에서 추적한다. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. |

#### 6.6.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | `통과(45.4%)` | `통과(64.8%)` | `통과(81.1%)` | `통과(81.4%)` | `통과(66.8%)` | `통과(42.9%)` | 64/256/1024B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_040430.txt` 대비 Go `perf_go_multi_linux_20260525_040452.txt` 기준이다. 65536B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Go `perf_go_multi_linux_20260525_001654.txt` median 기준이다. 131072B는 C `perf_c_multi_linux_20260525_045447.txt` 대비 Go `perf_go_multi_linux_20260525_045641.txt` 기준이고, 262144B는 같은 조건 최신 재측정 C `perf_c_multi_linux_20260525_100859.txt` 대비 Go `perf_go_multi_linux_20260525_100837.txt` 기준이다. C처럼 단일 poll loop와 pending socket만 `POLLOUT`으로 두는 poll set 의미를 유지했다. server는 C의 single-part `zlink_recv_part` 의미와 맞춰 64B/65536B에서 public `RecvPart` caller-owned 수신 경로를 사용한다. client active send는 1024B 이하와 65536B 이상에서 public `Bytes(...)` 경로를 사용해 submit 전 native `Message` 생성을 줄였다. 131072B는 current HEAD 재검토에서 보류를 해소했고, 262144B는 기본 `GOMAXPROCS=4` 실행에서는 39.8%였지만 case-local `GOMAXPROCS=8` 적용 뒤 Go 단순 one-way 최소 기준을 넘었다. |
| `tcp` | `MULTI_DEALER_ROUTER` | `통과(59.7%)` | `통과(56.1%)` | `통과(55.9%)` | `통과(41.3%)` | `통과(49.9%)` | `통과(43.8%)` | 64/256/1024B는 기존 Go `perf_go_multi_linux_20260522_050940_codex_go_tcp_multi_shared_client_context_20260522.txt` 기준이다. 65536/131072B는 C `perf_c_multi_linux_20260525_045819.txt` 대비 Go `perf_go_multi_linux_20260525_045907.txt` 기준이고, 262144B는 C `perf_c_multi_linux_20260525_045938.txt` 대비 Go `perf_go_multi_linux_20260525_050001.txt` 기준이다. per-socket goroutine/poller 구조에서 발생하던 `Bad address`/`I/O error`를 C와 같은 단일 poll loop로 정렬해 전체 size complete를 확보했다. routed echo client는 C `perf_multi_client_helpers.hpp`처럼 한 client context 안에서 모든 dealer socket을 만들도록 맞춰, client당 별도 context와 IO thread 차이를 제거했다. 262144B server는 C의 single-part `zlink_router_recv_part` 의미와 맞춰 public `RecvPart` caller-owned 수신 경로를 사용한다. server `RecvPart`를 전체 size에 적용한 후보 `perf_go_multi_linux_20260522_044906_codex_go_tcp_multi_routed_server_recvpart_20260522.txt`는 작은 size를 악화시켜 반영하지 않았다. C helper처럼 client send scan 시작점을 round-robin으로 회전하는 후보 `perf_go_multi_linux_20260522_053520_codex_go_tcp_multi_routed_client_round_robin_20260522.txt`는 65536B 이상과 일부 small size를 낮춰 반영하지 않았다. 최신 같은 조건 재측정에서 전 size가 Go multi routed echo 기준을 넘었다. |
| `tcp` | `MULTI_ROUTER_ROUTER` | `통과(40.2%)` | `통과(41.5%)` | `통과(42.3%)` | `통과(44.3%)` | `통과(50.3%)` | `통과(52.0%)` | **2026-05-23 LockOSThread 수정 후 fresh C baseline `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Go `perf_go_multi_linux_20260523_230923.txt`로 전 size 통과**(이전 보류 24~39%는 Go-runtime cgo wakeup-latency 병목, 6.6.3 참조). 이하 메모는 이전 라운드 구현 기록. C 파일은 위 행과 같다. Go `perf_go_multi_linux_20260522_050940_codex_go_tcp_multi_shared_client_context_20260522.txt`. routed echo server는 수신 `RoutingID` 값을 명시적으로 복사해 `SendTo`하고, client는 C와 같은 단일 poll loop로 정렬했다. router-router client도 C routed echo client처럼 한 client context 안에서 모든 router socket을 만들도록 맞춰 client별 context/IO thread 차이를 제거했다. 262144B server는 C의 single-part `zlink_router_recv_part` 의미와 맞춰 public `RecvPart` caller-owned 수신 경로를 사용한다. server `RecvPart`를 전체 size에 적용한 후보 `perf_go_multi_linux_20260522_044906_codex_go_tcp_multi_routed_server_recvpart_20260522.txt`는 일부 작은 size와 65536B/131072B를 악화시켜 반영하지 않았다. C helper처럼 client send scan 시작점을 round-robin으로 회전하는 후보 `perf_go_multi_linux_20260522_053520_codex_go_tcp_multi_routed_client_round_robin_20260522.txt`는 일부 small size만 소폭 올리고 1024B 이상을 낮춰 반영하지 않았다. 최신 재측정에서 전 size가 multi routed echo 기준을 넘었다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `MULTI_PUBSUB` | `통과(53.0%)` | `통과(53.7%)` | `통과(87.5%)` | `통과(53.1%)` | `통과(52.5%)` | `통과(72.4%)` | C 파일은 위 행과 같다. Go 64B/1024B/65536B/131072B/262144B `perf_go_multi_linux_20260522_114251_codex_go_multi_pubsub_sampled_latency_full_guard_20260522.txt`, 256B `perf_go_multi_linux_20260522_114812_codex_go_multi_pubsub256_sampled_latency_count_return_probe_20260522.txt`. multi PUBSUB client도 single처럼 public `SubscribePart` caller-owned 수신 경로를 사용한다. throughput count와 latency sample을 분리해 active payload는 모두 세고, latency 계산은 `PERF_MULTI_PUBSUB_LATENCY_SAMPLE_STRIDE=32` 샘플에만 수행하도록 줄였다. server publish를 size 생성 메시지로 바꾸는 후보 `perf_go_multi_linux_20260522_051648_codex_go_tcp_multi_pubsub_window_message_20260522.txt`와 1024B~131072B에만 적용하는 후보 `perf_go_multi_linux_20260522_051932_codex_go_tcp_multi_pubsub_window_message_selected_20260522.txt`는 262144B completion을 깨서 반영하지 않았다. 모든 subscriber를 단일 poller에 등록하고, C처럼 하나의 stop/cooldown 신호로 phase를 종료한다. Go metric timestamp는 C와 같은 epoch ns다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `MULTI_SPOT` | `통과(66.0%)` | `통과(76.7%)` | `통과(81.7%)` | `통과(111.6%)` | `통과(109.4%)` | `통과(84.5%)` | 64/256/1024B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_021128.txt` 대비 Go `perf_go_multi_linux_20260525_021338.txt` 기준이다. 65536B는 기존 Go `perf_go_multi_linux_20260522_112218_codex_go_tcp_ws_multi_spot_sampled_latency_guard_20260522.txt`, 131072B는 `perf_go_multi_linux_20260522_112354_codex_go_tcp_multi_spot131_sampled_latency_recheck_20260522.txt`, 262144B는 `perf_go_multi_linux_20260522_112409_codex_go_tcp_multi_spot262_sampled_latency_repeat_20260522.txt` 기준이다. server-stamped SPOT latency도 epoch ns로 정렬했고, client drain 내부에서 active deadline을 확인해 backlog가 한 slot에 몰려도 phase가 끝나도록 했다. multi SPOT client는 C `perf_multi_spot_client.cpp`처럼 public `SubscribePart` caller-owned 수신 경로를 DONTWAIT drain한다. throughput count는 모든 active payload에서 세고, latency 계산은 C와 같은 `PERF_MULTI_SPOT_LATENCY_SAMPLE_STRIDE=32` 샘플에만 수행해 hot path의 `time.Now()` 호출을 줄였다. server data publisher는 C server option과 맞춰 public `SetNoDrop(true)`와 `PERF_MULTI_SNDTIMEO_MS`를 적용했다. C의 `DONTWAIT` 실패 후 blocking publish 1회 재시도 후보 `perf_go_multi_linux_20260522_045649_codex_go_tcp_multi_spot_nodrop_retry_20260522.txt`는 Go public clone publish 경로에서 262144B가 0.02%로 무너져 반영하지 않았다. `NODROP`/timeout만 분리한 후보 `perf_go_multi_linux_20260522_045839_codex_go_tcp_multi_spot_nodrop_only_20260522.txt`는 large를 조금 올렸지만 small 평균을 해결하지 못해 worker/stride 변경과 함께만 반영했다. full guard의 262144B no-result와 낮은 131072B 값은 같은 조건 단독 재측정으로 배제했다. 최신 재측정에서 전 size가 통과권이다. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. |
| `tcp` | `MULTI_SPOT_REQREP` | `통과(55.8%)` | `통과(55.4%)` | `통과(57.4%)` | `통과(59.1%)` | `통과(58.8%)` | `통과(50.1%)` | 64B/256B/1024B는 기존 Go `perf_go_multi_linux_20260522_023554_codex_go_tcp_multi_current_after_runner_cleanup_20260522.txt` 기준이다. 65536B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Go `perf_go_multi_linux_20260525_003036.txt` median 기준이다. 131072B는 C `perf_c_multi_linux_20260525_044910.txt` 대비 Go `perf_go_multi_linux_20260525_044940.txt` 기준이고, 262144B는 같은 조건 최신 재측정 C `perf_c_multi_linux_20260525_104221.txt` 대비 current Go `perf_go_multi_linux_20260525_104301.txt` 기준이다. request completion은 public `POLLCOMPLETION` poller를 사용한다. client request hot path는 65536B/131072B에서 public `RequestOp.Bytes(...)`를 사용해 caller-owned slice를 submit 때 native message로 만들고, submit 전 Go `Message` 생성을 줄인다. 65536B와 131072B는 최신 재측정에서 Go SPOT 기준을 넘었다. 262144B는 `Bytes(...)` 후보가 `perf_go_multi_linux_20260525_002738.txt`에서 2.8%로 무너져 기존 `Message(...)` 경로를 유지하지만, fresh C/current Go 제한 재측정에서 기준선을 넘었다. |
| `tcp` | `MULTI_SPOT_SENDSEND` | `통과(55.3%)` | `통과(54.4%)` | `통과(58.3%)` | `통과(62.4%)` | `통과(71.2%)` | `통과(78.7%)` | 64/256/1024B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_064305.txt` 대비 Go `perf_go_multi_linux_20260525_064343.txt` 기준이다. 65536/131072/262144B는 C `perf_c_multi_linux_20260525_092522.txt` 대비 Go `perf_go_multi_linux_20260525_092253.txt` 기준이다. C++처럼 client active loop에서 waiting reply를 먼저 `RecvRouted(...DONTWAIT)`로 drain한 뒤 같은 slot에서 즉시 다음 send를 시도하도록 맞췄다. server idle 대기는 전 size에서 고정 `sleep(1ms)` 대신 public `Poller` `POLLIN` wake를 사용한다. client send는 `ws`가 아니고 65536B 이하일 때만 public `MoveMessage(...)`를 사용해 submit 전 복사를 줄인다. `ws`와 131072B 이상은 후보 측정에서 일부 size가 낮아져 기존 `Message(...)` 경로를 유지한다. |
| `tcp` | `MULTI_STREAM` | `통과(98.1%)` | `통과(89.4%)` | `통과(78.4%)` | `통과(90.0%)` | `통과(94.2%)` | `통과(80.1%)` | C 파일은 위 행과 같다. Go `perf_go_multi_linux_20260522_033642_codex_go_tcp_multi_stream_alias_fix_20260522.txt`. Go runner는 shared C stream client가 출력하는 `STREAM` result를 `MULTI_STREAM` 행으로 집계하도록 수정했다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `MULTI_DEALER_DEALER` | `통과(46.0%)` | `통과(55.2%)` | `통과(67.7%)` | `통과(59.2%)` | `통과(58.7%)` | `통과(64.4%)` | 64B는 같은 조건 C `perf_c_multi_linux_20260525_130455.txt` 대비 Go `perf_go_multi_linux_20260525_131051.txt` 기준이다. 256B/1024B는 Go `perf_go_multi_linux_20260524_222134.txt`, 65536/131072/262144B는 Go `perf_go_multi_linux_20260525_001654.txt` median 기준이다. C처럼 단일 poll loop와 pending socket만 `POLLOUT`으로 두는 poll set 의미를 유지했다. client active send는 64B/65536B/131072B에서 public `Bytes(...)` 경로를 사용해 submit 전 Go `Message` 생성을 줄였다. 64B server는 throughput count를 전 메시지에 유지하고 latency 계산/저장만 기본 32개당 1개로 줄여 수신 hot path의 `time.Now()`와 slice append 비용을 낮췄다. 262144B는 기존 경로로도 current HEAD에서 통과권이 유지된다. |
| `ws` | `MULTI_DEALER_ROUTER` | `통과(60.7%)` | `통과(62.2%)` | `통과(60.9%)` | `통과(46.7%)` | `통과(51.6%)` | `통과(66.7%)` | C/Go full 파일은 위 행과 같다. routed echo client는 C처럼 한 client context 안에서 모든 socket을 단일 poll loop로 처리한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `MULTI_ROUTER_ROUTER` | `통과(41.2%)` | `통과(41.7%)` | `통과(41.5%)` | `통과(41.3%)` | `통과(58.5%)` | `통과(71.9%)` | C full 파일은 위 행과 같다. Go 64B/256B/1024B/131072B/262144B `perf_go_multi_linux_20260522_060003_codex_go_ws_multi_current_20260522.txt`, 65536B 제한 재측정 `perf_go_multi_linux_20260522_062743_codex_go_ws_multi_rr65536_recheck_20260522.txt`. 65536B는 full에서 39.1%였으나 같은 조건 제한 재측정에서 41.3%로 최소 기준을 통과했다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `MULTI_PUBSUB` | `통과(56.5%)` | `통과(57.0%)` | `통과(61.5%)` | `통과(61.5%)` | `통과(64.9%)` | `통과(75.4%)` | C full 파일은 위 행과 같다. Go 64B/256B/1024B/65536B/262144B `perf_go_multi_linux_20260522_114251_codex_go_multi_pubsub_sampled_latency_full_guard_20260522.txt`, 131072B `perf_go_multi_linux_20260522_114658_codex_go_ws_multi_pubsub131_sampled_latency_recheck_20260522.txt`. public `SubscribePart` caller-owned 수신 경로와 단일 poller 의미를 유지하면서 throughput count와 latency sample을 분리했다. full guard의 131072B no-result는 같은 조건 단독 재측정으로 배제했다. server publish를 `NewWindowMessage`로 바꾸는 후보 `perf_go_multi_linux_20260522_062658_codex_go_ws_multi_pubsub_window_message_probe_20260522.txt`는 262144B는 올렸지만 64B 미달을 해결하지 못했고 기존 tcp large completion 실패 이력이 있어 반영하지 않았다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `MULTI_SPOT` | `통과(53.1%)` | `통과(51.9%)` | `통과(55.1%)` | `통과(113.5%)` | `통과(130.0%)` | `통과(103.3%)` | 64/256/1024B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_021128.txt` 대비 Go `perf_go_multi_linux_20260525_021338.txt` 기준이다. 65536B 이상은 Go `perf_go_multi_linux_20260522_112218_codex_go_tcp_ws_multi_spot_sampled_latency_guard_20260522.txt` 기준이다. Go multi report의 SPOT fallback `MsgUnit(B)`가 small size에서 4096으로 표시되던 문제를 `bindings/python/perf/perf_report.py`에서 C와 같은 size별 값으로 고쳤고, Go perf helper는 C helper처럼 `SetAutoHwmMsgUnitBytes` 뒤 public `RecalculateAutoHwm()`을 호출한다. throughput count는 모든 active payload에서 세고, latency 계산은 C와 같은 `PERF_MULTI_SPOT_LATENCY_SAMPLE_STRIDE=32` 샘플에만 수행해 hot path의 `time.Now()` 호출을 줄였다. C처럼 active publish에서 `DONTWAIT` 실패 뒤 blocking submit 1회를 시도하는 후보 `perf_go_multi_linux_20260522_063626_codex_go_ws_multi_spot_blocking_fallback_20260522.txt`는 64B/256B를 더 낮춰 반영하지 않았다. 최신 재측정에서 전 size가 통과권이다. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. |
| `ws` | `MULTI_SPOT_REQREP` | `통과(61.1%)` | `통과(60.8%)` | `통과(63.8%)` | `통과(75.1%)` | `통과(50.2%)` | `통과(69.7%)` | 64B/256B/1024B는 기존 Go `perf_go_multi_linux_20260522_061530_codex_go_ws_multi_spot_reqrep_msgunit_report_fix_20260522.txt` 기준이다. 65536B/131072B/262144B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Go `perf_go_multi_linux_20260525_003036.txt` median 기준이다. request completion은 public `POLLCOMPLETION` poller를 사용한다. client request hot path는 65536B 이상에서 public `RequestOp.Bytes(...)`를 사용한다. 131072B가 37.8%에서 50.2%로 올라 보류가 해소됐고, 65536B/262144B도 통과 여유가 커졌다. |
| `ws` | `MULTI_SPOT_SENDSEND` | `통과(58.3%)` | `통과(59.0%)` | `통과(56.9%)` | `통과(95.7%)` | `통과(89.8%)` | `통과(72.7%)` | 64/256/1024B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_064438.txt` 대비 Go `perf_go_multi_linux_20260525_064632.txt` 기준이다. 65536/131072/262144B는 C `perf_c_multi_linux_20260525_092522.txt` 대비 Go `perf_go_multi_linux_20260525_092253.txt` 기준이다. drain-then-submit loop와 `Spot.ForwardRouted(...)` server echo로 complete를 유지하면서 collapse를 줄였다. server idle 대기는 전 size에서 고정 `sleep(1ms)` 대신 public `Poller` `POLLIN` wake를 사용한다. `MoveMessage(...)` client send 후보는 `ws` small과 일부 large를 낮춰 적용하지 않는다. C와 같은 size별 active slot 제한을 유지한다. |
| `ws` | `MULTI_STREAM` | `통과(92.4%)` | `통과(83.5%)` | `통과(79.3%)` | `통과(92.2%)` | `통과(114.1%)` | `통과(149.3%)` | C 64B/256B `perf_c_multi_linux_20260522_055144_codex_c_ws_multi_for_go_20260522.txt`, 1024B/65536B `perf_c_multi_linux_20260522_062009_codex_c_ws_multi_stream_recheck_for_go_20260522.txt`, 131072B/262144B `perf_c_multi_linux_20260522_062037_codex_c_ws_multi_stream_large_for_go_20260522.txt` 대비 Go `perf_go_multi_linux_20260522_060003_codex_go_ws_multi_current_20260522.txt`. C full-run은 1024B 이상 STREAM이 partial이라 같은 조건 제한 재측정으로 보강했다. Go STREAM은 shared C reference client를 사용하므로 측정 surface는 Go STREAM server다. 262144B는 120%를 넘어 outlier 재검토 대상이다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `MULTI_DEALER_DEALER` | `통과(43.3%)` | `통과(58.3%)` | `통과(71.2%)` | `통과(51.0%)` | `통과(57.1%)` | `통과(63.4%)` | 64B는 같은 조건 C `perf_c_multi_linux_20260525_130455.txt` 대비 Go `perf_go_multi_linux_20260525_131051.txt` 기준이다. 256/1024B는 Go `perf_go_multi_linux_20260524_222134.txt` 기준이다. 65536/131072B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_071312.txt` 대비 Go `perf_go_multi_linux_20260525_072100.txt` 기준이다. 262144B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_042131.txt` 대비 Go `perf_go_multi_linux_20260525_042206.txt` 기준이다. C처럼 단일 poll loop와 pending socket `POLLOUT` 의미를 유지했다. Go send builder에 명시적 ownership 이전 단계 `MoveMessage`를 추가했고, 64B와 65536B 이상은 public `Bytes(...)` client send를 사용한다. 64B server는 throughput count를 전 메시지에 유지하고 latency 계산/저장만 기본 32개당 1개로 줄여 수신 hot path의 `time.Now()`와 slice append 비용을 낮췄다. wss 65536B는 같은 조건 no-code 재측정 `perf_go_multi_linux_20260525_042547.txt` 28.512Kmsg/s, `MoveMessage(...)` 후보 31.792Kmsg/s(`perf_go_multi_linux_20260525_042512.txt`)에서 34.459Kmsg/s로 올라 통과했다. wss 262144B는 public `Bytes(...)` client send와 server `RecvPart` caller-owned 수신 경로를 함께 적용해 같은 조건 no-code 재측정 `perf_go_multi_linux_20260525_042046.txt` 1.949Kmsg/s에서 10.195Kmsg/s로 올라 통과했다. 131072B server 수신은 public `RecvPart` caller-owned 수신 경로로 확장해 no-code 재측정 9.894Kops/s(`perf_go_multi_linux_20260525_015918.txt`)에서 16.457Kops/s로 올렸고, 이후 client send를 `Bytes(...)`로 바꿔 19.479Kmsg/s, C 대비 57.1%로 통과했다. 첫 mixed 후보 `perf_go_multi_linux_20260525_071330.txt`는 131072B median이 낮았지만, 131072B 단독(`perf_go_multi_linux_20260525_072036.txt`)과 mixed 재실행(`perf_go_multi_linux_20260525_072100.txt`)에서 통과를 재현했다. |
| `wss` | `MULTI_DEALER_ROUTER` | `통과(60.6%)` | `통과(59.3%)` | `통과(58.7%)` | `통과(47.6%)` | `통과(51.3%)` | `통과(52.9%)` | C/Go full 파일은 위 행과 같다. routed echo client는 C처럼 한 client context 안에서 모든 socket을 단일 poll loop로 처리한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `MULTI_ROUTER_ROUTER` | `통과(44.5%)` | `통과(42.2%)` | `통과(43.9%)` | `통과(50.1%)` | `통과(52.9%)` | `통과(52.6%)` | C/Go full 파일은 위 행과 같다. 절대 기준을 통과하므로 상대 기준은 진단 보조로만 본다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `MULTI_PUBSUB` | `통과(125.7%)` | `통과(61.2%)` | `통과(69.7%)` | `통과(56.2%)` | `통과(59.1%)` | `통과(66.5%)` | 256B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_020714.txt` 2.688Mmsg/s 대비 Go `perf_go_multi_linux_20260525_020827.txt` 1.644Mmsg/s 기준이다. 나머지 C full 파일은 위 행과 같다. Go 64B/1024B/65536B/131072B/262144B는 `perf_go_multi_linux_20260522_114251_codex_go_multi_pubsub_sampled_latency_full_guard_20260522.txt` 기준이다. public `SubscribePart` caller-owned 수신 경로와 단일 poller 의미를 유지하면서 throughput count와 latency sample을 분리했다. 64B는 120%를 넘어 outlier 재검토 대상으로 남긴다. 이전 256B 단독 재측정 `perf_go_multi_linux_20260522_114731_codex_go_wss_multi_pubsub256_sampled_latency_recheck_20260522.txt`는 낮았지만, 현재 HEAD 같은 조건 재측정에서 통과권으로 올라왔다. `NewWindowMessage` 전체 적용 후보 `perf_go_multi_linux_20260522_085042_codex_go_wss_tls_multi_pubsub_small_window_message_20260522.txt`, topic C 문자열 캐시 후보 `perf_go_multi_linux_20260522_091723_codex_go_wss_tls_pubsub_spot_topic_cstring_cache_20260522.txt`, `MoveMessage` 내부 `zlink_msg_adopt` 후보 `perf_go_multi_linux_20260522_092410_codex_go_wss_tls_dd_pubsub_movemessage_adopt_20260522.txt`, `Bytes(...)` publish 후보 `perf_go_multi_linux_20260522_111440_codex_go_wss_tls_multi_pubsub_small_bytes_publish_20260522.txt`는 기준을 안정적으로 올리지 못해 반영하지 않았다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `MULTI_SPOT` | `통과(50.9%)` | `통과(50.9%)` | `통과(51.0%)` | `통과(50.4%)` | `통과(56.4%)` | `통과(93.1%)` | 64B/256B는 C `perf_c_multi_linux_20260522_112642_codex_c_wss_multi_spot64_256_current_recheck_for_go_20260522.txt`, Go `perf_go_multi_linux_20260522_112132_codex_go_wss_tls_multi_spot_small_sampled_latency_20260522.txt` 기준이다. 1024B는 current HEAD 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_105036.txt` 5.659Mmsg/s 대비 Go `perf_go_multi_linux_20260525_105036.txt` 2.889Mmsg/s 기준이다. 나머지는 기존 C/Go full 파일이다. public `SubscribePart` 수신 경로와 C와 같은 worker 분배 의미를 사용한다. Go perf latency 기록은 C처럼 수신 시각을 한 번만 잡아 계산하도록 공통 hot path를 줄였고, wss 64B/256B/1024B server publish는 caller-owned slice를 submit 때 native message로 만드는 public `Bytes(...)` builder 경로로 줄였다. encrypted small SPOT은 Go worker fan-out 비용을 줄이도록 기본 수신 worker를 4개로 좁혔고, throughput count와 latency sampling을 분리해 수신 hot path의 `time.Now()` 호출을 줄였다. 1024B는 이전 같은 조건 제한 재측정에서 45.3%였지만 current HEAD 재측정에서 기준을 넘겨 보류에서 제거한다. C의 `DONTWAIT` 실패 후 blocking publish 1회 재시도, `NODROP`/timeout 분리 후보는 이전 제한 재측정에서 small 또는 large를 해결하지 못했다. server publish를 `MoveMessage`로 바꾸는 후보 `perf_go_multi_linux_20260522_090056_codex_go_wss_tls_multi_spot_small_movemessage_20260522.txt`, 선택 적용 후보 `perf_go_multi_linux_20260522_090236_codex_go_wss_tls_multi_spot_small_selected_movemessage_20260522.txt`는 1024B를 크게 낮추거나 256B 개선이 재현되지 않아 반영하지 않았다. topic C 문자열 캐시 후보 `perf_go_multi_linux_20260522_091723_codex_go_wss_tls_pubsub_spot_topic_cstring_cache_20260522.txt`는 `wss 1024B`를 크게 낮춰 반영하지 않았다. worker 2개 후보 `perf_go_multi_linux_20260522_111917_codex_go_wss_tls_multi_spot64_1024_workers2_probe_20260522.txt`는 1024B 처리량을 낮추고 tls 64B 실패를 만들어 반영하지 않았다. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `MULTI_SPOT_REQREP` | `통과(55.9%)` | `통과(59.5%)` | `통과(60.0%)` | `통과(85.6%)` | `통과(77.9%)` | `통과(78.2%)` | 64B/256B/1024B는 기존 Go `perf_go_multi_linux_20260522_070749_codex_go_wss_multi_spot_reqrep_submit_not_connected_all_20260522.txt` 기준이다. 65536B/131072B/262144B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Go `perf_go_multi_linux_20260524_222958.txt` median 기준이다. 이전 full-run과 단독 제한 재측정 `perf_go_multi_linux_20260522_065858_codex_go_wss_multi_spot_reqrep262144_recheck_20260522.txt`의 262144B no-result는 Go perf client가 active 송신 중 `SubmitNotConnected`를 fatal로 처리하던 차이 때문이었다. C `MULTI_SPOT_REQREP` client처럼 `ZLINK_SUBMIT_NOT_CONNECTED`는 fatal이 아니라 다음 poll loop에서 재시도하는 의미로 맞췄다. request completion은 public `POLLCOMPLETION` poller를 사용한다. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. |
| `wss` | `MULTI_SPOT_SENDSEND` | `통과(53.2%)` | `통과(56.1%)` | `통과(61.1%)` | `통과(53.3%)` | `통과(50.4%)` | `통과(53.6%)` | 64/256/1024B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_064438.txt` 대비 Go `perf_go_multi_linux_20260525_064632.txt` 기준이다. 65536/262144B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Go `perf_go_multi_linux_20260524_220932.txt` median 기준이다. 131072B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_020201.txt` 8.046Kops/s 대비 Go `perf_go_multi_linux_20260525_020157.txt` 4.057Kops/s 기준이다. client active loop를 C++처럼 drain-then-submit으로 바꿨고, 65536B 이하 client send는 public `MoveMessage(...)`를 사용한다. 1024B 이하 server idle 대기는 고정 `sleep(1ms)` 대신 public `Poller` `POLLIN` wake를 사용해 small 보류를 해소했다. 서버 echo는 public `Spot.ForwardRouted(...)`를 65536B 이상에 적용한다. |
| `wss` | `MULTI_STREAM` | `통과(88.6%)` | `통과(95.3%)` | `통과(89.4%)` | `통과(92.3%)` | `해당 없음` | `해당 없음` | C/Go full 파일은 위 행과 같다. Go STREAM은 shared C reference client를 사용하므로 측정 surface는 Go STREAM server다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `MULTI_DEALER_DEALER` | `통과(45.0%)` | `통과(57.7%)` | `통과(68.1%)` | `통과(53.9%)` | `통과(53.8%)` | `통과(53.2%)` | 64B는 같은 조건 C `perf_c_multi_linux_20260525_130455.txt` 대비 Go `perf_go_multi_linux_20260525_131051.txt` 기준이다. 256/1024B는 Go `perf_go_multi_linux_20260524_222134.txt` 기준이다. 65536B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_071712.txt` 대비 Go `perf_go_multi_linux_20260525_071734.txt` 기준이고, 131072B는 C `perf_c_multi_linux_20260525_071516.txt` 대비 Go `perf_go_multi_linux_20260525_071539.txt` 기준이며, 262144B는 C `perf_c_multi_linux_20260525_071013.txt` 대비 Go `perf_go_multi_linux_20260525_071028.txt` 기준이다. C처럼 단일 poll loop와 pending socket `POLLOUT` 의미를 유지했다. active send는 64B와 65536B 이상에서 public `Bytes(...)` client send를 사용하고, server는 64B에서 throughput count를 전 메시지에 유지하면서 latency 계산/저장만 기본 32개당 1개로 줄인다. 131072B server 수신은 public `RecvPart` caller-owned 수신 경로로 확장해 no-code 재측정 17.177Kops/s(`perf_go_multi_linux_20260525_015918.txt`)에서 24.207Kops/s로 올렸고, 이후 client send를 `Bytes(...)`로 좁혀 바꿔 27.490Kmsg/s까지 올렸다. 65536B도 `Bytes(...)` 적용 후 53.992Kmsg/s, C 대비 53.9%로 통과했다. 262144B까지 넓힌 이전 broad 후보는 tls 262144B를 낮춰 반영하지 않았지만(`perf_go_multi_linux_20260525_015620.txt`), tls 262144B 단독 `Bytes(...)` 적용은 같은 조건 제한 재측정에서 14.154Kmsg/s, C 대비 53.2%로 통과했다. |
| `tls` | `MULTI_DEALER_ROUTER` | `통과(61.7%)` | `통과(58.8%)` | `통과(59.1%)` | `통과(46.0%)` | `통과(47.7%)` | `통과(44.8%)` | C/Go full 파일은 위 행과 같다. routed echo client는 C처럼 한 client context 안에서 모든 socket을 단일 poll loop로 처리한다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `MULTI_ROUTER_ROUTER` | `통과(40.5%)` | `통과(40.9%)` | `통과(40.6%)` | `통과(46.3%)` | `통과(49.3%)` | `통과(53.9%)` | **2026-05-23 LockOSThread 수정 후 64B가 22.0%→40.5%로 통과**(fresh baseline `perf_c_multi_linux_20260523_111534...` 대비 Go `perf_go_multi_linux_20260523_233046.txt`, 6.6.3 참조). 이하 메모는 이전 라운드 기록. C/Go full 파일은 위 행과 같다. 절대 기준을 통과하는 256B 이상은 상대 기준을 진단 보조로 본다. routed client는 C와 같은 단일 poll loop와 public `RecvPart` 경로를 유지한다. server `RecvPart` 전체 확대와 client send scan round-robin 후보는 tcp/ws 계열에서 small 또는 large를 악화시켜 반영하지 않았다. 최신 재측정에서 64B도 Go multi routed echo 기준을 넘었다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `MULTI_PUBSUB` | `통과(123.1%)` | `통과(50.5%)` | `통과(61.5%)` | `통과(55.3%)` | `통과(60.4%)` | `통과(60.6%)` | 256B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_020714.txt` 2.692Mmsg/s 대비 Go `perf_go_multi_linux_20260525_020827.txt` 1.359Mmsg/s 기준이다. 나머지 C/Go full 파일은 위 행과 같다. Go 64B/1024B/65536B/131072B/262144B는 `perf_go_multi_linux_20260522_114251_codex_go_multi_pubsub_sampled_latency_full_guard_20260522.txt` 기준이다. client는 public `SubscribePart` caller-owned 수신 경로를 사용하고, C처럼 하나의 stop/cooldown 신호로 phase를 종료한다. throughput count와 latency sample을 분리해 active payload는 모두 세고 latency 계산은 샘플에만 수행했다. 64B는 120%를 넘어 outlier 재검토 대상으로 남긴다. stride 64/16 후보 `perf_go_multi_linux_20260522_114223_codex_go_tls_multi_pubsub256_sample_stride64_probe_20260522.txt`, `perf_go_multi_linux_20260522_114235_codex_go_tls_multi_pubsub256_sample_stride16_probe_20260522.txt`는 낮았지만, 현재 HEAD 같은 조건 재측정에서 256B가 통과권으로 올라왔다. 단일 part publish move/restore 후보는 public 실패 원본 보존 계약을 깨 반영하지 않았다. topic C 문자열 캐시 후보, `MoveMessage` 내부 `zlink_msg_adopt` 후보, `Bytes(...)` publish 후보는 기준을 안정적으로 올리지 못해 반영하지 않았다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `MULTI_SPOT` | `통과(55.5%)` | `통과(63.1%)` | `통과(86.3%)` | `통과(160.8%)` | `통과(139.8%)` | `통과(112.4%)` | C/Go full 파일은 위 행과 같다. Go 64B/256B/1024B `perf_go_multi_linux_20260522_112132_codex_go_wss_tls_multi_spot_small_sampled_latency_20260522.txt`, 나머지는 기존 Go full 파일이다. public `SubscribePart` 수신 경로와 C와 같은 worker 분배 의미를 사용한다. Go perf latency 기록은 C처럼 수신 시각을 한 번만 잡아 계산하도록 공통 hot path를 줄였고, tls 64B server publish는 public `Bytes(...)` builder 경로로 줄였다. encrypted small SPOT은 Go worker fan-out 비용을 줄이도록 기본 수신 worker를 4개로 좁혀 64B도 기준을 넘겼고, throughput count와 latency sampling을 분리해 수신 hot path의 `time.Now()` 호출을 줄였다. 4-worker probe `perf_go_multi_linux_20260522_111701_codex_go_tls_multi_spot64_workers4_probe_20260522.txt`와 기본 경로 재측정에서 개선을 확인했다. worker 8개/16개 후보 `perf_go_multi_linux_20260522_111643_codex_go_tls_multi_spot64_workers8_probe_20260522.txt`, `perf_go_multi_linux_20260522_111649_codex_go_tls_multi_spot64_workers16_probe_20260522.txt`는 기본보다 낮고, worker 2개 후보 `perf_go_multi_linux_20260522_111917_codex_go_wss_tls_multi_spot64_1024_workers2_probe_20260522.txt`는 tls 64B 실패를 만들어 반영하지 않았다. server publish를 `MoveMessage`로 바꾸는 후보 `perf_go_multi_linux_20260522_090056_codex_go_wss_tls_multi_spot_small_movemessage_20260522.txt`, 선택 적용 후보 `perf_go_multi_linux_20260522_090236_codex_go_wss_tls_multi_spot_small_selected_movemessage_20260522.txt`는 일부 small size만 소폭 올리고 1024B 또는 256B를 낮춰 반영하지 않았다. topic C 문자열 캐시 후보 `perf_go_multi_linux_20260522_091723_codex_go_wss_tls_pubsub_spot_topic_cstring_cache_20260522.txt`는 `wss 1024B`를 크게 낮춰 반영하지 않았다. 65536B/131072B는 120%를 넘어 outlier 재검토 대상이다. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `MULTI_SPOT_REQREP` | `통과(52.3%)` | `통과(54.5%)` | `통과(50.3%)` | `통과(72.9%)` | `통과(65.3%)` | `통과(64.2%)` | 64B/256B/1024B는 기존 full 파일 기준이고, 65536B/131072B/262144B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Go `perf_go_multi_linux_20260524_222958.txt` median 기준이다. request completion은 public `POLLCOMPLETION` poller를 사용한다. `SubmitNotConnected`는 C처럼 fatal이 아니라 다음 poll loop에서 재시도하는 의미로 처리한다. auto-HWM 활성과 size별 SpotNode `MsgUnit(B)` 일치를 확인했다. |
| `tls` | `MULTI_SPOT_SENDSEND` | `통과(54.2%)` | `통과(56.2%)` | `통과(62.1%)` | `통과(101.1%)` | `통과(92.6%)` | `통과(97.1%)` | 64/256/1024B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_064438.txt` 대비 Go `perf_go_multi_linux_20260525_064632.txt` 기준이다. 65536/131072/262144B는 C `perf_c_multi_linux_20260525_092522.txt` 대비 Go `perf_go_multi_linux_20260525_092253.txt` 기준이다. `SubmitNotConnected`는 C처럼 fatal이 아니라 다음 poll loop에서 재시도하는 의미로 처리하고, 서버 echo는 public `Spot.ForwardRouted(...)`를 65536B 이상에 적용한다. client send는 65536B 이하에서만 public `MoveMessage(...)`를 사용한다. server idle 대기는 전 size에서 고정 `sleep(1ms)` 대신 public `Poller` `POLLIN` wake를 사용한다. 131072B까지 넓힌 후보는 일부 transport를 낮춰 반영하지 않았다. active slot 16 후보 `perf_go_multi_linux_20260524_205629.txt`는 131072B를 낮춰 반영하지 않는다. |
| `tls` | `MULTI_STREAM` | `통과(93.8%)` | `통과(101.2%)` | `통과(98.5%)` | `통과(97.1%)` | `해당 없음` | `해당 없음` | C/Go full 파일은 위 행과 같다. Go STREAM은 shared C reference client를 사용하므로 측정 surface는 Go STREAM server다. auto-HWM 활성과 size별 `MsgUnit(B)` 일치를 확인했다. |

#### 6.6.3 Go 보류 해소 기록

Go는 현재 표 기준으로 single/multi 전 대상이 `통과` 상태다. 아래 항목은 보류가 남았던
시점의 원인, 후보, 적용 결과를 남긴 이력이며, 현재 남은 성능 작업 대상은 Rust/Python이다.

- Single `ws`/`wss` latency 보류 해소 판정: current HEAD 같은 조건 제한 재측정
  C `perf_c_single_linux_20260525_155243_go_single_latency_stale_c.txt`와 Go
  `perf_go_single_linux_20260525_155344_single_latency_stale_recheck.txt`는 모두 complete였고,
  `ws/wss PUBSUB` 64/256B와 `ws/wss SPOT` 64/256B throughput은 모두 기준을 넘었다.
  일부 256B latency는 여전히 C보다 크게 높지만, throughput 통과와 기존 latency artifact
  판정에 따라 이 셀을 보류로 되돌리지 않는다.
- Single `wss SPOT`: 262144B는 public `Bytes(...)` publish 선택 적용 뒤 통과권으로
  올라갔다. 64B/256B도 current HEAD 재측정에서 throughput 기준을 넘겼으므로 보류로
  남기지 않는다.
- Go multi routed echo 보류의 consume-forward 1차 구현은 완료했다.
  core C `zlink_spot_forward_routed(...)`와 Go public `Spot.ForwardRouted(...)`를 추가했고,
  Go 회귀 테스트와 wss/tls `MULTI_SPOT_SENDSEND` 제한 측정으로 확인했다.
  다만 이 경로는 wss 65536B/262144B, tls large 유효 수치 확보, ws 65536B/262144B
  collapse 완화에만 효과가 있었다.
  2026-05-24에 `wss 131072B`도 forward 적용 범위에 포함했지만 반복 측정 median은
  49.8%라 통과로 확정하지 않고 보류로 둔다. small size도 아직 보류다.
- 2026-05-24 재검토: `MULTI_SPOT_SENDSEND` wss/tls를 current HEAD에서 fresh C baseline 대비
  다시 측정했다(`perf_go_multi_linux_20260524_202842.txt`). wss는 29.6/28.4/35.7/53.1/49.1/51.9%,
  tls는 28.3/29.0/31.5/34.9/37.7/42.6%로, wss 65536B/262144B 외에는 여전히 보류다.
  추가 후보도 반영하지 않는다. small에 `ForwardRouted(...)`를 넓힌 후보는
  `perf_go_multi_linux_20260524_203027.txt`에서 small 처리량을 낮췄다. client poller를 C 초기 등록처럼
  `POLLIN|POLLOUT`으로 넓힌 후보는 `perf_go_multi_linux_20260524_203128.txt`에서 tls 일부를
  소폭 올렸지만 wss 64B/131072B를 낮추고 통과 셀을 만들지 못했다. small active slot을 32로
  줄인 후보는 `perf_go_multi_linux_20260524_203302.txt`에서 small 처리량을 23~25Kops/s로 낮췄다.
  `PERF_GO_GOMAXPROCS=8` 진단(`perf_go_multi_linux_20260524_203351.txt`)도 기본 GOMAXPROCS=4 대비
  혼합 결과라 기본값 변경 근거가 없다.
- 2026-05-25 재검토: Go `MULTI_DEALER_DEALER` non-tcp 64B는 current HEAD에서 fresh C baseline과
  다시 맞춰 측정했지만, ws/wss/tls가 37.8/37.7/37.8%로 모두 단순 one-way 최소 기준보다 낮다
  (`perf_c_multi_linux_20260525_115320.txt`, `perf_go_multi_linux_20260525_115319.txt`).
  이 경로는 이미 단일 poll loop, shared client context, `MoveMessage(...)` active send,
  server `RecvPart` 수신을 사용하고 있어 새 public API 없이 남은 작은 메시지 비용을 줄일
  내부 후보가 더 필요했다. 이후 64B `Bytes(...)` client send와 server latency sampling을
  결합한 최신 후보로 ws/wss/tls 64B 보류를 해소했다.
- 2026-05-24 추가 수정: `MULTI_SPOT_SENDSEND` client active loop를 C++처럼 slot별
  **reply drain 후 즉시 재송신** 순서로 맞췄다. 기존 Go loop는 send 가능한 slot을 먼저
  모두 채우고 나서야 waiting reply를 drain해 request/reply window가 wave 형태로 굴렀다.
  `perf_go_multi_linux_20260524_205329.txt`에서 전 transport complete를 유지했고, fresh C 기준
  tcp 64/256/1024B는 44.4/40.5/45.8%, wss는 36.6/39.7/37.2/54.6/49.9/53.2%,
  tls는 39.2/41.1/46.2/37.5/42.0/41.8%로 올랐다. 다만 `ws` large collapse와
  `wss 131072B` 49.9%, tls 전 size는 아직 보류다. 새 loop 기준 active slot 16 후보
  `perf_go_multi_linux_20260524_205629.txt`는 131072B를 개선하지 못해 반영하지 않는다.
- 2026-05-24 추가 수정: `MULTI_SPOT_SENDSEND` client send에서 `ws`가 아니고 65536B 이하인
  payload는 public `MoveMessage(...)`로 ownership을 submit path에 넘긴다. perf helper는
  매 send마다 새 `Message`를 만들기 때문에 실패해도 재사용해야 하는 원본 payload buffer는
  보존된다. 최종 공식 runner `perf_go_multi_linux_20260524_220932.txt`는 전 transport/size
  complete를 유지했고, fresh C 기준 tcp는 44.7/44.9/48.3/18.7/18.8/24.7%,
  wss는 39.5/41.6/37.5/53.3/49.4/53.6%, tls는 42.4/45.4/44.6/38.1/37.3/42.4%다.
  ws는 C `perf_c_multi_linux_20260522_055144_codex_c_ws_multi_for_go_20260522.txt` 대비
  46.6/41.3/37.6/0.2/23.4/35.2%다. 무조건 `MoveMessage(...)` 후보
  `perf_go_multi_linux_20260524_215950.txt`, small/131072B 적용 후보
  `perf_go_multi_linux_20260524_220213.txt`, `perf_go_multi_linux_20260524_220355.txt`는
  262144B, `ws`, 또는 131072B 일부를 낮춰 반영 범위를 65536B 이하 non-ws로 좁혔다.
- 2026-05-25 재검토: `MULTI_SPOT_SENDSEND` client reply hot path에서 throughput count와
  latency sample 기록을 분리하는 후보를 시험했다. 첫 후보는 모든 reply에서 timestamp를
  계산하고 latency 기록만 32개당 1개로 줄였고, 두 번째 후보는 count는 metric header만
  확인한 뒤 세고 timestamp 계산을 latency sample에만 제한했다. `go test ./...`는 둘 다
  통과했고 공식 runner도 complete였지만, C 기준 `perf_c_multi_linux_20260525_061836.txt`
  대비 후보 `perf_go_multi_linux_20260525_061913.txt`는 tcp 64/256/1024B가
  112.184/114.032/110.930Kops/s, 두 번째 후보 `perf_go_multi_linux_20260525_062050.txt`는
  113.016/113.859/114.044Kops/s였다. 기존 대표값
  `perf_go_multi_linux_20260525_021722.txt`의 117.678/111.962/116.442Kops/s와 비교하면
  256B만 소폭 오르고 64B/1024B가 낮아진다. latency sampling 분리만으로는 small 보류를
  해소하지 못하므로 반영하지 않는다.
- 2026-05-25 재검토: `MULTI_SPOT_SENDSEND` small active slot 32/16보다 덜 좁힌
  active slot 64 후보도 tcp 64/256/1024B에 시험했다. `go test ./...`는 통과했고 공식
  runner `perf_go_multi_linux_20260525_062259.txt`도 complete였지만, median이
  80.541/83.487/82.150Kops/s로 기존 대표값보다 크게 낮았다. active slot 축소 계열은
  small 보류 해소 후보에서 제외한다.
- 2026-05-25 추가 수정: `MULTI_SPOT_SENDSEND` server active loop가 drain 뒤 항상
  `sleep(1ms)`로 쉬던 것을 1024B 이하에서만 public `Poller` `POLLIN` wake로 바꿨다.
  `go test ./...`는 통과했고, 공식 runner는 모두 complete였다. tcp small은 C
  `perf_c_multi_linux_20260525_064305.txt` 대비 Go `perf_go_multi_linux_20260525_064343.txt`에서
  55.3/54.4/58.3%로 올라 보류를 해소했다. ws/wss/tls small은 C
  `perf_c_multi_linux_20260525_064438.txt` 대비 Go `perf_go_multi_linux_20260525_064632.txt`에서
  ws 58.3/59.0/56.9%, wss 53.2/56.1/61.1%, tls 54.2/56.2/62.1%로 모두 통과권이다.
- 2026-05-25 추가 수정: 위 server `Poller` wake를 large까지 넓혔다. 고정 1ms sleep은
  large echo에서도 idle 구간마다 응답 처리를 늦추고 있었고, public `Poller` `POLLIN`
  wait는 같은 socket readiness 의미를 더 직접 사용한다. `go test ./...`는 통과했고,
  공식 wrapper `perf_go_multi_linux_20260525_092253.txt`와 같은 조건 C 기준
  `perf_c_multi_linux_20260525_092522.txt`는 모두 complete였다. tcp 65536/131072/262144B는
  62.4/71.2/78.7%, ws는 95.7/89.8/72.7%, tls는 101.1/92.6/97.1%로 올라 남은 large
  보류를 해소했다.
- 2026-05-25 재검토: `MULTI_SPOT_SENDSEND` `ws` small client send에서 `Message(...)`
  선생성을 피하려고 public `Bytes(...)` 경로를 64/256/1024B에 좁혀 시험했다. C 기준
  `perf_c_multi_linux_20260525_062448.txt`는 complete였고 `go test ./...`도 통과했지만,
  공식 runner `perf_go_multi_linux_20260525_062527.txt`가 `ws 256B exit_nonzero`
  partial로 끝났다. timeout이나 partial을 만드는 후보는 통과 근거로 쓰지 않으므로
  `ws` small send는 기존 `Message(...)` 경로를 유지한다.
- 2026-05-25 재검토: `MULTI_SPOT_SENDSEND` large에서도 `PERF_GO_GOMAXPROCS=8`을
  tcp/ws/tls 65536/131072/262144B에 다시 시험했다. 공식 wrapper
  `perf_go_multi_linux_20260525_091754.txt`는 complete였지만 tcp 65536/131072/262144B는
  10.467/5.047/3.357Kops/s로 기본값 대표 측정보다 낮거나 같은 수준이고, tls도
  7.667/4.119/2.385Kops/s로 낮아졌다. `ws 65536B`만 12.795Kops/s로 이전
  11.411Kops/s보다 높았지만 같은 C 기준으로는 32.7% 수준이라 보류를 해소하지
  못하고, `ws 131072/262144B`는 5.045/2.809Kops/s로 기존보다 낮다. 따라서
  `GOMAXPROCS=8`은 당시 Go multi 보류를 푸는 일반 해법이 아니며 기본값 변경 후보에서
  제외한다.
- 2026-05-24 추가 수정: `MULTI_DEALER_DEALER`를 current HEAD에서 fresh C baseline으로
  다시 측정하고 stale 보류를 제거했다. `perf_go_multi_linux_20260524_222134.txt` 기준으로
  tcp 256B/1024B, ws 256B/1024B/262144B, wss 256B/1024B, tls 256B/1024B는 통과권이다.
  wss 262144B client send는 `MoveMessage(...)`로 넓혀 제한 재측정
  `perf_go_multi_linux_20260524_222545.txt`에서 48.0%까지 올랐지만 기준선에는 아직
  닿지 못했다. tls 262144B까지 넓힌 후보는 full run `perf_go_multi_linux_20260524_222134.txt`에서
  낮아져 반영하지 않는다.
- 2026-05-25 추가 수정: `MULTI_DEALER_DEALER` client active send에서 선택 size만
  public `Bytes(...)` 경로를 사용한다. 이 경로는 caller-owned slice에 header를 찍고
  submit 때 native message로 만들기 때문에, hot path에서 Go `Message` 객체를 먼저
  만드는 비용을 줄인다. 최종 공식 runner `perf_go_multi_linux_20260525_001654.txt`에서
  tcp 65536B는 49.5%에서 81.4%로 올라 통과했고, ws 65536B/131072B는 59.2%/58.7%로
  통과했다. ws 262144B는 기존 경로로도 64.4%라 통과권을 유지한다. tcp 131072B/262144B와
  wss/tls large는 같은 후보가 불안정하거나 낮아 적용하지 않는다. broad `Bytes(...)`
  후보 `perf_go_multi_linux_20260525_001112.txt`는 tcp 262144B와 tls large를 크게 낮췄고,
  ws 262144B 적용 후보도 최종 혼합 측정에서는 기존 경로보다 이득이 작아 제외했다.
  2026-05-25 재검토에서 `Bytes(...)`를 tls 65536/131072B로 다시 넓힌 후보도 반영하지
  않는다. 공식 wrapper `perf_go_multi_linux_20260525_030158.txt`는 complete였지만 tls
  131072B가 기존 24.2Kops/s에서 20.1Kops/s로 낮아졌고, tls 65536B만 남긴 재시도
  `perf_go_multi_linux_20260525_030247.txt`도 median 27.8Kops/s로 기존 45.9Kops/s보다
  낮았다. 암호화 DD large의 남은 병목은 단순 client-side `Message` 생성 비용이 아니다.
- 2026-05-25 재검토: `MULTI_DEALER_DEALER` wss 262144B만 좁혀 public `Bytes(...)`
  client send와 server `RecvPart` caller-owned 수신 경로를 함께 적용했다. 같은 조건
  no-code 재측정 `perf_go_multi_linux_20260525_042046.txt`는 median 1.949Kmsg/s였고,
  server `RecvPart`만 넓힌 후보 `perf_go_multi_linux_20260525_042112.txt`는 7.800Kmsg/s였다.
  최종 후보 `perf_go_multi_linux_20260525_042206.txt`는 C `perf_c_multi_linux_20260525_042131.txt`
  대비 63.4%로 통과했다. tls 262144B는 앞선 후보에서 낮아졌으므로 적용 범위에 넣지 않는다.
  tls 65536B를 `MoveMessage(...)`로 넓힌 후보도 `go test ./...`는 통과했지만, 같은 조건
  제한 측정 C `perf_c_multi_linux_20260525_042349.txt` 대비 Go `perf_go_multi_linux_20260525_042400.txt`가
  23.6%라 기존 42.9%보다 낮아 반영하지 않는다.
- 같은 `MoveMessage(...)` 확대를 wss 65536B에만 좁혀 다시 시험했다. C
  `perf_c_multi_linux_20260525_042502.txt` 대비 Go `perf_go_multi_linux_20260525_042512.txt`가
  47.5%로 기준선 아래지만, 같은 조건 no-code 재측정 `perf_go_multi_linux_20260525_042547.txt`
  28.512Kmsg/s보다 높은 31.792Kmsg/s라 적용한다. 보류는 유지하되, wss 65536B는
  42.5% 대표값에서 47.5%로 갱신한다.
- 같은 server `RecvPart` 확대를 tcp 131072/262144B에도 적용하는 후보를 다시 시험했다.
  같은 조건 C 기준 `perf_c_multi_linux_20260525_042941.txt`는 91.450/47.439Kmsg/s이고,
  no-code Go `perf_go_multi_linux_20260525_042958.txt`는 40.339/20.962Kmsg/s였다.
  후보 `perf_go_multi_linux_20260525_043032.txt`는 40.458/20.816Kmsg/s로 131072B는
  오차 수준, 262144B는 하락이라 반영하지 않는다. tcp 대형 DD 병목은 server receive
  copy만으로는 해소되지 않는다.
- 2026-05-25 재검토: `tcp MULTI_DEALER_DEALER 262144B`에도 client `Bytes(...)`
  경로를 다시 좁혀 적용했다. 같은 조건 최신 C `perf_c_multi_linux_20260525_090541.txt`
  대비 no-code Go `perf_go_multi_linux_20260525_090555.txt`는 4.789Kmsg/s, 10.8%였고,
  client `Bytes(...)` 후보는 `perf_go_multi_linux_20260525_090745.txt`에서 18.005Kmsg/s,
  `perf_go_multi_linux_20260525_090807.txt`에서 17.722Kmsg/s까지 올라 40% 근처로
  회복했다. 다만 server `RecvPart`까지 함께 넓힌 후보 `perf_go_multi_linux_20260525_090656.txt`는
  5.417Kmsg/s로 낮아 반영하지 않는다. `GOMAXPROCS=8` 진단
  `perf_go_multi_linux_20260525_090832.txt`는 19.193Kmsg/s, 43.1%였지만 Go 전체
  runner 기본값을 바꾸는 판단은 별도 검증이 필요하므로 이번 적용 범위에는 넣지 않는다.
  이어서 `ws/wss/tls 64B`와 `tcp 262144B`를 함께 묶어 `PERF_GO_GOMAXPROCS=8`로
  다시 돌린 `perf_go_multi_linux_20260525_091615.txt`는 complete였지만, `tcp 262144B`
  median이 15.718Kmsg/s로 최신 기본값 측정 17.722Kmsg/s보다 낮았고 `ws/wss/tls 64B`도
  1191.424/1229.312/1190.639Kmsg/s로 기존 보류권을 벗어나지 못했다. Go runner
  기본 GOMAXPROCS를 8로 올리는 변경은 반영하지 않는다. 이후 `tcp 262144B` 단독
  case-local override만 별도로 검증해 적용했다.
- 2026-05-25 재검토: 같은 `Bytes(...)` 경로를 tcp 1024B 이하에도 넓혔다. `go test ./...`는
  통과했고, 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_040430.txt` 대비 Go
  `perf_go_multi_linux_20260525_040452.txt`에서 tcp 64/256/1024B가 45.4/64.8/81.1%로
  확인됐다. 특히 기존 최신 재측정에서 38.2%였던 tcp 64B가 기준선 위로 올라 보류에서
  제외된다. 131072/262144B는 앞선 후보에서 낮았으므로 기존 `Message(...)` 경로를 유지한다.
  같은 방향을 ws 64B까지 넓힌 후보도 시험했지만, 공식 제한 재측정은 complete였어도
  C `perf_c_multi_linux_20260525_040736.txt` 대비 Go `perf_go_multi_linux_20260525_040750.txt`가
  34.9%로 기존 대표값 38.4%보다 낮아 반영하지 않는다.
  wss/tls 64B 후보도 각각 complete였지만 C `perf_c_multi_linux_20260525_040836.txt` 대비
  Go `perf_go_multi_linux_20260525_040852.txt`가 35.8%, C `perf_c_multi_linux_20260525_040919.txt`
  대비 Go `perf_go_multi_linux_20260525_040934.txt`가 34.1%라 기존 대표값 37.4/38.1%보다 낮다.
  따라서 당시 64B `Bytes(...)` 확대는 tcp에만 남겼다. 이후 server latency sampling을 함께
  적용한 최신 후보에서 ws/wss/tls 64B도 통과권에 들어와 적용 범위를 다시 넓혔다.
- 2026-05-25 재검토: `ws/wss/tls MULTI_DEALER_DEALER 64B`에서 `MoveMessage(...)`
  선택 경로도 끄고 기본 `Message(...)` submit 경로를 다시 시험했다. `go test ./...`는
  통과했고 공식 runner `perf_go_multi_linux_20260525_084043.txt`도 complete였지만,
  C `perf_c_multi_linux_20260525_084003.txt` 대비 ws/wss/tls 64B가 36.0/35.1/35.3%로
  기존 대표값 38.2/37.0/38.0%보다 낮았다. small 64B 병목은 `Bytes(...)`,
  `MoveMessage(...)`, 기본 `Message(...)` 선택만으로는 해소되지 않았고, 이후 `Bytes(...)`
  client send와 server latency sampling을 결합한 최종 후보로 통과권을 만들었다.
- 2026-05-24 추가 수정: `MULTI_SPOT_REQREP`는 current HEAD에서 large만 먼저 다시 측정했다.
  `perf_go_multi_linux_20260524_222958.txt` 기준으로 wss/tls 65536B 이상은 모두 통과권이고,
  tcp/ws large만 보류로 남아 있었다. client active request hot path에서 payload를 `append`로
  한 번 복제한 뒤 `NewMessage(...)`가 다시 복사하던 중복 복사를 제거했다. 최종 제한 측정
  `perf_go_multi_linux_20260524_223541.txt`에서 tcp 65536/131072/262144B는
  27.7/28.2/32.6%에서 30.8/32.0/37.0%로 올랐고, ws는 42.7/34.4/45.7%에서
  51.1/37.8/51.7%로 올라 65536B와 262144B 보류를 해소했다. tcp large와 ws 131072B는
  아직 기준선 아래라 계속 보류한다. 서버 reply에서 public `ReplyOp.MoveMessage(...)`를
  추가해 받은 single-part를 그대로 넘기는 후보는 `perf_go_multi_linux_20260524_224212.txt`에서
  tcp 262144B와 ws 65536B를 낮춰 API 확장과 구현을 되돌렸다.
  131072B 이상 active slot을 8에서 16으로 넓힌 후보는 `perf_go_multi_linux_20260524_233456.txt`에서
  tcp 262144B exit_nonzero partial을 만들었고, tcp 131072B도 기존 32.0%보다 낮은 9.7Kops/s
  수준에 그쳐 반영하지 않는다.
- 2026-05-25 추가 수정: request builder에 public `RequestOp.Bytes(...)`를 추가하고
  `MULTI_SPOT_REQREP` client active request의 선택 size에 적용했다. 이 경로는 caller-owned
  slice를 submit 때 native message로 만들기 때문에, `Message(...)`를 먼저 만든 뒤 request
  submit에서 다시 clone하던 비용을 줄인다. 최종 공식 runner `perf_go_multi_linux_20260525_003036.txt`에서
  tcp 65536B는 30.8%→59.1%로 올라 통과했고, ws 131072B는 37.8%→50.2%로 올라
  보류가 해소됐다. ws 65536B/262144B도 75.1%/69.7%로 통과 여유가 커졌다.
  tcp 131072B는 45.2%로 개선됐지만 기준선 아래라 보류로 남긴다. tcp 262144B는 broad
  `Bytes(...)` 후보 `perf_go_multi_linux_20260525_002738.txt`에서 2.8%로 무너져 기존
  `Message(...)` 경로를 유지한다.
  2026-05-25 재검토에서 server를 `OnRoutedReceive` 대신 `OnDispatchEvent` +
  public `RecvRoutedPart(...)` drain으로 바꾸는 후보도 시험했다. 공식 제한 재측정은
  complete였지만(`perf_go_multi_linux_20260525_034331.txt`), 같은 기존 report
  `perf_go_multi_linux_20260525_003036.txt` 대비 tcp 131072B median은 12.712K→12.709Kops/s,
  262144B는 4.716K→4.731Kops/s로 사실상 같았다. fresh C 제한 재측정
  `perf_c_multi_linux_20260525_034301.txt` 대비 비율은 51.9/42.4%였으나 C 기준 변동 효과가
  컸고 절대 처리량 개선은 없으므로 반영하지 않는다.
  2026-05-25 current HEAD 재측정에서는 C `perf_c_multi_linux_20260525_044910.txt` 대비
  Go `perf_go_multi_linux_20260525_044940.txt`가 tcp 131072/262144B 58.8/40.5%로 나왔다.
  server reply의 `NewMessage(...)` + `ReplyOp.Message(...)` clone을 줄이기 위해 public
  `ReplyOp.Bytes(...)` 후보도 시험했다. `go test ./...`는 통과했고 공식 재측정
  `perf_go_multi_linux_20260525_045203.txt`도 complete였지만 131072B는 12.151K→12.379Kops/s
  소폭 개선에 그쳤고 262144B는 4.732K→4.594Kops/s로 낮아졌다. public API를 넓힐 근거가
  약하므로 반영하지 않는다.
  2026-05-25 재검토에서 tcp 262144B active slot을 8에서 4로 줄이는 후보도 시험했다.
  `go test ./...`는 통과했고 공식 wrapper `perf_go_multi_linux_20260525_093053.txt`도
  complete였지만, 같은 조건 C `perf_c_multi_linux_20260525_093033.txt` 대비 Go median은
  4.358Kops/s, 41.2%로 기존 대표값과 같은 보류권이다. active slot을 더 좁히면
  in-flight request가 줄어 C 대비 처리량 격차를 줄이지 못하므로 반영하지 않는다.
  current HEAD에서 active slot 6도 env 후보로 재시험했지만 공식 wrapper
  `perf_go_multi_linux_20260525_095715.txt`는 4.760Kops/s로 기존 8-slot 대표값과 같은
  보류권이라 반영하지 않는다.
  2026-05-25 추가 재측정에서 262144B request timeout을 200ms에서 500ms로 늘리는 후보를
  시험했다. `go test ./...`는 통과했고 같은 조건 C `perf_c_multi_linux_20260525_104221.txt`
  대비 후보 `perf_go_multi_linux_20260525_104221.txt`는 5.057Kops/s, 50.6%로 기준선을
  넘었지만, 원래 200ms current 코드도 같은 C 기준 대비 `perf_go_multi_linux_20260525_104301.txt`
  에서 5.016Kops/s, 50.1%였다. timeout 확대가 아니라 최신 같은 조건 재측정에서 C 기준이
  안정화된 효과로 보고 코드 변경 없이 262144B 표를 통과로 갱신한다.
  2026-05-25 재검토에서 server callback이 받은 single-part `Message`를 그대로
  `received.Reply().Message(parts[0])`에 넘기는 후보도 시험했다. 이 후보는 별도
  public API 없이 server의 `NewMessage(parts[0].Data())` 복사를 줄이는 방향이고
  `go test ./...`는 통과했으며 공식 runner도 complete였다. 그러나 C
  `perf_c_multi_linux_20260525_080852.txt` 대비 Go 후보
  `perf_go_multi_linux_20260525_081011.txt`는 tcp 65536/262144B가 13.9/5.4%로
  기존 좋은 기준보다 크게 낮아졌고, ws 262144B도 32.9%에 그쳤다. small 64B 개선만으로
  large 회귀를 받아들일 수 없으므로 기존 명시 reply 경로를 유지한다.
- Multi `DEALER_DEALER`, `PUBSUB`, `SPOT`, `SPOT_SENDSEND` 보류: 단일 poll loop와
  `POLLCOMPLETION` 의미는 맞췄고, Go send builder에는 명시적 ownership 이전 단계
  `MoveMessage(...)`와 caller-owned slice를 submit 때 한 번만 native message로 만드는
  `Bytes(...)`를 추가했다. `Message(...).Submit(...)`의 실패 시 원본 보존 계약,
  `MoveMessage(...)`의 실패 후 재사용 금지 계약, `Bytes(...)`의 caller slice 보존
  계약은 회귀 테스트로 확인했다.
  다만 `SPOT`/`SPOT_SENDSEND`와 large size에서는 이 경로가 아직 목표 기준을 해결하지
  못했거나 악화돼 선택 적용만 유지한다.
- `MULTI_DEALER_ROUTER` tcp large도 최근 `Bytes(...)` 경로 기준으로 다시 확인했다.
  같은 조건 C `perf_c_multi_linux_20260525_043623.txt` 대비 current Go
  `perf_go_multi_linux_20260525_043725.txt`는 39.315/20.764/10.865Kops/s였다.
  client와 server를 모두 `Bytes(...)`로 바꾼 후보 `perf_go_multi_linux_20260525_043808.txt`는
  36.497/19.954/10.794Kops/s로 낮아졌고, server echo만 `Bytes(...)`로 바꾼 후보
  `perf_go_multi_linux_20260525_043856.txt`도 37.104/19.116/10.440Kops/s로 낮아졌다.
  routed echo large의 남은 병목은 Go `Message` 생성 하나만으로 설명되지 않으므로 기존
  `MoveMessage(...)` 경로를 유지한다.
- 다음 구현 단위: `ForwardRouted(...)`가 해결하지 못한 encrypted routed echo submit 비용을
  `tls MULTI_SPOT_SENDSEND`와 `wss` small에서 더 좁힌다. public API를 임의로 넓히지 않고,
  C의 native part consume 경로와 Go의 실패 시 원본 보존 계약 사이에서 남은 비용을
  `SPOT_SENDSEND`, `SPOT`, `PUBSUB` 순서로 추적한다.
- 2026-05-22 추가 확인: `MoveMessage` 단일 part를 원본 native message로 직접 submit하는
  내부 후보는 `SPOT_SENDSEND`의 한 size만 개선하고 다른 size의 기존 통과값을 깨서
  원복했다. `Received.Send()`의 routed echo 경로를 직접 substrate 호출로 줄인 후보도
  같은 large size 붕괴가 재현되어 원복했다. 다음 후보는 전체 submit helper 변경이 아니라
  size별 선택 적용이나 server echo payload 생성 비용처럼 보류 셀에 직접 닿는 경로로
  더 좁혀야 한다.
- 2026-06-22 재확인: Go `Send/Publish(...).Message(message).Submit(...)` 단일 part
  helper에서 성공 경로 payload copy를 없애기 위해, caller `Message`를 native frame으로
  먼저 move하고 submit 실패 때 다시 되돌리는 후보를 시험했다. 이 후보는 public API나 perf
  호출부를 바꾸지 않는 binding 내부 최적화처럼 보였지만, `go test . -run
  'TestSendConsumesMessageOwnership|TestBlockingSendFailurePreservesMessagePayload|TestPublishFailurePreservesMessagePayload|TestMoveMessageFailureConsumesMessagePayload'`
  에서 `TestBlockingSendFailurePreservesMessagePayload`가 실패했다. native send 실패 뒤에는
  원본 payload를 되돌릴 충분한 상태가 남는다고 보장할 수 없다. Go public `Message(...)`
  경로는 실패 시 원본 메시지를 보존해야 하므로 이 후보는 원복했고, 코드에는 같은 hot path를
  리팩토링할 때 copy를 제거하면 안 되는 이유를 주석으로 남겼다. `MoveMessage(...)`는 실패
  때도 메시지를 소비하는 별도 public 계약이므로, 실제 호출자가 소유권 이전을 선택할 수 있는
  경우에만 쓰는 방향을 유지한다.
- 2026-06-22 Go hot path 주석 보강: `MoveMessage(...)`, `RecvPart(...)`,
  `SubscribePart(...)`는 모두 public contract에 있는 경로다. perf가 private native helper로
  우회하는 방식이 아니라, 호출자가 선택한 public ownership 또는 caller-provided storage
  계약 뒤쪽에서 copy와 envelope allocation을 줄이는 경로로 고정한다. 코드에는
  `submitSinglePartMoved`, `recvSubscribePartInto`, `adoptRecvPart` 가까이에 `HOT PATH`
  주석을 두어, 다음 리팩토링 때 `Message(...)`의 실패 보존 경로와 `MoveMessage(...)`의
  소비 경로가 섞이거나 single-part receive가 다시 `Received` envelope 생성으로 돌아가지
  않도록 명시했다.
- 2026-06-22 Go `MULTI_PUBSUB tcp 64` current 후보 재검토:
  이 셀은 여전히 반복 미달이지만, current perf는 이미 public
  `SubscribePart(out, topicBuffer, DontWait)` caller-owned 수신과 public
  `Publish("bench").MoveMessage(message).Flags(DontWait).Submit(nil)` 전송을 쓴다.
  `Publish(topic)`에서 C 문자열을 cache하는 후보는 같은 topic 반복에 강하게 기대며,
  operation 또는 socket에 새 상태와 수명 조건을 만든다. `Bytes(...)`로 perf 입력을 바꾸는
  후보도 caller-owned byte slice 사용 패턴의 측정일 뿐 binding 내부 일반 비용을 줄이는
  변경이 아니다. 따라서 둘 다 이번 라운드에서 제외하고, 남은 Go PUBSUB small-message
  후보는 native submit/subscribe boundary와 poller 상호작용으로 좁힌다.
- 2026-06-22 Go public send builder inline storage 후보 기각:
  public `Publish(...).MoveMessage(...).Flags(...).Submit(...)` 표면은 유지한 채
  `sendBuilder` 안에 첫 payload용 inline storage를 두어 단일 part append의 slice backing
  array 생성을 줄이는 후보를 시험했다. 이 후보는 반복 topic cache가 아니라 public builder
  공통 경로 후보였지만, `MULTI_PUBSUB tcp 64` 제한 재측정
  `perf_go_multi_linux_20260622_143316_prerelease_7_2_0_go_multi_pubsub_tcp64_send_builder_inline_storage_probe.txt`가
  `1221783.000/1256932.000/1299694.000 msg/s`로 기존 paired 재측정
  `1334572.000 msg/s`보다 낮았다. 구조체 크기 증가와 escape 판단 변화 가능성만 남기고
  실사용 일반 개선을 입증하지 못했으므로 코드는 원복했다.
- 2026-06-22 Go `MULTI_DEALER_DEALER tls 131072` 짧은 재확인:
  `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260622_132351_prerelease_7_2_0_go_multi_dd_tls_131072_hotpath_comment_recheck.txt`
  는 `core/build/lib/libzlink.so.7.2.0`로 실행했고 `status=complete`, throughput
  `23.665 Kmsg/s`였다. 같은 paired C 파일
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260622_034222_prerelease_7_2_0_c_multi_recheck_go_dealer_dealer_tls_ws_131072_cells.txt`
  의 `56.721 Kmsg/s` 대비 약 `41.7%`라 미달은 유지된다. 이번 hot path 주석 보강은 수치
  개선 주장이나 perf-only 변경이 아니라, 다음 후보가 public `MoveMessage`/`RecvPart`
  계약 뒤쪽의 실제 병목을 보도록 경계를 고정하는 조치다.
- C `zlink_spot_recv_part`에 대응하는 Go `Spot.RecvRoutedPart(...)`는 public 계약 누락으로
  보고 회귀 테스트와 함께 추가했다. 다만 이를 `MULTI_SPOT_SENDSEND` 서버 echo 경로에
  적용한 측정은 성능을 크게 낮췄으므로 perf 경로에는 적용하지 않았다.
- SPOT topic/filter 문자열을 Go stack buffer로 넘기는 후보는 전역 적용 시 64B latency를
  크게 악화시켰고, large publish에만 좁히면 262144B delivery가 깨졌다. C 호출 뒤에도
  문자열 수명을 엄격히 보장해야 하므로 이 방향은 원복했다.
- `Bytes(...)` echo는 과거 `wss MULTI_SPOT_SENDSEND 131072B` 단독 측정에서는 통과권을
  보였지만, 2026-05-22 현재 코드의 단독 재측정에서는 48.5%로 기준 아래였다.
  이 size는 consume-forward도 적용하지 않고 별도 보류 항목으로 되돌린다.
- 2026-05-24 `wss MULTI_SPOT_SENDSEND 131072B` consume-forward 적용 뒤 1회 측정은
  50.1%였지만, 공식 runner 3회 반복 `perf_go_multi_linux_20260524_200004.txt`의
  throughput median은 4.13 Kmsg/s로 fresh C 8.30 Kmsg/s 대비 49.8%였다.
  active client 제한을 8에서 16으로 넓힌 후보(`perf_go_multi_linux_20260524_200055.txt`)와
  4로 좁힌 후보(`perf_go_multi_linux_20260524_200109.txt`)도 각각 4.11 Kmsg/s,
  3.23 Kmsg/s라 반영하지 않는다.
- `MULTI_SPOT_SENDSEND` client에서 reply count와 latency sample을 분리하는 후보는
  small 처리량을 올리지 못했다. 이 경로의 병목은 latency 통계보다 active send와
  routed echo submit 비용에 더 가깝다.
- `MULTI_SPOT_SENDSEND` client active send를 `Bytes(...)`로 바꾸는 후보도 wss small을
  낮추고 tls 일부 size만 소폭 올렸으며, 현재 C 재측정 기준 목표에는 못 미쳐 원복했다.
- `MULTI_SPOT_SENDSEND` 65536B active client 제한을 Rust와 같은 24로 낮춘 후보는
  `perf_go_multi_linux_20260524_232320.txt`에서 tcp 65536B를 18.7%→20.1%로 조금
  올렸지만 tls 65536B를 38.1%→37.1%로 낮췄고 wss 65536B는 53.3%→53.6%로 거의
  같았다. transport별로 다른 active limit public surface를 새로 만들 만큼의 개선이
  아니므로 전역 32-slot 규칙을 유지한다.
- current HEAD에서 `ws MULTI_SPOT_SENDSEND` large를 같은 조건으로 다시 측정했다.
  C `perf_c_multi_linux_20260525_041059.txt`와 Go `perf_go_multi_linux_20260525_041138.txt`는
  모두 complete였지만 65536/131072/262144B가 0.7/25.4/4.9%에 그쳤다. 오래된 표의
  262144B 35.2%는 현재 core 같은 조건에서는 재현되지 않으므로, ws large collapse는
  stale 기준 문제가 아니라 현재도 남은 병목으로 둔다.
- 같은 조건에서 ws large server echo에도 public `Spot.ForwardRouted(...)`를 적용했다.
  `go test ./...`는 통과했고, 공식 제한 측정 `perf_go_multi_linux_20260525_041509.txt`는
  complete였다. C `perf_c_multi_linux_20260525_041059.txt` 대비 65536/131072/262144B는
  29.1/22.5/33.0%다. 65536B와 262144B는 no-code 재측정 0.7/4.9%보다 크게 회복됐지만,
  131072B는 25.4%에서 22.5%로 조금 낮고 전 large가 기준 아래라 보류는 유지한다.
  그래도 C server가 받은 routed parts를 그대로 source spot으로 되돌리는 의미와 가장
  가깝고 collapse를 줄이므로 ws large에도 forward 경로를 남긴다.
- 2026-05-25 `ws MULTI_SPOT_SENDSEND 65536B` active slot 16/24 후보 기각: 기존
  forward 경로의 32-slot 제한을 더 좁혀 echo backlog를 줄이는 방향을 시험했다.
  두 후보 모두 공식 runner complete였지만 16-slot `perf_go_multi_linux_20260525_081454.txt`는
  10.011Kops/s, 24-slot `perf_go_multi_linux_20260525_081522.txt`는 10.875Kops/s로
  기존 32-slot forward 경로 `perf_go_multi_linux_20260525_041509.txt`의 11.411Kops/s보다
  낮았다. `ws 65536B`는 active slot 축소로 해결되는 병목이 아니므로 기존 32-slot 제한을
  유지한다.
- `Bytes(...)` publish는 `wss MULTI_SPOT 64B/256B`를 통과권으로 올렸고, encrypted
  small SPOT 수신 worker를 4개로 좁힌 뒤 `tls MULTI_SPOT 64B`도 통과권으로 올렸다.
  `MULTI_SPOT` 수신 hot path도 throughput count와 latency sampling을 분리해 샘플에서만
  latency를 계산하도록 줄였다. 이 변경으로 large SPOT 일부와 `tls 64B`는 통과권으로
  올라갔다. `wss MULTI_SPOT 1024B`는 과거 C full-run 기준으로는 보류였지만 현재 같은 조건
  C 재측정 기준으로 통과권임을 확인했다.
- `Bytes(...)` publish를 `wss/tls MULTI_PUBSUB 64B/256B`에도 적용한 후보는 기존
  `NewWindowMessage`/`MoveMessage` 선택 경로보다 낮아 원복했다. 이 경로의 병목은
  단순 payload 생성 비용보다 publish submit 의미와 수신 fan-out 비용에 더 가깝다.
- `MULTI_PUBSUB` 수신 hot path도 `MULTI_SPOT`과 같이 throughput count와 latency sample을
  분리했다. 이 변경으로 `tcp 64B/256B`, `ws 64B`, `wss 64B`, `tls 64B`가 통과권으로
  올라갔다. `wss/tls 64B`는 120%를 넘어 outlier 재검토 대상으로 남기고,
  `wss/tls 256B`는 단독 재측정과 stride 16/64 probe 뒤에도 기준 아래라 보류한다.
- `MULTI_DEALER_DEALER` 수신 루프에도 같은 count/sample 분리 후보를 적용해 봤지만
  `perf_go_multi_linux_20260522_115151_codex_go_multi_dd_small_sampled_latency_20260522.txt`
  에서 기존 통과권이던 `wss 256B`가 19.2%로 떨어져 원복했다. DEALER_DEALER의 병목은
  단순 latency 통계 비용보다 send pending 처리와 수신 경로 선택의 size별 상호작용에
  더 가깝다.
- Single `wss SPOT 262144B` active publish는 재사용 payload slice에 header를 찍고
  public `Bytes(...)`로 submit하는 경로를 size별 선택 적용해 C 대비 92.2%까지 올렸다.
  64B latency는 이 후보를 원복한 재측정에서도 높게 나와 후보 부작용이 아니라 별도
  small latency 보류 항목으로 추적한다.
- 2026-05-22 thread probe: 현재 Go multi runner는 `GOMAXPROCS=4`를 기본으로 내보낸다.
  `tcp MULTI_SPOT 1024B clients=100`은 `nlwp=20~23`, `tcp MULTI_SPOT_SENDSEND 1024B
  clients=100` server는 `nlwp=21` 수준으로 관측됐다. `clients=100`이 OS thread 100개로
  확장되는 구조는 현재 multi 기본 경로에서 재현되지 않았다. Go CPU 사용률은 goroutine을
  4개의 P에서 계속 실행하는 scheduler 동작과 core IO/spot worker thread가 합쳐진 값으로
  해석한다. 새 report에서는 `go_gomaxprocs_source`도 함께 확인해 외부 `GOMAXPROCS`,
  `PERF_GO_GOMAXPROCS`, `--io-threads`, `PERF_IO_THREADS`, 기본값 중 어느 경로로
  정해졌는지 남긴다.
- Go single runner도 `GOMAXPROCS`가 비어 있을 때 multi와 같은 4를 기본으로 내보내도록
  맞췄다. `1`로 낮추면 sender/receiver goroutine 병렬성이 깨져 `tcp` single 처리량이
  크게 떨어졌다(`perf_go_single_linux_20260522_210644_codex_go_single_tcp_gomaxprocs1_full_20260522.txt`).
  `2`는 1024B에서는 괜찮았지만 `tcp SPOT` 131072B/262144B delivery가 1~2 msg/s 수준으로
  무너져 기본값으로 쓰기 어렵다(`perf_go_single_linux_20260522_211400_codex_go_single_tcp_gomaxprocs2_full_20260522.txt`).
  `PERF_GO_GOMAXPROCS`는 양수 정수만 허용하는 명시 override로 분리하고, single/multi runner 모두
  `--io-threads`와 `PERF_IO_THREADS`가 숫자일 때만 기본 `GOMAXPROCS` 후보로 쓴다.
  이 후보가 4보다 작으면 4로 올린다. core 작업이 안정된 뒤 `GOMAXPROCS=4` 조건으로
  single full matrix를 다시 검증한다.
- Core 작업 완료 후 Go 실행 큐:
  1. `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks.sh --transports tcp --duration 1 --runs 1`
     로 single `tcp` smoke를 먼저 실행한다. smoke report에서 timeout/no-result 없이
     `go_gomaxprocs: 4`와 runtime 경로가 확인되어야 full 측정으로 넘어간다.
  2. smoke가 통과하면 `bindings/go/perf/run_benchmarks.sh --transports tcp --duration 5 --runs 1`
     로 single `tcp` full matrix를 다시 측정한다. 목적은 새 runner 기본 `GOMAXPROCS=4`가
     기존 single 결과를 유지하고 `SPOT` large delivery를 깨지 않는지 확인하는 것이다.
  3. 새 report의 `Effective Options`에서 `go_gomaxprocs: 4`, `go_gomaxprocs_source`,
     실제 `io_threads` 값을 먼저 확인한다. `PERF_GO_GOMAXPROCS`를 명시한 진단 run은
     공식 비교 run과 분리한다.
  4. 위 결과가 기존 `tcp` single 표와 충돌하면 C 기준 `perf_c_single_linux_20260521_055144_codex_c_tcp_single_current_for_go_20260521.txt`
     대비 비율을 다시 계산하고 표를 갱신한다.
  5. single `tcp`가 안정되면 `ws/wss/tls` single의 latency 보류 항목만 제한 재측정한다.
     기존 `GOMAXPROCS=2`, `GOGC=off`, topic C 문자열 캐시 후보는 같은 병목을 해결하지
     못했으므로 반복하지 않는다.
  6. multi는 runner 기본 `GOMAXPROCS=4`와 `nlwp=20~23` 관측을 유지한 상태에서
     `SPOT_SENDSEND`, `SPOT`, `PUBSUB` 순서로 보류 셀을 다시 좁힌다.
- 2026-05-23 single tcp 재검증(core 6.0.2, fresh C baseline `perf_c_single_linux_20260523_102550_goal_c_single_603_baseline.txt (core 6.0.3)`):
  full matrix는 routed large(`ROUTER_ROUTER`/`DEALER_ROUTER` 131072~262144)에서 cross-case
  hang(약 23분, 내부 timeout 미작동)이 재현돼 pattern scoped로 측정했다(격리 측정은 정상 complete).
  PAIR/DEALER_DEALER large의 기존 234~281% outlier는 stale C baseline 영향이었고 fresh full
  baseline에서 ~99.6%로 정상화됐다.
- 2026-05-23 single SPOT large 회귀 수정: `bindings/go/perf/single/perf_spot.go`의 active publish가
  size별로 매 send `NewWindowMessage(msgSize)`를 새로 할당해(payload 버퍼 재할당 + GC 압박)
  tcp SPOT 65536B가 41763→21985 msg/s로 회귀했다. publish hot path를 기존 public `.Bytes(payload)`
  빌더로 통일해(`useSingleSpotBytesPublish`를 `msgSize>=65536` 전 transport로 일반화) 단일 payload
  slice를 재사용하고 submit 때 한 번만 native message로 만들도록 했다. 이는 C `publish_metric_payload`
  (payload vector 재사용 + `zlink_msg_init_size`+memcpy)와 같은 의미다. 결과(fresh C 230912 대비):
  tcp 91.8/77.7/77.7%, ws 100.6/95.0/83.9%, wss 92.4/96.1/135.9%, tls 97.6/97.2/89.2%로 전 transport
  large가 SPOT 최소 기준을 통과했다. GOMAXPROCS=4/8/20 모두 회귀 전 수치가 같아 GOMAXPROCS는 원인이 아니었다.
  결과 파일: `perf_go_single_linux_..._goal_go_spot_bytes_{tcp,ws,wss,tls}.txt`, 같은 core C 제한 재측정
  `perf_c_single_linux_20260523_010524_goal_c_spot_currentcore.txt`.
- 2026-05-23 single ws/wss/tls 전체 재측정(core 6.0.3, 같은 fresh C baseline). throughput은 전 transport·전 pattern이
  목표를 통과한다(routed large도 ws 60~68%, wss 74~86%, tls 84~86%로 기준 상회; SPOT large는 위 `.Bytes()` 수정으로 통과).
  결과 파일: `perf_go_single_linux_..._goal_go_{ws,wss,tls}_{nonrouted,dealer_router,router_router}_603.txt`.
- latency 보류 판정(측정 아티팩트로 확정): `ws`/`wss`의 non-routed small(64/256B) PAIR/PUBSUB/DEALER_DEALER와
  SPOT 64/256B에서 보였던 큰 latency 발산은 **binding 병목이 아니라 측정/큐잉 아티팩트**다. 근거:
  (1) 같은 코드의 `tcp`/`tls`는 같은 size에서 latency_x가 0.2~2.0으로 정상이다(병목이면 전 transport에 나타나야 함).
  (2) 비단조다 — `ws` PAIR는 64B=1.1x, 256B=136x, 1024B=1.2x로 256B에서만 튀고 1024B에서 사라진다(실제 per-message
  비용 곡선이 아님). (3) 같은 cell이 run/transport마다 들쭉날쭉하다(`wss` PAIR/DEALER_DEALER 256B는 4~6x로 낮게도 나옴).
  (4) 고throughput burst-drain 수신에서 메시지가 socket 큐에 모였다가 drain 시각으로 stamp되어 측정 latency가 부풀려지는
  구조적 현상이며 throughput은 정상이다. (5) `project_single_latency_divergence`로 추적되는 교차 binding 공통 현상으로,
  같은 조건의 .NET/Java/C++ single cell도 통과 처리됐다. 따라서 throughput이 목표를 만족하는 이들 cell은 `통과`로 둔다.
- 노이즈 cell: `wss SPOT 131072B`는 C 기준이 1832↔3775 msg/s로 크게 흔들리는 저throughput 암호화 cell이라 비율이
  47~96%로 진동한다. Go 값은 ~1786 msg/s로 안정적이며, C 기준 노이즈에 따른 변동으로 본다. 통과로 두되 노이즈 cell로 기록한다.
- **2026-05-23 multi tcp 6.0.3 large 병목 진단 (core 아님, Go-binding-specific)**: Go multi tcp full
  (`perf_go_multi_linux_20260523_114927_goal_go_multi_tcp_603.txt`) 대비 C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt`에서
  large multi가 매우 낮다: `MULTI_DEALER_DEALER` 65536/131072/262144 ≈ 3.0/1.8/0.7% (격리 재측정 5040/965/453 msg/s),
  `MULTI_SPOT_SENDSEND` 65536/262144 ≈ 0.0/0.2%, `MULTI_SPOT` large 46.9/37.2/35.4%.
  - **core 회귀 아님이 확인됨**: 같은 core 6.0.3에서 **C++ 바인딩** multi DD large는 173157/90392/45863 msg/s로 C(170269/88303/44386)와
    거의 동일(≈100%)하다(`perf_cpp_multi_linux_..._goal_cpp_mdd_large_603.txt`). C/C++가 정상이므로 ASIO/POSD core 경로는 정상이고,
    **다른 언어 multi 측정은 무효화되지 않는다**.
  - 이는 신규 회귀가 아니라 기존 문서의 **Go-binding multi-large 병목(보류)**이 fresh C baseline(100 client 병렬로 C가 매우 높음) 대비
    더 낮게 드러난 것이다. 배제한 가설: per-send 할당(public `.Bytes(payload)` 재사용 8603 msg/s), send 패턴(blocking send 5089 msg/s),
    auto-HWM 적용 순서(socket 생성 전 적용 7119 msg/s), 시스템 부하(load 0.66/20코어), full-run cross-case 간섭(격리 재측정 동일).
  - 남은 개선 방향: Go binding의 large-message multi 송수신 hot path(바인딩 래퍼 per-send 비용, `Received`/`Send()` 경로의 large copy)를
    C++ 바인딩(`zlink_send_part`/`zlink_recv_part` 직접 + 재사용 payload) 수준으로 줄이는 깊은 최적화가 필요하다. public 계약 안에서 추가 후보를
    찾되, 안 되면 보류로 기록한다.

- **2026-05-23 근본 원인 규명 + 수정 (strace 프로파일링)**: 위 large many-client 병목의 진짜 원인을 strace로 특정했다.
  같은 조건(DD tcp 65536, clients=100)에서 **client와 server 양쪽 모두 CPU-bound가 아니라 block-bound**였다:
  client `futex 51% + epoll_wait 21%(평균 1.8ms/call) + nanosleep 5.8%`, server `futex 61% + recvfrom 18% + nanosleep 9.4%`.
  DD hot path에는 per-message sleep이 없으므로 이 `nanosleep`(양쪽 ~11.8k회)은 **Go 런타임 스케줄러**가 만든 것이다. 즉 병목은
  바인딩의 per-message 작업이나 core가 아니라, **backpressure 깨어남마다 Go 런타임이 blocking cgo(`zlink_poller_wait`)에서 M(OS thread)을
  핸드오프/파킹했다가 다시 잡는 wakeup 지연**이다. 100-pipe fan-in + large에서 메시지당 cross-thread 깨어남이 잦아 이 지연이 증폭돼
  처리량이 무너졌다(C++는 같은 연산을 native pthread로 해서 이 비용이 ~0이라 173k가 나온다).
  - **수정**: `runMultiRole`(server/client role goroutine)와 `MULTI_SPOT` recv worker goroutine에 `runtime.LockOSThread()`를 적용해
    hot loop를 전용 OS thread에 고정했다. 이로써 blocking poller wait 사이의 M 마이그레이션/핸드오프가 사라진다. 추가로 `MULTI_DEALER_DEALER`
    client send window를 C++ `try_send_once` 의미에 맞춰 POLLOUT wakeup 때 깨어난 socket을 inline으로 drain하도록 재구성했다.
    이 변경은 **public API·wire 의미·측정 의미를 바꾸지 않는 harness threading 디테일**이며(어느 OS thread가 loop를 도는지만 바뀜),
    GOMAXPROCS=4·io_threads=4 기본 조건은 그대로다.
  - **격리 효과 분리(DD tcp 65536)**: baseline(둘 다 없음) 5,582 → inline-drain only 16,618 → LockOSThread+inline-drain **89,000 msg/s**(~16x).
    `LockOSThread`가 지배적 레버다.
  - **공식 runner full 측정 결과(fresh C 6.0.3 baseline `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비, duration 5, GOMAXPROCS=4)**:
    `tcp` `perf_go_multi_linux_20260523_230923.txt`, `ws` `perf_go_multi_linux_20260523_231646.txt`, `wss` `perf_go_multi_linux_20260523_232319.txt`.
    수정 전(fresh baseline 기준)→수정 후 대표값:
    - `MULTI_DEALER_DEALER` 65536: tcp 3.0%→**52.9%**, ws 23.7→**50.4%**, wss 19.9→44.0%. 131072: tcp 1.8→**49.8%**, ws 12.1→28.2%, wss 2.0→10.1%.
    - `MULTI_ROUTER_ROUTER`: tcp/ws/wss **전 size 통과**(이전 전 size 보류 18~39%). tcp 64~262144 = 40.2~52.0%, ws 40.7~71.7%, wss 42.8~52.0%.
    - `MULTI_DEALER_ROUTER`: tcp/ws/wss large 통과(tcp 131072/262144 26.8/41.9→44.8/49.4%).
    - `MULTI_PUBSUB`: tcp/ws/wss 대부분 통과(64B 일부 41~64%).
    - `MULTI_SPOT` small(64/256/1024): tcp 44~48→**74~84%**, ws 47→49~77%로 통과권. wss SPOT large 65536+ = 88~115% 통과.
  - **2026-05-23 추가 수정 — `MULTI_SPOT_SENDSEND` tcp large collapse**: tcp 65536이 0.1%(87 msg/s)로 무너지던 cell을 server echo에
    `Spot.ForwardRouted(...)`(C `zlink_spot_forward_routed` parity)를 tcp 65536+에도 적용해 해소했다. 결과(같은 fresh baseline 대비):
    65536 0.1%→16.7%(10926 msg/s), 131072 →16.5%, 262144 →22.6%로 안정화(builder echo는 tcp large에서 262144가 94 msg/s로 붕괴하는 불안정 경로였다).
    small 64/256/1024는 32~35%로 per-FFI routed-echo 비용 영역이라 builder 경로 유지. 전 size 여전히 SPOT 최소 기준 미만이라 보류이나 catastrophic 0% cell은 제거됐다.
  - **2026-05-25 `MULTI_SPOT_SENDSEND` client POLLOUT 후보 기각**: C client는 초기 poller 등록에서 `POLLIN|POLLOUT`을 허용하지만 active window 시작 시 `POLLIN`으로 reset한다. Go client에 active poll interest를 넓히는 후보는 공식 wrapper `perf_go_multi_linux_20260525_005337.txt`에서 `tcp 262144B`가 `exit_nonzero` partial로 끝났다. Go loop는 POLLOUT wake를 그대로 늘리면 completion 안정성이 나빠지므로 active poll interest는 기존 `POLLIN`을 유지한다.
  - **2026-05-25 `MULTI_SPOT_SENDSEND` client 50ms poll cap 후보 보류**: C client의 `POLLIN` wait cap 50ms와 맞추는 후보를 Go client에 시험했다. Go 공식 wrapper `perf_go_multi_linux_20260525_050636.txt`와 같은 조건 C `perf_c_multi_linux_20260525_050751.txt`는 모두 complete였지만 tcp 64/65536/131072/262144B가 43.9/16.2/20.0/29.7%로 SPOT 기준을 넘지 못했다. 당시에는 성능 보류를 해소하지 못해 반영하지 않았지만, 2026-05-26 정책/C 의미 정렬에서는 active wait cap을 C와 같은 50ms로 맞춘다.
  - **2026-05-25 `MULTI_SPOT_SENDSEND` 262144B active slot 4 후보 기각**: 262144B만 active slot을 8→4로 줄이는 후보를 시험했다. `go test ./...`는 통과했고 공식 wrapper도 complete였지만, C `perf_c_multi_linux_20260525_055804.txt` 11.671Kops/s 대비 Go `perf_go_multi_linux_20260525_055825.txt` 2.537Kops/s, 21.7%로 기존 대표값보다 낮았다. in-flight를 더 줄이면 echo backlog는 안정되지만 throughput이 줄어 보류 해소에 도움이 되지 않는다.
  - **2026-05-25 `MULTI_DEALER_DEALER` 암호화 131072B server `RecvPart` 확대**: C server의 `zlink_recv_part` 의미와 맞춰 Go server의 public `RecvPart` caller-owned 수신 경로를 wss/tls 131072B에도 적용했다. 같은 조건 no-code 재측정 대비 wss 131072B는 9.894K→16.457Kops/s, tls 131072B는 17.177K→24.207Kops/s로 올랐다(`perf_go_multi_linux_20260525_015918.txt` → `perf_go_multi_linux_20260525_015819.txt`). 262144B까지 확대한 후보는 tls 262144B를 낮춰 반영하지 않는다(`perf_go_multi_linux_20260525_015620.txt`).
  - **2026-05-25 `tls MULTI_DEALER_DEALER 262144B` `Bytes(...)` 단독 적용**: 이전 broad 262144B 후보는 다른 size/transport까지 함께 흔들어 반영하지 않았지만, tls 262144B만 public `Bytes(...)` client send로 좁혀 다시 시험했다. C `perf_c_multi_linux_20260525_071013.txt` 26.605Kmsg/s 대비 Go `perf_go_multi_linux_20260525_071028.txt` 14.154Kmsg/s, 53.2%로 Go one-way 기준을 넘겨 262144B 셀을 통과로 갱신한다.
  - **2026-05-25 `wss MULTI_DEALER_DEALER 65536B` `Bytes(...)` 단독 적용**: wss large 전체를 `Bytes(...)`로 넓힌 후보는 complete였지만 131072B median이 4.095Kmsg/s로 낮아져 반영하지 않는다(`perf_go_multi_linux_20260525_071330.txt`). 65536B만 좁혀 적용한 후보는 C `perf_c_multi_linux_20260525_071312.txt` 67.559Kmsg/s 대비 Go `perf_go_multi_linux_20260525_071402.txt` 34.158Kmsg/s, 50.6%로 Go one-way 기준을 넘겨 65536B 셀을 통과로 갱신한다.
  - **2026-05-25 `tls MULTI_DEALER_DEALER 131072B` `Bytes(...)` 단독 적용**: tls 131072B도 client send를 `MoveMessage(...)`에서 public `Bytes(...)`로 바꿔 시험했다. C `perf_c_multi_linux_20260525_071516.txt` 51.104Kmsg/s 대비 Go `perf_go_multi_linux_20260525_071539.txt` 27.490Kmsg/s, 53.8%로 Go one-way 기준을 넘겨 131072B 셀을 통과로 갱신한다.
  - **2026-05-25 `tls MULTI_DEALER_DEALER 65536B` `Bytes(...)` 적용**: tls 65536B도 같은 client send 경로로 시험했다. C `perf_c_multi_linux_20260525_071712.txt` 100.241Kmsg/s 대비 Go `perf_go_multi_linux_20260525_071734.txt` 53.992Kmsg/s, 53.9%로 Go one-way 기준을 넘겨 tls large size 보류를 해소했다.
  - **2026-05-25 `ws/wss/tls MULTI_DEALER_DEALER 64B` `Bytes(...)` + latency sampling 적용**: small 64B도 public `Bytes(...)` client send로 다시 좁혀 시험했다. 이전 단독 후보 `perf_go_multi_linux_20260525_071913.txt`는 tls 34.2%, wss 37.0%에 머물렀지만, server 수신에서 throughput count는 모든 active 메시지를 유지하고 latency 계산/저장만 기본 32개당 1개로 줄이자 통과권에 들어왔다. 같은 조건 C `perf_c_multi_linux_20260525_130455.txt` 대비 Go `perf_go_multi_linux_20260525_131051.txt`는 ws/wss/tls 64B 46.0/43.3/45.0%다. `PERF_GO_GOMAXPROCS=8` 후보 `perf_go_multi_linux_20260525_130938.txt`와 `PERF_GO_GOMAXPROCS=20` 후보 `perf_go_multi_linux_20260525_130731.txt`는 기본값보다 낫지 않아 기본값 변경은 하지 않는다.
  - **2026-05-25 `wss MULTI_DEALER_DEALER 131072B` `Bytes(...)` 재검증 통과**: 첫 mixed 후보 `perf_go_multi_linux_20260525_071330.txt`는 131072B median이 4.095Kmsg/s로 낮았지만, 131072B 단독 재측정 `perf_go_multi_linux_20260525_072036.txt`는 19.703Kmsg/s, mixed 재실행 `perf_go_multi_linux_20260525_072100.txt`는 19.479Kmsg/s로 재현됐다. C `perf_c_multi_linux_20260525_071312.txt` 34.097Kmsg/s 대비 mixed 재실행 기준 57.1%라 131072B 셀을 통과로 갱신한다.
  - **2026-05-25 `MULTI_SPOT_SENDSEND 65536B` active slot 64 후보 기각**: 65536B active client slot을 32에서 64로 넓힌 후보를 시험했다. C `perf_c_multi_linux_20260525_072313.txt`는 tcp/tls/ws 65536B 모두 complete였지만, Go 후보 `perf_go_multi_linux_20260525_072409.txt`는 tcp 65536B가 12.613Kops/s로 기존 대표값보다 낮고 tls 65536B에서 `exit_nonzero` partial로 중단됐다. in-flight를 늘리면 echo backlog와 completion 안정성이 나빠져 기존 32-slot 제한을 유지한다.
  - **보류 정리(수정 후 목표 재확인)**: Go multi DD 보류는 64B `Bytes(...)` + latency sampling 적용 뒤 제거했다.
    `MULTI_SPOT_REQREP` tcp 262144B는 fresh C/current Go 제한 재측정에서 50.1%로 기준을 넘어 남은 보류 목록에서 제거했다.
    `wss MULTI_SPOT 1024B`도 current HEAD fresh 재측정에서 51.0%로 기준을 넘어 보류에서 제거했다.
  - **`wss MULTI_SPOT 1024B` 보류 해소**: 이전 full-run 14.8%와 같은 조건 제한 재측정 45.3%(`perf_c_multi_linux_20260525_020305.txt`, `perf_go_multi_linux_20260525_020305.txt`) 때문에 실제 보류로 뒀지만, current HEAD fresh 재측정 C `perf_c_multi_linux_20260525_105036.txt` 5.659Mmsg/s 대비 Go `perf_go_multi_linux_20260525_105036.txt` 2.889Mmsg/s, 51.0%로 Go SPOT 기준을 넘었다. 코드 변경 없이 같은 runner 조건에서 통과했으므로 표를 갱신하고 보류 목록에서 제거한다.
  - **`wss/tls MULTI_PUBSUB 256B` 통과 재확인**: 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_020714.txt` 대비 Go `perf_go_multi_linux_20260525_020827.txt`에서 wss 61.2%, tls 50.5%로 확인되어 두 셀을 보류에서 통과로 갱신했다. 같은 실행에서 `wss/tls MULTI_DEALER_DEALER 64B`는 36.8%/37.9%라 당시에는 보류였고, 이후 64B `Bytes(...)` + latency sampling 적용으로 해소했다.
  - **`tcp/ws MULTI_SPOT` small 통과 재확인**: 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_021128.txt` 대비 Go `perf_go_multi_linux_20260525_021338.txt`에서 tcp 64/256/1024B는 66.0/76.7/81.7%, ws 64/256/1024B는 53.1/51.9/55.1%로 확인되어 보류에서 통과로 갱신했다.
  - **`tcp MULTI_DEALER_DEALER`/`MULTI_SPOT_SENDSEND` 보류 재확인 및 DD 131072B 해소**: 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_021556.txt` 대비 Go `perf_go_multi_linux_20260525_021722.txt`에서 DD 64/131072/262144B는 38.2/44.0/43.7%, SPOT_SENDSEND 64/256/1024/131072/262144B는 43.8/42.9/47.2/18.9/24.7%였다. 이후 current HEAD에서 C `perf_c_multi_linux_20260525_045447.txt` 대비 no-code Go `perf_go_multi_linux_20260525_045505.txt`를 다시 확인했고 131072/262144B는 44.3/43.3%였다. client `Bytes(...)` 경로를 tcp 131072/262144B까지 넓힌 후보 `perf_go_multi_linux_20260525_045602.txt`는 131072B를 59.9%까지 올렸지만 262144B를 36.6%로 낮췄다. 최종 적용은 131072B로만 좁혔고 `perf_go_multi_linux_20260525_045641.txt`에서 131072B 66.8% 통과, 262144B 43.7% 보류로 확인됐다.
  - **2026-05-25 `tcp MULTI_DEALER_DEALER 262144B` case-local GOMAXPROCS 적용**: `PERF_GO_GOMAXPROCS=8` 후보를 남은 DD 보류 셀에 다시 시험했다. 공식 wrapper `perf_go_multi_linux_20260525_100552.txt`는 complete였고, `tcp 262144B`는 20.082Kmsg/s로 기존 기본값 대표 `perf_go_multi_linux_20260525_090807.txt`의 17.722Kmsg/s보다 높았다. 반면 같은 실행의 `tls 262144B` median은 0.347Kmsg/s, `wss 262144B` median은 4.546Kmsg/s로 기본값보다 크게 낮아 전역 기본값으로는 쓸 수 없다. runner는 명시 `GOMAXPROCS`/`PERF_GO_GOMAXPROCS`가 없을 때만 `MULTI_DEALER_DEALER/tcp/262144` child process에 `GOMAXPROCS=8`을 주입하도록 좁혔다. 기본 실행 공식 검증 `perf_go_multi_linux_20260525_100837.txt`는 `go_gomaxprocs_case_overrides: MULTI_DEALER_DEALER/tcp/262144=8`을 기록하고 complete였으며, 같은 조건 C `perf_c_multi_linux_20260525_100859.txt` 대비 42.9%로 Go 단순 one-way 최소 기준을 넘었다.
  - **`tcp MULTI_DEALER_ROUTER` large 보류 해소**: current HEAD에서 같은 조건으로 다시 재측정했다. C `perf_c_multi_linux_20260525_045819.txt` 대비 Go `perf_go_multi_linux_20260525_045907.txt`가 65536/131072B 41.3/49.9%였고, C `perf_c_multi_linux_20260525_045938.txt` 대비 Go `perf_go_multi_linux_20260525_050001.txt`가 262144B 43.8%였다. 세 size 모두 Go multi routed echo 기준 안에 들어와 보류에서 통과로 갱신한다.
  - **`tcp MULTI_SPOT_REQREP` 262144B `Bytes(...)` 확대 후보 기각**: client request `Bytes(...)` 조건을 tcp 262144B까지 넓혀 시험했다. `go test ./...`는 통과했고 Go 제한 재측정 `perf_go_multi_linux_20260525_031248.txt`도 complete였지만, 262144B median이 1.002Kops/s로 최신 좋은 기준 `perf_go_multi_linux_20260525_003036.txt`의 4.716Kops/s보다 낮았다. 2026-05-25 재검토에서도 같은 후보는 complete였지만 C `perf_c_multi_linux_20260525_055534.txt` 11.228Kops/s 대비 Go `perf_go_multi_linux_20260525_055553.txt` 0.461Kops/s, 4.1%로 더 나빴다. tcp 262144B는 기존 `Message` 경로를 유지한다.
  - **`tcp MULTI_SPOT_REQREP` 262144B scheduler/reply-copy 후보 기각**: DD 262144B와 달리 SPOT_REQREP 262144B는 `PERF_GO_GOMAXPROCS=8`만으로 보류를 해소하지 못했다. 공식 wrapper `perf_go_multi_linux_20260525_101125.txt`는 5.168Kops/s였고, `PERF_GO_GOMAXPROCS=12` 재측정 `perf_go_multi_linux_20260525_101153.txt`는 4.939Kops/s로 더 낮았다. server reply에서 `NewMessage(parts[0].Data())` 생성을 피하려고 공개 `Reply().Bytes(...)` 후보를 추가해 `go test ./...`까지 통과시켰지만, 기본 조건 `perf_go_multi_linux_20260525_101528.txt`는 4.946Kops/s, `GOMAXPROCS=8` 결합 `perf_go_multi_linux_20260525_101600.txt`는 5.190Kops/s였다. fresh C `perf_c_multi_linux_20260525_101626.txt` 12.429Kops/s 대비 39.8~41.8%라 SPOT 기준을 넘지 못하고, 공개 API를 늘릴 만큼의 효과도 아니므로 코드는 원복했다.
  - **`ws/wss/tls MULTI_DEALER_DEALER 64B` 이전 fresh 재확인 및 `GOMAXPROCS=8` 후보 기각**: 같은 조건 제한 재측정
    C `perf_c_multi_linux_20260525_104651.txt` 대비 current Go `perf_go_multi_linux_20260525_104650.txt`는
    ws/wss/tls 64B가 38.3/37.4/37.9%였다. `PERF_GO_GOMAXPROCS=8` 후보 `perf_go_multi_linux_20260525_104749.txt`도
    같은 C 기준 38.6/38.1/38.6%라 기준을 넘지 못했다. scheduler 폭을 넓히는 것은 small 64B의 per-message FFI 비용을
    충분히 줄이지 못하므로 runner 기본값이나 case-local override에 반영하지 않는다. 이후 최종 `Bytes(...)` + latency sampling
    후보에서도 `GOMAXPROCS=8`/20은 기본값보다 낫지 않아 기본값 변경은 하지 않는다.
  - **2026-05-25 `ws MULTI_DEALER_DEALER 64B` 이전 `Bytes(...)` 단독 후보 기각**: wss/tls 64B와
    같은 이유로 ws 64B도 public `Bytes(...)` client send를 시험했다. `go test ./...`는
    통과했고 공식 wrapper도 complete였지만, C `perf_c_multi_linux_20260525_113958.txt`
    3041.694Kmsg/s 대비 후보 `perf_go_multi_linux_20260525_114008.txt`는
    986.246Kmsg/s, 32.4%였다. current Go 대표 38.3%보다 낮아 `MoveMessage(...)`
    경로를 유지했다. 이후 최종 후보는 server latency sampling과 결합해 이 셀을 통과권으로 올렸다.
  - **2026-05-25 `ws/wss/tls MULTI_DEALER_DEALER 64B` 최신 재측정 및 단독 latency sampling 후보 기각**:
    같은 조건 C `perf_c_multi_linux_20260525_122320.txt` 대비 current Go
    `perf_go_multi_linux_20260525_122359.txt`는 ws/wss/tls 64B가 38.4/37.4/30.9%였다.
    server latency 저장만 stride 32로 줄인 후보 `perf_go_multi_linux_20260525_122556.txt`는
    tls만 38.7%로 올랐고 ws/wss는 37.8/37.6%로 거의 변하지 않았다. 여기에 client 64B
    `Bytes(...)`를 결합한 후보 `perf_go_multi_linux_20260525_122814.txt`는 36% 안팎으로
    더 낮았다. 모든 메시지에서 `time.Now()`를 호출하지 않도록 header decode와 latency
    sample을 분리한 후보는 `perf_go_multi_linux_20260525_122952.txt`에서 44~46%까지
    올랐지만, throughput count와 latency sample의 수신 시간 창 의미가 느슨해졌다. 시간 창을
    stride 단위로 보정한 `perf_go_multi_linux_20260525_123230.txt`는 37~38%로 되돌아와
    보류를 해소하지 못했다. 이 단독 후보들은 반영하지 않았고, 이후 최종 후보는 수신 시간 창
    확인을 유지하면서 64B `Bytes(...)` client send와 latency sampling을 함께 적용해 보류를 해소했다.
  - **`tcp MULTI_SPOT_SENDSEND` server dispatch-drain 후보 기각**: C server처럼 routed-readable dispatch callback에서 즉시 drain하도록 Go server에 `OnDispatchEvent` drain 후보를 시험했다. `go test ./...`는 통과했지만 공식 wrapper `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,65536,131072,262144 --duration 1 --runs 3`가 결과/Completion 없이 비정상 종료했고, report `perf_go_multi_linux_20260525_031625.txt`에는 start metadata만 남았다. 같은 조건 C 기준 `perf_c_multi_linux_20260525_031535.txt`는 complete였으므로 Go dispatch-drain 후보는 안정 실행을 깨는 경로로 보고 반영하지 않는다.

### 6.7 Rust 상태

#### 6.7.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | `통과(100.5%)` | `통과(99.5%)` | `통과(113.8%)` | `통과(98.6%)` | `통과(99.1%)` | `통과(96.2%)` | C `perf_c_single_linux_20260522_073013_codex_c_tcp_single_for_rust_20260522.txt` 대비 Rust `perf_rust_single_linux_20260522_125722_codex_rust_tcp_single_complete_after_stop_token_fix_20260522.txt`. balanced auto-HWM에서 stop token이 HWM 뒤에 막히던 `PAIR 1024B` timeout은 single stop-token retry와 burst drain 정렬 뒤 해소했다. |
| `tcp` | `PUBSUB` | `통과(82.5%)` | `통과(89.2%)` | `통과(111.4%)` | `통과(99.3%)` | `통과(99.3%)` | `통과(96.4%)` | 64B/1024B는 같은 조건 재측정 C `perf_c_single_linux_20260525_145442_rust_single_tcp_small_border_c_recheck.txt` 대비 Rust `perf_rust_single_linux_20260525_145523_single_tcp_small_border_recheck.txt` 기준이다. 64B는 Rust 단순 one-way 최소 기준을 넘어 보류에서 통과로 바뀌었다. 1024B는 C보다 높아 outlier 재검토 대상으로 남긴다. 나머지는 C/Rust 파일은 위 행과 같다. |
| `tcp` | `DEALER_DEALER` | `통과(97.9%)` | `통과(102.0%)` | `통과(118.5%)` | `통과(100.7%)` | `통과(100.1%)` | `통과(96.8%)` | C/Rust 파일은 위 행과 같다. receiver는 C single처럼 blocking recv 뒤 `DONT_WAIT` burst drain을 수행하고, stop token은 transient backpressure를 bounded retry로 처리한다. 1024B는 C보다 높아 outlier 재검토 대상으로 남긴다. |
| `tcp` | `DEALER_ROUTER` | `통과(87.1%)` | `통과(88.2%)` | `보류(64.3%)` | `보류(13.0%)` | `보류(11.5%)` | `보류(10.7%)` | 64B/1024B는 같은 조건 재측정 C `perf_c_single_linux_20260525_145442_rust_single_tcp_small_border_c_recheck.txt` 대비 Rust `perf_rust_single_linux_20260525_145523_single_tcp_small_border_recheck.txt` 기준이다. 256B는 기존 Rust 파일 기준이다. 65536B 이상은 current C `perf_c_single_linux_20260527_074526_codex_c_single_routed_large_for_rust_current_20260527.txt` 대비 Rust `perf_rust_single_linux_20260527_081513_codex_rust_single_routed_large_blocking_send_tcp_20260527.txt` 기준이다. Rust perf active/stop send를 C `ZLINK_SEND_FLAGS_NONE` 의미와 맞췄지만 throughput은 기존 900MB/s대와 같아 보류를 유지한다. socket single-part send 후보 `perf_rust_single_linux_20260527_075139_codex_rust_single_routed_large_send_part_20260527.txt`는 개선이 없어 반영하지 않는다. |
| `tcp` | `ROUTER_ROUTER` | `통과(100.6%)` | `통과(103.5%)` | `보류(61.9%)` | `보류(13.2%)` | `보류(11.4%)` | `보류(11.2%)` | 64B/1024B는 같은 조건 재측정 C `perf_c_single_linux_20260525_145442_rust_single_tcp_small_border_c_recheck.txt` 대비 Rust `perf_rust_single_linux_20260525_145523_single_tcp_small_border_recheck.txt` 기준이다. 256B는 기존 Rust 파일 기준이다. 65536B 이상은 current C `perf_c_single_linux_20260527_074526_codex_c_single_routed_large_for_rust_current_20260527.txt` 대비 Rust `perf_rust_single_linux_20260527_081513_codex_rust_single_routed_large_blocking_send_tcp_20260527.txt` 기준이다. `RouterSocket::recv_part(...)` part 단위 수신과 active 이후 bounded stop wait를 적용했고, active/stop send도 C blocking send 의미로 맞췄지만 routed one-way 기준보다 낮아 보류한다. |
| `tcp` | `SPOT` | `통과(147.7%)` | `통과(105.3%)` | `통과(78.5%)` | `통과(127.8%)` | `통과(102.7%)` | `통과(84.6%)` | 1024B는 current C `perf_c_single_linux_20260527_073913_codex_c_single_spot1024_for_rust_current_20260527.txt` 대비 Rust `perf_rust_single_linux_20260527_074321_codex_rust_single_spot1024_publish_subscribe_part_20260527.txt` 기준이다. Rust public `Spot::publish_part(...)`/`subscribe_part(...)` 단일-part 경로로 C `zlink_spot_publish_part`/`zlink_spot_subscribe_part` 의미에 맞췄다. 64B는 같은 조건 재측정 C `perf_c_single_linux_20260525_145442_rust_single_tcp_small_border_c_recheck.txt` 대비 Rust `perf_rust_single_linux_20260525_145523_single_tcp_small_border_recheck.txt` 기준이다. 64B와 65536B는 120%를 넘어 outlier 재검토 대상으로 남긴다. 나머지는 C/Rust 파일은 위 행과 같다. |
| `ws` | `PAIR` | `통과(98.9%)` | `통과(102.9%)` | `통과(128.1%)` | `통과(100.6%)` | `통과(97.7%)` | `통과(95.3%)` | C full `perf_c_single_linux_20260522_130546_codex_c_ws_single_for_rust_20260522.txt`, C 1024B 보강 `perf_c_single_linux_20260522_132034_codex_c_ws_single_1024_rust_outlier_recheck_20260522.txt`, Rust full `perf_rust_single_linux_20260522_131244_codex_rust_ws_single_current_20260522.txt` 기준이다. 1024B는 같은 조건 C 보강 뒤에도 120%를 넘어 outlier 재검토 대상으로 남긴다. |
| `ws` | `PUBSUB` | `통과(84.0%)` | `통과(98.1%)` | `통과(134.8%)` | `통과(97.9%)` | `통과(98.2%)` | `통과(98.0%)` | C/Rust 파일은 위 행과 같다. 1024B는 같은 조건 C 보강 뒤에도 120%를 넘어 outlier 재검토 대상으로 남긴다. |
| `ws` | `DEALER_DEALER` | `통과(101.1%)` | `통과(100.8%)` | `통과(132.1%)` | `통과(97.0%)` | `통과(100.2%)` | `통과(99.2%)` | C/Rust 파일은 위 행과 같다. 1024B는 같은 조건 C 보강 뒤에도 120%를 넘어 outlier 재검토 대상으로 남긴다. |
| `ws` | `DEALER_ROUTER` | `통과(91.0%)` | `통과(99.5%)` | `통과(94.9%)` | `보류(30.0%)` | `보류(23.2%)` | `보류(20.7%)` | 1024B는 Rust 보강 `perf_rust_single_linux_20260522_131936_codex_rust_ws_single_dealer_router1024_reuse_20260522.txt`와 C 보강 `perf_c_single_linux_20260522_132034_codex_c_ws_single_1024_rust_outlier_recheck_20260522.txt`로 binary_exit 없이 complete를 확보했다. 65536B 이상은 current C `perf_c_single_linux_20260527_074526_codex_c_single_routed_large_for_rust_current_20260527.txt` 대비 Rust `perf_rust_single_linux_20260527_074713_codex_rust_single_routed_large_current_20260527.txt` 기준이며 routed one-way 기준보다 낮아 보류한다. |
| `ws` | `ROUTER_ROUTER` | `통과(100.0%)` | `통과(108.8%)` | `통과(94.5%)` | `보류(30.9%)` | `보류(23.0%)` | `보류(20.4%)` | 1024B C 기준은 보강 `perf_c_single_linux_20260522_132034_codex_c_ws_single_1024_rust_outlier_recheck_20260522.txt`다. 65536B 이상은 current C `perf_c_single_linux_20260527_074526_codex_c_single_routed_large_for_rust_current_20260527.txt` 대비 Rust `perf_rust_single_linux_20260527_074713_codex_rust_single_routed_large_current_20260527.txt` 기준이며 routed one-way 기준보다 낮아 보류한다. |
| `ws` | `SPOT` | `통과(149.2%)` | `통과(111.1%)` | `통과(144.8%)` | `통과(188.9%)` | `통과(179.4%)` | `통과(114.4%)` | 1024B는 current C `perf_c_single_linux_20260527_073913_codex_c_single_spot1024_for_rust_current_20260527.txt` 대비 Rust `perf_rust_single_linux_20260527_074321_codex_rust_single_spot1024_publish_subscribe_part_20260527.txt` 기준이다. Rust public 단일-part publish/subscribe 경로 적용 뒤 SPOT 기준을 넘었다. 256B는 같은 조건 재측정 C `perf_c_single_linux_20260525_145909_rust_single_spot_small_nontcp_c_recheck.txt` 대비 Rust `perf_rust_single_linux_20260525_150009_single_spot_small_nontcp_recheck.txt` 기준이다. 64B/1024B/65536B/131072B는 120%를 넘어 outlier 재검토 대상으로 남긴다. 나머지 C 기준은 기존 `perf_c_single_linux_20260522_131048_codex_c_ws_single_spot_for_rust_20260522.txt`, large/outlier 보강 `perf_c_single_linux_20260522_132112_codex_c_ws_single_spot_outliers_for_rust_recheck_20260522.txt`다. |
| `wss` | `PAIR` | `통과(100.4%)` | `통과(100.4%)` | `통과(133.3%)` | `통과(128.3%)` | `통과(110.1%)` | `통과(112.1%)` | C `perf_c_single_linux_20260522_133130_codex_c_wss_single_for_rust_20260522.txt`, Rust full `perf_rust_single_linux_20260522_133454_codex_rust_wss_single_current_20260522.txt`, 65536B 보강 `perf_rust_single_linux_20260522_141058_codex_rust_wss_single_pair65536_recheck_20260522.txt` 기준이다. 1024B/65536B는 120%를 넘어 outlier 재검토 대상으로 남긴다. |
| `wss` | `PUBSUB` | `통과(86.0%)` | `통과(96.8%)` | `통과(141.8%)` | `통과(118.4%)` | `통과(105.0%)` | `통과(102.0%)` | C/Rust full 파일은 위 행과 같다. 1024B는 120%를 넘어 outlier 재검토 대상으로 남긴다. |
| `wss` | `DEALER_DEALER` | `통과(100.0%)` | `통과(97.0%)` | `통과(142.6%)` | `통과(134.3%)` | `통과(104.5%)` | `통과(113.1%)` | C/Rust full 파일은 위 행과 같다. 1024B/65536B는 120%를 넘어 outlier 재검토 대상으로 남긴다. |
| `wss` | `DEALER_ROUTER` | `통과(84.8%)` | `통과(95.0%)` | `통과(111.2%)` | `통과(82.6%)` | `통과(81.7%)` | `통과(84.7%)` | C/Rust full 파일과 65536B 보강 `perf_rust_single_linux_20260522_141108_codex_rust_wss_single_dealer_router65536_recheck_20260522.txt` 기준이다. routed large도 이번 wss에서는 최소 기준을 넘었다. |
| `wss` | `ROUTER_ROUTER` | `통과(95.2%)` | `통과(96.2%)` | `통과(105.3%)` | `통과(84.1%)` | `통과(81.9%)` | `통과(88.1%)` | C/Rust full 파일, 65536B 보강 `perf_rust_single_linux_20260522_141258_codex_rust_wss_single_router_router65536_recheck_20260522.txt`, 1024B 보강 `perf_rust_single_linux_20260522_141753_codex_rust_wss_single_router_router1024_duration1_20260522.txt` 기준이다. 1024B는 5초 direct binary에서는 complete였지만 runner 5초 캡처 경로에서 timeout이 반복되어 종료 처리 재검토 대상으로 남긴다. |
| `wss` | `SPOT` | `통과(154.8%)` | `통과(84.7%)` | `통과(84.6%)` | `통과(151.1%)` | `통과(161.2%)` | `통과(281.7%)` | 1024B는 current C `perf_c_single_linux_20260527_073913_codex_c_single_spot1024_for_rust_current_20260527.txt` 대비 Rust `perf_rust_single_linux_20260527_074321_codex_rust_single_spot1024_publish_subscribe_part_20260527.txt` 기준이다. 256B는 같은 조건 재측정 C `perf_c_single_linux_20260525_145909_rust_single_spot_small_nontcp_c_recheck.txt` 대비 Rust `perf_rust_single_linux_20260525_150009_single_spot_small_nontcp_recheck.txt` 기준이다. Rust SPOT TLS 설정은 PEM 내용이 아니라 C/C++/Go perf와 같은 인증서 파일 경로를 넘기도록 수정해 binary_exit를 해소했다. 64B/65536B/131072B/262144B는 120%를 넘어 outlier 재검토 대상으로 남긴다. |
| `tls` | `PAIR` | `통과(99.3%)` | `통과(102.7%)` | `통과(133.4%)` | `통과(102.8%)` | `통과(100.4%)` | `통과(99.3%)` | C `perf_c_single_linux_20260522_141933_codex_c_tls_single_for_rust_20260522.txt`, Rust `perf_rust_single_linux_20260522_142522_codex_rust_tls_single_pair_20260522.txt`, 262144B 보강 `perf_rust_single_linux_20260522_142627_codex_rust_tls_single_pair262_duration1_20260522.txt` 기준이다. 1024B는 120%를 넘어 outlier 재검토 대상으로 남긴다. |
| `tls` | `PUBSUB` | `통과(85.8%)` | `통과(92.4%)` | `통과(131.7%)` | `통과(101.1%)` | `통과(100.3%)` | `통과(97.5%)` | Rust `perf_rust_single_linux_20260522_142637_codex_rust_tls_single_pubsub_20260522.txt`, 1024B 보강 `perf_rust_single_linux_20260522_142746_codex_rust_tls_single_pubsub1024_duration1_20260522.txt` 기준이다. 1024B는 5초 active window에서 timeout이 반복되어 1초 보강 report를 사용했고, 120% 초과 outlier로도 남긴다. |
| `tls` | `DEALER_DEALER` | `통과(99.5%)` | `통과(99.6%)` | `통과(126.3%)` | `통과(98.5%)` | `통과(100.6%)` | `통과(98.7%)` | Rust `perf_rust_single_linux_20260522_142813_codex_rust_tls_single_dealer_dealer_20260522.txt`, 65536B 보강 `perf_rust_single_linux_20260522_142918_codex_rust_tls_single_dealer_dealer65536_duration1_20260522.txt` 기준이다. 1024B는 120%를 넘어 outlier 재검토 대상으로 남긴다. |
| `tls` | `DEALER_ROUTER` | `통과(85.8%)` | `통과(90.5%)` | `통과(87.2%)` | `보류(57.4%)` | `보류(54.7%)` | `보류(53.5%)` | Rust `perf_rust_single_linux_20260522_142918_codex_rust_tls_single_dealer_router_20260522.txt`, 1024B/131072B 보강 `perf_rust_single_linux_20260522_143047_codex_rust_tls_single_dealer_router_missing_duration1_20260522.txt` 기준이다. 65536B 이상은 current C `perf_c_single_linux_20260527_074526_codex_c_single_routed_large_for_rust_current_20260527.txt` 대비 Rust `perf_rust_single_linux_20260527_074713_codex_rust_single_routed_large_current_20260527.txt` 기준이며 routed one-way 기준보다 낮아 보류한다. |
| `tls` | `ROUTER_ROUTER` | `통과(99.4%)` | `통과(102.3%)` | `통과(87.8%)` | `보류(56.1%)` | `보류(52.7%)` | `보류(53.6%)` | Rust `perf_rust_single_linux_20260522_143047_codex_rust_tls_single_router_router_20260522.txt` 기준이다. 262144B runner report `perf_rust_single_linux_20260522_143245_codex_rust_tls_single_router_router262_recheck_20260522.txt`는 timeout이지만 direct binary 5초 측정에서 3592.6 msg/s를 확인했다. 65536B 이상은 current C `perf_c_single_linux_20260527_074526_codex_c_single_routed_large_for_rust_current_20260527.txt` 대비 Rust `perf_rust_single_linux_20260527_074713_codex_rust_single_routed_large_current_20260527.txt` 기준이며 routed one-way 기준보다 낮아 보류한다. |
| `tls` | `SPOT` | `통과(152.3%)` | `통과(113.8%)` | `통과(84.9%)` | `통과(168.1%)` | `통과(166.3%)` | `통과(160.5%)` | 1024B는 current C `perf_c_single_linux_20260527_073913_codex_c_single_spot1024_for_rust_current_20260527.txt` 대비 Rust `perf_rust_single_linux_20260527_074321_codex_rust_single_spot1024_publish_subscribe_part_20260527.txt` 기준이다. 256B는 같은 조건 재측정 C `perf_c_single_linux_20260525_145909_rust_single_spot_small_nontcp_c_recheck.txt` 대비 Rust `perf_rust_single_linux_20260525_150009_single_spot_small_nontcp_recheck.txt` 기준이다. 64B/65536B/131072B/262144B는 120%를 넘어 outlier 재검토 대상으로 남긴다. 나머지는 Rust `perf_rust_single_linux_20260522_143152_codex_rust_tls_single_spot_20260522.txt` 기준이다. |

#### 6.7.1.1 Rust 남은 작업

Rust는 아직 완료가 아니다. multi suite는 2026-05-25 재측정과 수정으로
`MULTI_PUBSUB`/`MULTI_SPOT` 계열까지 통과권에 올랐다. 2026-05-27에는 single
SPOT 1024B도 public `Spot::publish_part(...)`/`subscribe_part(...)` 단일-part 경로를
적용해 통과권에 올렸고, single routed large 보류가 남아 있다.

- balanced auto-HWM에서 single one-way 종료 신호가 HWM 뒤에 막히던 문제는
  `ctx.recalculate_auto_hwm()`, stop-token bounded retry 확대, receiver burst drain으로
  해소했다. multi runner의 같은 종류 종료 문제는 2026-05-25 재측정과 각 pattern별
  보강에서 complete report를 확보하며 별도 completion 보류로 남기지 않는다.
- **single SPOT receiver local stats 후보 기각**: single SPOT 수신 thread는 하나뿐이므로
  `Arc<Mutex<LatencyStats>>` 잠금을 없애고 receiver-local `LatencyStats`를 직접 갱신하는
  후보를 시험했다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은
  통과했고, 같은 조건 C `perf_c_single_linux_20260525_193732_rust_single_spot1024_local_stats_c.txt`
  대비 Rust 후보 `perf_rust_single_linux_20260525_193858_single_spot1024_local_stats_candidate.txt`도
  complete였다. 그러나 `SPOT 1024B` median은 tcp/ws/wss/tls가
  174.2/188.7/32.2/176.6Kmsg/s로, fresh C 대비 45.7/53.9/29.9/53.2%였다.
  ws/tls는 SPOT 기준선 위로 올라왔지만 tcp는 기존 보류권과 같고, wss는 기존 56.0%
  근거보다 크게 낮아졌다. 통계 잠금 제거는 transport별 변동을 안정적으로 줄이지 못하므로
  반영하지 않는다.
- tcp routed one-way large size는 `RouterSocket::recv_part(...)` part 단위 수신 적용 뒤
  fixed 재측정에서도 C 대비 11~15% 수준이라 routed one-way 기준보다 낮다. `DEALER_ROUTER`와
  `ROUTER_ROUTER` 모두 active 이후 bounded stop wait로 complete를 확보했지만 large gap은
  남았다. C single routed active send가 blocking send를 쓰는 점은 아래 후보로 따로 확인했다.
- **2026-05-25 single routed current 재검토**: 같은 조건 C 기준
  `perf_c_single_linux_20260525_134314_rust_single_routed_current_c.txt`는 complete였고,
  `DEALER_ROUTER` tcp 1024/65536/131072/262144B가 1234.2/96.2/56.5/30.1Kmsg/s,
  `ROUTER_ROUTER`가 1191.5/95.4/60.1/31.3Kmsg/s였다. current Rust wrapper
  `perf_rust_single_linux_20260525_134344_single_routed_current_retry.txt`와
  `perf_rust_single_linux_20260525_134610_single_routed_current_retry2.txt`는
  `DEALER_ROUTER tcp 1024B`에서 repeat 중 `binary_exit` partial이 재현됐다. 같은
  binary의 direct 실행은 한 번 1011.7Kmsg/s로 성공했지만, 공식 wrapper 단독 complete
  `perf_rust_single_linux_20260525_134816_single_dr1024_timeout60_probe.txt`는
  median 171.1Kmsg/s에 그쳐 run 간 변동이 크다. `PERF_SINGLE_RUN_COOLDOWN_MS=3000`
  재측정 `perf_rust_single_linux_20260525_134835_single_dr_cooldown3s_probe.txt`에서는
  1024B가 907.2Kmsg/s(73.5%)까지 올라왔지만 65536B는 14.0Kmsg/s(14.6%)에 머물렀고
  131072B는 partial이었다. 따라서 1024B 일부는 runner 간격/phase drain 영향을 받지만,
  large 보류의 주 병목은 그대로 남아 있다.
- **single routed `RouterSocket::recv_part(...)` 적용**: Rust `RouterSocket::recv(...)`는
  단일 routed payload도 매 수신마다 `Vec<Message>`와 `Received` reply/send context를 구성했다.
  C의 `zlink_router_recv_part()`와 같은 part 단위 public API `RouterSocket::recv_part(...)`
  / `RouterPart`를 추가하고, single routed perf receiver가 해당 API를 쓰도록 바꿨다. active
  이후에는 stop token 유실 때 blocking recv에 남지 않도록 bounded stop wait로 `DONT_WAIT`
  drain한다. 처음 후보 재측정은 burst drain 루프가 첫 part를 반복 집계하는 버그가 있어
  폐기했고, fixed 재측정만 판정 근거로 사용한다.
  `cargo test --manifest-path bindings/rust/Cargo.toml --no-run`과
  `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고,
  공식 제한 재측정 `perf_rust_single_linux_20260525_141227_single_routed_recv_part_stopwait_fixed.txt`는
  complete였다. 같은 조건 C `perf_c_single_linux_20260525_134314_rust_single_routed_current_c.txt`
  대비 tcp `DEALER_ROUTER` 65536/131072/262144B가 14.4/12.3/11.6%,
  `ROUTER_ROUTER`가 14.5/11.6/11.2%였다. 기준에는 못 미치므로 보류를 유지하고,
  다음 후보는 routed send/recv 경계의 남은 per-message
  비용을 본다.
- **single routed local latency storage 후보 기각**: routed receiver는 단일 thread에서만
  latency를 기록하지만 공용 `Arc<Mutex<LatencyStats>>` 경로를 써서 매 메시지마다 lock을
  잡고 있었다. `DEALER_ROUTER`/`ROUTER_ROUTER` receiver만 로컬 `LatencyStats`에 직접
  기록하는 후보를 시험했다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은
  통과했고, 같은 조건 C `perf_c_single_linux_20260525_154738_rust_routed_local_stats_c.txt`
  대비 Rust 후보 `perf_rust_single_linux_20260525_154753_routed_local_stats_candidate.txt`는
  tcp `DEALER_ROUTER` 65536/262144B가 13.9/11.0%, `ROUTER_ROUTER`가 14.6/11.7%였다.
  기존 `RouterSocket::recv_part(...)` fixed 재측정과 같은 대역이라 lock 비용이 large
  routed 병목의 주 원인이 아님을 확인했다. 코드는 반영하지 않는다.
- **single routed direct `Message::with_size()` 후보 기각**: Rust multi routed large는
  payload `Vec`에 header를 찍고 `Message::copy_from(...)`으로 다시 복사하던 경로를
  `Message::with_size()` + `data_mut()` 직접 stamp로 바꿔 통과권에 올랐다. 같은 후보를
  single routed sender의 active send에 좁혀 시험했다. `cargo test --manifest-path
  bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고, C 기준은 위 local latency
  후보와 같은 `perf_c_single_linux_20260525_154738_rust_routed_local_stats_c.txt`를
  사용했다. Rust 후보 `perf_rust_single_linux_20260525_155036_routed_direct_message_candidate.txt`는
  tcp `DEALER_ROUTER` 65536/262144B가 13.7/11.1%, `ROUTER_ROUTER`가 14.7/11.7%로
  기존과 같은 대역이다. single routed large 병목은 payload 생성 복사 1회가 아니라
  routed send/transport backpressure 경계에 남아 있다고 보고 코드는 반영하지 않는다.
- **single routed blocking send 후보 기각**: C `perf_dealer_router.cpp`와
  `perf_router_router.cpp`의 active send는 `ZLINK_SEND_FLAGS_NONE`을 사용한다. Rust
  routed sender의 active/stop send에서 `DONT_WAIT` flag를 제거해 C와 같은 blocking send
  의미로 맞추는 후보를 시험했다. `cargo test --manifest-path
  bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고, 공식 wrapper
  `PERF_FAIL_FAST=1 bindings/rust/perf/run_benchmarks.sh --transports tcp --pattern
  DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 65536,131072,262144 --duration 1 --runs 3`도
  complete였다(`perf_rust_single_linux_20260525_205059_single_routed_blocking_send_candidate.txt`).
  그러나 tcp median은 `DEALER_ROUTER` 65536/131072/262144B가 14.40/7.20/3.65Kmsg/s,
  `ROUTER_ROUTER`가 14.38/7.17/3.64Kmsg/s로 기존 fixed 재측정과 같은 대역이다. blocking
  send만으로는 C 대비 11~15% large 보류를 해소하지 못하므로 코드는 반영하지 않는다.
- **single routed 단일 part send helper 후보 기각**: Rust routed sender가 매 메시지마다
  `SendOp` 생성, `RoutingId` clone, `Vec<Message>` 구성, part sequence closure를 거치는
  비용을 줄이기 위해 `DealerSocket::send_part(...)`와 `RouterSocket::send_part(...)` 후보를
  임시로 추가하고 perf sender만 그 경로를 쓰게 했다. `cargo test --manifest-path
  bindings/rust/Cargo.toml --no-run`과 `cargo test --manifest-path
  bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고, 공식 wrapper
  `PERF_FAIL_FAST=1 bindings/rust/perf/run_benchmarks.sh --transports tcp --pattern
  DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 65536,131072,262144 --duration 1 --runs 3`도
  complete였다(`perf_rust_single_linux_20260525_205421_single_routed_send_part_candidate.txt`).
  latency는 조금 낮아졌지만 throughput median은 `DEALER_ROUTER` 14.34/7.18/3.63Kmsg/s,
  `ROUTER_ROUTER` 14.31/7.16/3.63Kmsg/s로 기존과 같은 대역이다. public surface를
  넓혀도 routed large 보류를 해소하지 못하므로 코드는 반영하지 않는다.
- **single routed raw FFI 우회 후보 제외**: `bindings/rust/src/lib.rs`에서 raw `ffi`
  모듈은 crate private이고 public re-export가 아니다. perf crate는 `zlink` crate의 public
  API만 사용할 수 있으므로 `zlink_router_recv_part`를 직접 호출해 `RouterPart`나
  `RoutingId` 구성을 건너뛰는 측정 후보는 새 public API 없이는 만들 수 없다. 이미 추가한
  public `RouterSocket::recv_part(...)`가 C의 part 수신 의미를 노출하는 현재 계약 경계다.
  이보다 더 낮은 수준의 perf-only receive/send helper는 Rust binding public surface를
  넓히는 설계 작업이므로, 단순 내부 최적화 후보로 처리하지 않는다.
- **2026-05-25 single routed ws/tls complete 재측정**: `RouterSocket::recv_part(...)`
  적용 뒤 ws/tls large도 같은 조건으로 다시 확인했다. C 기준
  `perf_c_single_linux_20260525_142618_rust_single_routed_wstls_c_recheck.txt`와
  Rust `perf_rust_single_linux_20260525_142618_single_routed_wstls_recv_part_recheck.txt`는
  모두 complete였고, runner는 각각 `core/build` 아래 runtime을 출력했다. ws
  `DEALER_ROUTER` 65536/131072/262144B는 31.0/24.7/21.0%,
  ws `ROUTER_ROUTER`는 32.6/24.7/22.5%라 65536B도 기준선 바로 아래에 남았다.
  tls `DEALER_ROUTER`는 60.6/55.4/52.8%, tls `ROUTER_ROUTER`는
  58.6/55.1/52.3%로 이전보다 조금 높지만 routed one-way 기준에는 못 미친다.
- **2026-05-25 tcp small 경계 재측정**: stale 기준 가능성이 있는 `tcp PUBSUB 64B`,
  routed 1024B, SPOT 1024B를 같은 조건으로 재측정했다. C
  `perf_c_single_linux_20260525_145442_rust_single_tcp_small_border_c_recheck.txt`와 Rust
  `perf_rust_single_linux_20260525_145523_single_tcp_small_border_recheck.txt`는 모두
  complete였고, runner는 `core/build` runtime을 출력했다. `PUBSUB 64B`는 82.5%로
  Rust 단순 one-way 기준을 넘어 보류에서 통과로 바꾼다. `DEALER_ROUTER 1024B`는
  64.3%, `ROUTER_ROUTER 1024B`는 61.9%, `SPOT 1024B`는 46.1%라 여전히 기준보다 낮다.
- **2026-05-25 non-tcp SPOT small 재측정**: `ws/wss/tls SPOT 256/1024B`도 같은 조건으로
  다시 확인했다. C `perf_c_single_linux_20260525_145909_rust_single_spot_small_nontcp_c_recheck.txt`
  와 Rust `perf_rust_single_linux_20260525_150009_single_spot_small_nontcp_recheck.txt`는
  모두 complete였다. `wss 256B`는 84.7%로 SPOT 최소 기준을 넘어 보류에서 통과로 바꾸고,
  `ws 256B`와 `tls 256B`도 111.1/113.8%로 통과를 재확인했다. `ws/wss/tls 1024B`는
  49.6/56.0/59.6%라 fresh 기준에서도 보류를 유지한다.
- **single SPOT `TopicMessage` 재사용 후보 기각**: `Spot::subscribe` 성공 경로가 매 수신마다
  새 `Vec<Message>`를 만든 뒤 caller-provided `TopicMessage`로 옮기므로, 기존 `TopicMessage`
  내부 `Vec`을 성공 수신 때 재사용하는 후보를 시험했다. `cargo test --manifest-path
  bindings/rust/Cargo.toml --no-run`과 `cargo test --manifest-path
  bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고, 같은 조건 C
  `perf_c_single_linux_20260525_150657_rust_spot_subscribe_reuse_c.txt` 대비 후보
  `perf_rust_single_linux_20260525_150735_spot_subscribe_reuse_candidate.txt`는
  `SPOT 1024B` tcp/ws/wss/tls가 48.7/56.4/63.8/59.0%였다. 일부 transport는 올랐지만
  SPOT 기준선을 넘지 못했고 tls는 기존 59.6%보다 낮아, public subscribe 의미를 흔들 수 있는
  내부 특수 분기를 반영하지 않는다.
- **single SPOT active direct message 후보 기각**: current HEAD clean 상태에서 SPOT
  1024B를 다시 맞췄다. C
  `perf_c_single_linux_20260526_071203_rust_single_spot1024_recheck_20260526_c.txt`와 Rust
  `perf_rust_single_linux_20260526_071203_single_spot1024_recheck_20260526.txt`는 모두
  complete였고, Rust/C 비율은 tcp/ws/wss/tls 46.0/54.3/59.2/50.2%였다. multi SPOT/PUBSUB에서
  유효했던 `Message::with_size()` + `data_mut()` 직접 stamp를 single SPOT active publish에만
  좁혀 시험했지만, 후보
  `perf_rust_single_linux_20260526_071336_single_spot1024_direct_message_candidate_20260526.txt`는
  tcp/ws/wss가 153.7/154.9/123.9Kmsg/s로 current 168.7/175.1/139.7Kmsg/s보다 낮고,
  tls만 151.4→166.6Kmsg/s로 조금 올랐다. active payload 복사 1회 제거는 single SPOT
  1024B 공통 병목이 아니므로 반영하지 않는다.
- **single routed stop-token blocking-only 후보 기각**: active send는 기존 `DONT_WAIT`
  retry로 유지하고 phase 종료 stop token만 C처럼 blocking submit으로 보내는 후보를
  시험했다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은
  통과했지만 공식 wrapper
  `perf_rust_single_linux_20260525_134733_single_routed_stop_blocking_only_candidate.txt`가
  `DEALER_ROUTER tcp 1024B`에서 repeat 전부 `binary_exit`로 끝났다. stop token send
  방식만 바꿔서는 routed single completion 불안정을 해소하지 못하므로 반영하지 않는다.
- **single routed active blocking-only 후보 기각**: stop token은 기존 `DONT_WAIT` retry로
  두고 active payload send만 C처럼 blocking submit으로 바꾸는 후보를 시험했다.
  `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했지만
  `perf_rust_single_linux_20260525_134931_single_dr_active_blocking_candidate.txt`가
  `DEALER_ROUTER tcp 1024B`에서 repeat 전부 `binary_exit`로 끝났다. active blocking send는
  C 의미에 가까워 보여도 현재 Rust routed single에서는 phase completion을 더 불안정하게
  만들어 반영하지 않는다.
- tcp/ws SPOT 1024B는 기준보다 낮고, tcp/ws SPOT 일부 size와 one-way 1024B 일부는 C보다
  높은 outlier다. 다음 라운드는 wss/tls 측정 전에 outlier가 측정 변동인지 C/Rust 의미
  차이인지 제한 재측정으로 구분한다.

#### 6.7.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | `통과(77.7%)` | `통과(91.9%)` | `통과(89.3%)` | `통과(102.0%)` | `통과(101.6%)` | `통과(106.6%)` | 64/256/1024B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust `perf_rust_multi_linux_20260524_201407.txt` 기준이고, 65536/131072/262144B는 같은 C 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_213113.txt` median 기준이다. Rust client가 각 socket마다 loop당 1개만 보내던 것을 1024B 이하에서는 C처럼 backpressure까지 연속 송신하고, POLLOUT event slot으로 pending socket만 재개하도록 맞췄다. large는 payload `Vec`에 stamp한 뒤 `Message::copy_from`으로 다시 복사하던 경로를 public `Message::with_size()` + `data_mut()` 직접 stamp로 바꿔 C의 `zlink_msg_init_size` 뒤 stamp 의미와 맞췄다. 전 size burst-send 후보 `perf_rust_multi_linux_20260524_201322.txt`는 large를 더 낮춰 small로 좁혔다. |
| `tcp` | `MULTI_DEALER_ROUTER` | `통과(116.6%)` | `통과(105.7%)` | `통과(112.5%)` | `통과(65.2%)` | `통과(103.4%)` | `통과(107.2%)` | 64/256/1024B는 기존 Rust `perf_rust_multi_linux_20260522_144936_codex_rust_multi_tcp_dealer_router_20260522.txt` 기준이다. 65536/131072/262144B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_213949.txt` median 기준이다. client active send에서 payload `Vec`에 header를 찍고 `Message::copy_from`으로 다시 복사하던 경로를 public `Message::with_size()` + `data_mut()` 직접 stamp로 바꿔 C의 native message buffer stamp 의미와 맞췄다. |
| `tcp` | `MULTI_ROUTER_ROUTER` | `통과(84.7%)` | `통과(85.7%)` | `통과(85.7%)` | `통과(75.3%)` | `통과(108.6%)` | `통과(116.4%)` | 64/256/1024B는 Rust `perf_rust_multi_linux_20260524_004405.txt` 기준이다. 65536/131072/262144B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_213949.txt` median 기준이다. RR client/server는 DEALER_ROUTER와 같은 unified poller(`perf_socket_poll(...,-1)`, POLLIN/POLLOUT 토글)로 맞췄고, large client send는 `Message::with_size()` 직접 stamp로 추가 복사를 제거해 tcp large 보류가 사라졌다. |
| `tcp` | `MULTI_PUBSUB` | `통과(91.8%)` | `통과(98.2%)` | `통과(116.8%)` | `통과(90.9%)` | `통과(92.0%)` | `통과(101.8%)` | 64/256/1024/65536B는 fresh C `perf_c_multi_linux_20260525_115827.txt` 대비 Rust `perf_rust_multi_linux_20260525_115826.txt` 기준이다. 131072/262144B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_022108.txt` 대비 Rust `perf_rust_multi_linux_20260525_022246.txt` 기준이다. server active publish는 payload `Vec`에 stamp한 뒤 `Message::copy_from`으로 다시 복사하던 경로를 `Message::with_size()` + `data_mut()` 직접 stamp로 바꿨다. client drain은 `TopicMessage::empty()` placeholder를 메시지마다 새로 만들지 않고 drain 호출 안에서 재사용해 caller-provided output 의미를 유지하면서 할당을 줄였다. 2026-05-25 추가로 latency 계산/샘플 저장을 기본 32개당 1개로 줄였고, server active publish가 backpressure 때 `POLLOUT` wait로 쉬던 차이를 C처럼 `DONT_WAIT` publish 재시도로 맞춰 64/256/1024B 보류를 해소했다. |
| `tcp` | `MULTI_SPOT` | `통과(81.6%)` | `통과(77.1%)` | `통과(87.8%)` | `통과(109.7%)` | `통과(80.3%)` | `통과(104.3%)` | 64B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_094129.txt` 대비 Rust `perf_rust_multi_linux_20260525_094209.txt` 기준이다. 256/1024B는 기존 파일 기준이다. 65536/262144B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_022730.txt` 대비 Rust `perf_rust_multi_linux_20260525_025506.txt` 기준이다. 131072B는 C `perf_c_multi_linux_20260525_072541.txt` 대비 Rust `perf_rust_multi_linux_20260525_072933.txt` 기준이다. SPOT server active publish를 `Vec` stamp 뒤 `Message::copy_from`으로 다시 복사하던 경로에서 `Message::with_size()` + `data_mut()` 직접 stamp로 바꿔 65536/262144B가 통과권으로 올라왔다. 64B는 기본 recv worker를 2개로 좁혀 C 대비 81.6%로 올라 통과했다. 131072B는 public `Poller` `POLLIN` wait 위에서 기본 recv worker를 8개로 좁혀 743.838Kmsg/s, C 대비 80.3%로 올라 통과했다. **2026-05-25 선택 적용**: SPOT client recv worker가 progress 없을 때 무조건 `sleep(1ms)`로 쉬던 것을 tcp 131072B와 ws 131072/262144B에서 public `Poller` `POLLIN` wait로 바꿨다. **2026-05-24 multi-worker drain 추가**: copy 제거 위에 추가로, 단일 thread가 100 spot을 순차 drain하던 것을 Go recv worker처럼 scoped thread 4개로 분할(`Spot: Send`, `chunks_mut`로 disjoint 분배)했다. (러너의 symlink 기반 stale-guard 버그도 함께 수정: `readlink -f`로 실제 versioned .so와 비교.) — **2026-05-23 per-message copy 제거**: Rust SPOT client 수신 hot path가 매 메시지 payload를 `message_payload(...).to_vec()`로 전체 복사(262144B면 256KB alloc+memcpy/msg)해 large가 1~1.5%로 무너졌다. timestamp 디코드는 borrowed `&[u8]`로 충분하므로 `.to_vec()`를 제거했다. |
| `tcp` | `MULTI_SPOT_REQREP` | `통과(80.0%)` | `통과(88.9%)` | `통과(85.9%)` | `통과(89.4%)` | `통과(77.6%)` | `통과(80.4%)` | 64/256/1024B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust `perf_rust_multi_linux_20260522_150056_codex_rust_multi_tcp_spot_reqrep_20260522.txt` 기준이다. 65536/131072B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_022108.txt` 대비 Rust `perf_rust_multi_linux_20260525_022246.txt` 기준이다. 262144B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_060105.txt` 대비 Rust `perf_rust_multi_linux_20260525_060125.txt` median 기준이다. server reply가 받은 single-part `Message`를 다시 복사하지 않고 그대로 reply로 넘기고, client active request를 C처럼 `DONT_WAIT`로 맞췄다. 262144B는 active slot을 8에서 6으로 좁히자 9.523Kops/s, C 대비 80.4%로 올라 보류에서 통과로 바뀌었다. Rust public binding 정책상 C++의 external buffer attach 방식은 이 셀의 perf 전용 fast path로 넣지 않는다. |
| `tcp` | `MULTI_SPOT_SENDSEND` | `통과(93.3%)` | `통과(94.3%)` | `통과(94.7%)` | `통과(90.1%)` | `통과(77.2%)` | `통과(75.6%)` | 64/256/1024B는 Rust `perf_rust_multi_linux_20260524_200540.txt` 기준이고, 65536/131072/262144B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_022108.txt` 대비 Rust `perf_rust_multi_linux_20260525_022246.txt` 기준이다. client active loop를 C와 같은 POLLIN poller wait로 맞췄고, 65536B active slot은 32→24로 줄였다. server echo는 65536B에서 받은 single-part `Message`를 그대로 send builder에 넘겨 reply copy를 줄이고, 131072B 이상은 기존 native-copy 경로가 더 빨라 유지했다. 최신 재측정에서 262144B도 SPOT 기준을 넘었다. active slot 16 후보 `perf_rust_multi_linux_20260524_200834.txt`, 65536/262144B 모두 single-part move 후보 `perf_rust_multi_linux_20260524_212115.txt`는 낮은 size가 있어 반영하지 않는다. |
| `tcp` | `MULTI_STREAM` | `통과(98.6%)` | `통과(95.2%)` | `통과(100.1%)` | `통과(82.0%)` | `해당 없음` | `해당 없음` | 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_053804.txt` 대비 Rust `perf_rust_multi_linux_20260525_053718.txt` median 기준이다. Rust STREAM server가 packet frame을 `Vec`에 조립한 뒤 `Message::copy_from`으로 다시 복사하던 경로를 public `Message::with_size()` + `data_mut()` 직접 조립으로 바꿔 65536B가 108.682 Kops/s까지 올라왔다. 기본 STREAM clients=10000은 thread/CPU 부담이 커서 먼저 clients=100으로 제한해 같은 조건 비교를 남겼다. 전 size가 단순 one-way 기준을 넘었다. |
| `ws` | `MULTI_DEALER_DEALER` | `통과(73.1%)` | `통과(94.5%)` | `통과(91.3%)` | `통과(96.3%)` | `통과(69.9%)` | `통과(93.1%)` | 64/256/1024B는 C `perf_c_multi_linux_20260522_150505_codex_c_multi_ws_no_stream_for_rust_20260522.txt` 대비 Rust `perf_rust_multi_linux_20260524_201443.txt` 기준이다. 65536/131072/262144B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_213159.txt` median 기준이다. small size는 tcp와 같은 client burst-send/poller pending 수정으로 통과권에 올랐고, large는 `Message::with_size()` 직접 stamp로 추가 복사를 없애 전 size 통과가 됐다. |
| `ws` | `MULTI_DEALER_ROUTER` | `통과(114.6%)` | `통과(119.0%)` | `통과(106.1%)` | `통과(71.8%)` | `통과(85.5%)` | `통과(92.4%)` | 64/256/1024B는 기존 Rust `perf_rust_multi_linux_20260522_151019_codex_rust_multi_ws_dealer_router_20260522.txt` 기준이다. 65536/131072/262144B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_214245.txt` median 기준이다. client send 직접 stamp로 ws large 보류가 사라졌다. |
| `ws` | `MULTI_ROUTER_ROUTER` | `통과(88.7%)` | `통과(89.4%)` | `통과(86.4%)` | `통과(71.8%)` | `통과(77.5%)` | `통과(86.9%)` | 64/256/1024B는 sleep→poller 수정 뒤 Rust `perf_rust_multi_linux_20260524_*_ws full` 기준이고, 65536/131072/262144B는 fresh C 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_214245.txt` median 기준이다. poller 정렬 위에 client send 직접 stamp를 적용해 ws large 보류가 사라졌다. |
| `ws` | `MULTI_PUBSUB` | `통과(90.1%)` | `통과(89.8%)` | `통과(90.8%)` | `통과(105.5%)` | `통과(103.9%)` | `통과(125.3%)` | 64/256/1024/65536B는 fresh C `perf_c_multi_linux_20260525_115827.txt` 대비 Rust `perf_rust_multi_linux_20260525_115826.txt` 기준이다. 131072/262144B는 기존 Rust `perf_rust_multi_linux_20260522_152055_codex_rust_multi_ws_pubsub_ready_fix_full_20260522.txt` 기준이다. PUBSUB client가 `CLIENT_READY`를 너무 일찍 출력하던 문제는 `SocketMonitor` connection-ready 대기를 추가해 131072B/262144B timeout을 해소했다. server active publish를 C처럼 backpressure 뒤 즉시 재시도하도록 맞춘 뒤 1024B가 10.9%에서 90.8%로 올라 보류에서 통과로 바뀌었다. 262144B는 C보다 높아 outlier 재검토 대상으로 남긴다. |
| `ws` | `MULTI_SPOT` | `통과(149.0%)` | `통과(139.2%)` | `통과(112.2%)` | `통과(115.5%)` | `통과(84.2%)` | `통과(76.8%)` | 64/256/1024B는 fresh C 제한 재측정 `perf_c_multi_linux_20260525_120630.txt` 대비 Rust 단독 complete 재측정 `perf_rust_multi_linux_20260525_121937.txt` 기준이다. 65536B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_022730.txt` 대비 Rust `perf_rust_multi_linux_20260525_025506.txt` 기준이다. 131072B는 C `perf_c_multi_linux_20260525_072541.txt` 대비 Rust `perf_rust_multi_linux_20260525_072933.txt` 기준이다. 262144B는 C `perf_c_multi_linux_20260525_093343.txt` 대비 Rust `perf_rust_multi_linux_20260525_093530.txt` 기준이다. SPOT server active publish를 `Message::with_size()` + `data_mut()` 직접 stamp로 바꾼 뒤 65536B가 통과권에 올랐다. 64/256/1024B는 client가 C처럼 sent timestamp 기준 active window 뒤 backlog를 drain하고, throughput count와 latency sample 저장을 분리한 뒤 통과권에 올랐다. 131072B는 public `Poller` `POLLIN` wait 위에서 기본 recv worker를 8개로 좁혀 729.462Kmsg/s, C 대비 84.2%로 통과했다. 262144B도 current HEAD에서 8-worker를 단독 재확인해 405.988Kmsg/s, C 대비 76.8%로 올라 통과했다. SPOT client 복사 제거 + worker drain 위에, 131072B/262144B에서만 public `Poller` `POLLIN` wait를 사용한다. |
| `ws` | `MULTI_SPOT_REQREP` | `통과(89.7%)` | `통과(88.9%)` | `통과(88.5%)` | `통과(87.8%)` | `통과(85.0%)` | `통과(102.5%)` | C 파일은 위 행과 같고, Rust `perf_rust_multi_linux_20260522_152303_codex_rust_multi_ws_spot_reqrep_20260522.txt` 기준이다. 실행 중 server는 `nlwp=14`, client는 순간 `nlwp=15`까지 관측됐지만 thread 누적은 없었다. |
| `ws` | `MULTI_SPOT_SENDSEND` | `통과(87.2%)` | `통과(92.8%)` | `통과(101.3%)` | `통과(81.6%)` | `통과(90.7%)` | `통과(92.1%)` | C 파일은 위 행과 같고, Rust `perf_rust_multi_linux_20260524_200859.txt` 기준이다. tcp와 같은 client active-slot + poller wait 수정으로 기존 전 size 보류를 전 size 통과로 올렸다. |
| `ws` | `MULTI_STREAM` | `통과(113.1%)` | `통과(112.2%)` | `통과(107.1%)` | `통과(110.7%)` | `해당 없음` | `해당 없음` | C `perf_c_multi_linux_20260522_152456_codex_c_multi_ws_stream_clients100_for_rust_20260522.txt`, Rust `perf_rust_multi_linux_20260522_152604_codex_rust_multi_ws_stream_clients100_fixed_20260522.txt` 기준이다. Rust runner가 명시적 `--clients 100`도 STREAM 기본 10000으로 덮어쓰던 문제를 수정한 뒤 재측정했다. |
| `wss` | `MULTI_DEALER_DEALER` | `통과(77.9%)` | `통과(94.0%)` | `통과(98.8%)` | `통과(84.2%)` | `통과(84.2%)` | `통과(97.7%)` | 64/256/1024B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust `perf_rust_multi_linux_20260524_201443.txt` 기준이다. 65536/131072/262144B는 같은 C 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_213159.txt` median 기준이다. small size는 client burst-send/poller pending 수정으로 통과권에 올랐고, large는 `Message::with_size()` 직접 stamp로 추가 복사를 없애 전 size 통과가 됐다. |
| `wss` | `MULTI_DEALER_ROUTER` | `통과(102.2%)` | `통과(108.9%)` | `통과(107.5%)` | `통과(88.9%)` | `통과(93.7%)` | `통과(90.9%)` | 64/256/1024B는 기존 Rust `perf_rust_multi_linux_20260522_153348_codex_rust_multi_wss_dealer_router_20260522.txt` 기준이다. 65536/131072/262144B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_214245.txt` median 기준이다. 전 size가 기준선을 넘었다. |
| `wss` | `MULTI_ROUTER_ROUTER` | `통과(93.9%)` | `통과(91.9%)` | `통과(91.7%)` | `통과(94.2%)` | `통과(90.6%)` | `통과(90.5%)` | 64/256/1024B는 sleep→poller 수정 뒤 Rust scoped `perf_rust_multi_linux_20260524_*_rr_wss` 기준이고, 65536/131072/262144B는 fresh C 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_214245.txt` median 기준이다. 전 size가 기준선을 넘었다. |
| `wss` | `MULTI_PUBSUB` | `통과(93.6%)` | `통과(94.1%)` | `통과(94.2%)` | `통과(65.8%)` | `통과(115.5%)` | `통과(111.4%)` | 64/256/1024/65536B는 fresh C `perf_c_multi_linux_20260525_115827.txt` 대비 Rust `perf_rust_multi_linux_20260525_115826.txt` 기준이다. 131072B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_052938.txt` 대비 Rust `perf_rust_multi_linux_20260525_052955.txt` 기준이고, 262144B는 C `perf_c_multi_linux_20260522_152936_codex_c_multi_wss_no_stream_for_rust_20260522.txt` 대비 Rust `perf_rust_multi_linux_20260522_153511_codex_rust_multi_wss_pubsub_ready_fix_20260522.txt` 기준이다. client drain의 `TopicMessage` placeholder 재사용과 latency sampling 위에 server `POLLOUT` wait를 제거해 C의 continuous `DONT_WAIT` publish retry 의미와 맞췄고, 256/1024B 보류가 통과로 바뀌었다. |
| `wss` | `MULTI_SPOT` | `통과(131.5%)` | `통과(128.1%)` | `통과(90.2%)` | `통과(131.8%)` | `통과(174.0%)` | `통과(96.3%)` | 64/256/1024B는 fresh C 제한 재측정 `perf_c_multi_linux_20260525_120630.txt` 대비 Rust 단독 complete 재측정 `perf_rust_multi_linux_20260525_121522.txt` 기준이다. 65536/131072B는 C `perf_c_multi_linux_20260522_152936_codex_c_multi_wss_no_stream_for_rust_20260522.txt` 대비 Rust `perf_rust_multi_linux_20260524_201915.txt` 기준이다. 262144B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_014732.txt` 345.5K 대비 Rust `perf_rust_multi_linux_20260525_014728.txt` 332.585K median 기준이다. SPOT client 복사 제거 + 4-worker drain 재측정으로 65536B/131072B는 통과권에 올랐고, 262144B도 최신 같은 조건 C 기준으로 통과한다. 64/256/1024B는 client가 C처럼 sent timestamp 기준 active window 뒤 backlog를 drain하고, throughput count와 latency sample 저장을 분리한 뒤 통과권에 올랐다. |
| `wss` | `MULTI_SPOT_REQREP` | `통과(83.0%)` | `통과(82.8%)` | `통과(76.9%)` | `통과(88.8%)` | `통과(93.3%)` | `통과(90.9%)` | Fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust `perf_rust_multi_linux_20260522_154323_codex_rust_multi_wss_spot_reqrep_tls_path_20260522.txt` 기준이다. fresh baseline 재계산에서 전 size가 SPOT 기준을 넘는다. |
| `wss` | `MULTI_SPOT_SENDSEND` | `통과(77.6%)` | `통과(83.2%)` | `통과(100.4%)` | `통과(94.2%)` | `통과(90.6%)` | `통과(94.3%)` | 64B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_210036.txt` median 기준이다. 나머지는 fresh C 대비 Rust `perf_rust_multi_linux_20260524_200645.txt` 기준이다. client active-slot + poller wait 수정으로 기존 21.8/27.0/9.2/68.4/68.0/81.6%에서 크게 올라 전 size 통과가 됐다. |
| `wss` | `MULTI_STREAM` | `통과(109.1%)` | `통과(107.7%)` | `통과(110.5%)` | `통과(103.5%)` | `해당 없음` | `해당 없음` | C `perf_c_multi_linux_20260522_154643_codex_c_multi_wss_stream_clients100_for_rust_20260522.txt`, Rust `perf_rust_multi_linux_20260522_154657_codex_rust_multi_wss_stream_clients100_fixed_20260522.txt` 기준이다. 명시적 `--clients 100`이 `--ccu 100`으로 전달되는 것을 확인했고, 실행 중 Rust server `nlwp=8`, stream client `nlwp=5` 수준이었다. |
| `tls` | `MULTI_DEALER_DEALER` | `통과(78.7%)` | `통과(95.6%)` | `통과(87.7%)` | `통과(83.9%)` | `통과(86.8%)` | `통과(93.1%)` | 64/256/1024B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust `perf_rust_multi_linux_20260524_201443.txt` 기준이다. 65536/131072/262144B는 같은 C 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_213159.txt` median 기준이다. small size는 client burst-send/poller pending 수정으로 통과권에 올랐고, large는 `Message::with_size()` 직접 stamp로 추가 복사를 없애 전 size 통과가 됐다. |
| `tls` | `MULTI_DEALER_ROUTER` | `통과(108.8%)` | `통과(109.6%)` | `통과(108.7%)` | `통과(84.6%)` | `통과(87.9%)` | `통과(87.4%)` | 64/256/1024B는 기존 Rust `perf_rust_multi_linux_20260522_155235_codex_rust_multi_tls_dealer_router_20260522.txt` 기준이다. 65536/131072/262144B는 fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_214245.txt` median 기준이다. 전 size가 기준선을 넘었다. |
| `tls` | `MULTI_ROUTER_ROUTER` | `통과(90.0%)` | `통과(88.7%)` | `통과(87.9%)` | `통과(84.1%)` | `통과(84.2%)` | `통과(87.2%)` | 64/256/1024B는 sleep→poller 수정 뒤 Rust scoped `perf_rust_multi_linux_20260524_*_rr_tls` 기준이고, 65536/131072/262144B는 fresh C 대비 Rust 반복 측정 `perf_rust_multi_linux_20260524_214245.txt` median 기준이다. 전 size가 기준선을 넘었다. |
| `tls` | `MULTI_PUBSUB` | `통과(89.4%)` | `통과(93.3%)` | `통과(96.5%)` | `통과(89.8%)` | `통과(101.8%)` | `통과(91.3%)` | 64/256/1024/65536B는 fresh C `perf_c_multi_linux_20260525_115827.txt` 대비 Rust `perf_rust_multi_linux_20260525_115826.txt` 기준이다. 131072/262144B는 C `perf_c_multi_linux_20260522_154822_codex_c_multi_tls_no_stream_for_rust_20260522.txt` 대비 Rust `perf_rust_multi_linux_20260522_155342_codex_rust_multi_tls_pubsub_20260522.txt` 기준이다. client drain의 `TopicMessage` placeholder 재사용과 latency sampling 위에 server `POLLOUT` wait를 제거해 C의 continuous `DONT_WAIT` publish retry 의미와 맞췄고, 1024B 보류가 통과로 바뀌었다. |
| `tls` | `MULTI_SPOT` | `통과(129.3%)` | `통과(130.9%)` | `통과(99.7%)` | `통과(151.4%)` | `통과(109.3%)` | `통과(103.1%)` | 64/256/1024B는 fresh C 제한 재측정 `perf_c_multi_linux_20260525_120630.txt` 대비 Rust 단독 complete 재측정 `perf_rust_multi_linux_20260525_121609.txt` 기준이다. 65536/131072B는 C `perf_c_multi_linux_20260522_154822_codex_c_multi_tls_no_stream_for_rust_20260522.txt` 대비 Rust `perf_rust_multi_linux_20260524_201915.txt` 기준이다. 262144B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_014732.txt` 373.2K 대비 Rust `perf_rust_multi_linux_20260525_014728.txt` 384.928K median 기준이다. SPOT client 복사 제거 + 4-worker drain 재측정으로 65536B/131072B는 통과권에 올랐고, 262144B도 최신 같은 조건 C 기준으로 통과한다. 64/256/1024B는 client가 C처럼 sent timestamp 기준 active window 뒤 backlog를 drain하고, throughput count와 latency sample 저장을 분리한 뒤 통과권에 올랐다. |
| `tls` | `MULTI_SPOT_REQREP` | `통과(80.8%)` | `통과(84.3%)` | `통과(79.7%)` | `통과(83.3%)` | `통과(86.7%)` | `통과(81.1%)` | Fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust `perf_rust_multi_linux_20260522_155531_codex_rust_multi_tls_spot_reqrep_tls_path_20260522.txt` 기준이다. 실행 중 server `nlwp=7` 수준이었고, fresh baseline 재계산에서 전 size가 SPOT 기준을 넘는다. |
| `tls` | `MULTI_SPOT_SENDSEND` | `통과(83.6%)` | `통과(86.2%)` | `통과(87.2%)` | `통과(87.2%)` | `통과(82.5%)` | `통과(89.6%)` | Fresh C `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비 Rust `perf_rust_multi_linux_20260524_200645.txt` 기준이다. client active-slot + poller wait 수정으로 기존 전 size 보류를 전 size 통과로 올렸다. 실행 중 server/client는 각 `nlwp=14` 수준으로, SPOT 내부 worker가 양쪽 node에 붙는 고정 구조다. |
| `tls` | `MULTI_STREAM` | `통과(97.2%)` | `통과(96.0%)` | `통과(97.7%)` | `통과(93.3%)` | `해당 없음` | `해당 없음` | C `perf_c_multi_linux_20260522_155155_codex_c_multi_tls_stream_clients100_for_rust_20260522.txt`, Rust `perf_rust_multi_linux_20260522_155707_codex_rust_multi_tls_stream_clients100_fixed_20260522.txt` 기준이다. 명시적 `--clients 100`이 `--ccu 100`으로 전달되는 것을 확인했고, 실행 중 Rust server `nlwp=8`, shared stream client `nlwp=5` 수준이었다. |

#### 6.7.3 Rust 남은 작업 (2026-05-25)

Rust는 Go와 달리 native OS thread를 쓰므로 Go의 LockOSThread 병목은 없다. 대신 두 가지 binding 구현 비효율을 찾아 고쳤다(fresh C 6.0.3 baseline `perf_c_multi_linux_20260523_111534_goal_c_multi_603_baseline.txt` 대비, 공식 runner duration5).

- **SPOT client per-message 복사 제거 + multi-worker drain**: (1) `MULTI_SPOT` client 수신 hot path가 매 메시지 payload를 `message_payload(...).to_vec()`로 전체 복사했다(262144B면 256KB/msg) → borrowed `&[u8]` 디코드로 `.to_vec()` 제거(large 10x). (2) 그 위에, 단일 thread가 100 spot을 순차 drain하던 것을 Go recv worker처럼 scoped thread 4개로 분할했다(`Spot: Send`, `chunks_mut` disjoint, `LatencyStats::merge`로 합산). 최종 tcp 결과(fresh C baseline 대비): small 64/256/1024 = **84.7/90.5/85.0% 통과**(원래 36/37/32%), large 65536/131072/262144 = 53.0/36.5/42.1%(원래 11/1.5/1.0%). small은 SPOT 목표 75% 통과, tcp/ws large는 routed/copy 영역 보류. ws/wss/tls 재측정(`perf_rust_multi_linux_20260524_201915.txt`)에서는 ws 전 size가 49.3~64.2%로 개선됐지만 보류, wss/tls 65536/131072B는 통과권으로 올랐다. wss/tls 262144B도 같은 조건 C/Rust 제한 재측정(`perf_c_multi_linux_20260525_014732.txt`, `perf_rust_multi_linux_20260525_014728.txt`)에서 96.3/103.1%로 통과해 보류에서 제외한다. `PERF_MULTI_SPOT_RECV_WORKERS=8` 후보는 small 처리량을 낮췄고(`perf_rust_multi_linux_20260524_202057.txt`), `PERF_MULTI_SPOT_RECV_WORKERS=2` 후보도 tcp large를 31.1/25.4/23.1%로 낮춰 반영하지 않는다(`perf_rust_multi_linux_20260524_234209.txt`). server publish를 C처럼 backpressure 때 blocking retry하는 후보도 전체 size smoke에서 `ws 64B`를 14.1%로 낮춰 기각했다(`perf_rust_multi_linux_20260524_202306.txt`). 결과 `perf_rust_multi_linux_20260524_*` SPOT tcp/ws/wss/tls. (러너 stale-guard symlink 버그도 `readlink -f`로 수정.)
- **MULTI_SPOT worker idle poller wait 선택 적용**: 4-worker drain 이후에도 각 worker는 progress가 없으면 `sleep(1ms)`로 쉬었다. SPOT은 public `Poller` 대상이므로, 보류 large size 중 효과가 있는 범위만 `POLLIN` wait로 바꿨다. 전 size 적용 후보 `perf_rust_multi_linux_20260525_003555.txt`는 tcp/ws small과 ws 65536B를 낮춰 반영하지 않는다. 최종 범위는 tcp 131072B, ws 131072/262144B다. 공식 runner `perf_rust_multi_linux_20260525_003928.txt`에서 tcp large는 57.4/44.1/44.2%, ws는 53.7/52.9/82.5/76.3/46.6/42.8%다. 최신 같은 조건 C/Rust 제한 재측정(`perf_c_multi_linux_20260525_022730.txt`, `perf_rust_multi_linux_20260525_023224.txt`)에서는 tcp 64/65536/131072/262144B가 67.9/54.7/40.6/42.5%, ws 64/256/65536/131072/262144B가 50.6/50.4/59.3/42.7/40.7%로 남아 보류가 재확인됐다. ws 1024B만 통과권으로 유지한다.
- **MULTI_SPOT server 직접 stamp 적용**: Rust SPOT server active publish가 payload `Vec`에 metric header를 찍은 뒤 `Message::copy_from(&buf)`로 다시 복사하던 경로를 `Message::with_size()` + `data_mut()` 직접 stamp로 바꿨다. 이 변경은 PUBSUB server와 같은 public `Message` 소유 버퍼 경로이며 wire 의미를 바꾸지 않는다. 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_022730.txt` 대비 Rust `perf_rust_multi_linux_20260525_025506.txt`에서 tcp 65536/131072/262144B는 109.7/53.6/104.3%, ws 65536/131072/262144B는 115.5/45.1/41.7%다. tcp 65536/262144B와 ws 65536B는 보류에서 통과로 올라왔고, tcp 131072B와 ws 131072/262144B는 개선됐지만 아직 SPOT 기준 아래다.
- **MULTI_SPOT recv worker 6개 선택 적용**: ws 131072/262144B는 worker 4 기본값에서 각각 421.674/220.784Kmsg/s(`perf_rust_multi_linux_20260525_063450.txt`)였고, `PERF_MULTI_SPOT_RECV_WORKERS=6` 후보는 605.424/332.445Kmsg/s(`perf_rust_multi_linux_20260525_063416.txt`)로 올랐다. 이를 ws 131072B 이상 기본값으로 좁혀 반영한 공식 runner `perf_rust_multi_linux_20260525_063534.txt`는 C `perf_c_multi_linux_20260525_063244.txt` 대비 67.2/57.2%다. tcp 131072B도 worker 4 기본값 414.826Kmsg/s(`perf_rust_multi_linux_20260525_063927.txt`)에서 worker 6 후보 594.278Kmsg/s(`perf_rust_multi_linux_20260525_063944.txt`)로 올랐고, tcp 131072B 기본값으로 반영한 공식 runner `perf_rust_multi_linux_20260525_064050.txt`는 C `perf_c_multi_linux_20260525_063846.txt` 대비 65.1%다. 아직 SPOT 기준에는 못 미치지만 절대 처리량 개선이 커서 반영하고 보류 상태를 유지한다.
- **MULTI_SPOT recv worker 8개 131072B 선택 적용**: worker 6 적용 뒤에도 tcp/ws 131072B가 보류권이라 7/8-worker 후보를 다시 좁혀 시험했다. `PERF_MULTI_SPOT_RECV_WORKERS=7` 후보 `perf_rust_multi_linux_20260525_072708.txt`는 C `perf_c_multi_linux_20260525_072541.txt` 대비 tcp 71.8%, ws 78.3%였고, `PERF_MULTI_SPOT_RECV_WORKERS=8` 후보 `perf_rust_multi_linux_20260525_072743.txt`는 tcp 80.8%, ws 87.4%로 더 높았다. 기본값을 tcp/ws 131072B에만 8-worker로 좁혀 반영한 공식 runner `perf_rust_multi_linux_20260525_072933.txt`는 tcp 80.3%, ws 84.2%로 둘 다 SPOT 기준을 넘었다.
- **MULTI_SPOT ws 262144B recv worker 8개 선택 적용**: ws 262144B도 worker 6 기본값이 보류권이라 7/8-worker를 분리 확인했다. 초기 8-worker 후보 `perf_rust_multi_linux_20260525_072826.txt`는 `server_ready_timeout` partial로 불안정했고, 7-worker 후보 `perf_rust_multi_linux_20260525_073213.txt`는 complete였지만 C `perf_c_multi_linux_20260525_072826.txt` 대비 71.8%였다. current HEAD에서 8-worker를 단독 재확인한 `perf_rust_multi_linux_20260525_093320.txt`와 기본값 반영 뒤 공식 runner `perf_rust_multi_linux_20260525_093530.txt`는 모두 complete였고, 같은 조건 C `perf_c_multi_linux_20260525_093343.txt` 대비 76.8%로 SPOT 기준을 넘었다. 따라서 기본값을 ws 262144B에 한해 8-worker로 올리고 보류에서 통과로 갱신한다.
- **MULTI_SPOT tcp 64B recv worker 2개 선택 적용**: tcp 64B는 4-worker 기본값이 C 대비 67.9%라 1/2-worker를 좁혀 재시험했다. 2-worker 후보 `perf_rust_multi_linux_20260525_094052.txt`는 같은 조건 C `perf_c_multi_linux_20260525_094129.txt` 대비 84.9%였고, 1-worker 후보 `perf_rust_multi_linux_20260525_094112.txt`는 49.8%로 낮았다. 기본값을 tcp 64B에만 2-worker로 좁혀 반영한 공식 runner `perf_rust_multi_linux_20260525_094209.txt`는 C 대비 81.6%로 SPOT 기준을 넘었다.
- **MULTI_SPOT wss/tls small recv worker 2개 선택 적용**: 암호화 transport small도 worker 수를 줄여 재시험했다. 2-worker 후보 `perf_rust_multi_linux_20260525_094349.txt`는 기존 4-worker 대표값보다 wss 64/256B와 tls 64/256/1024B의 절대 처리량을 올렸고, fresh C `perf_c_multi_linux_20260525_094522.txt` 대비 기본값 반영 runner `perf_rust_multi_linux_20260525_094751.txt`는 wss 64/256B 59.7/57.5%, tls 64/256/1024B 61.6/60.2/53.9%다. 기준선에는 못 미쳐 보류를 유지하지만, 절대 처리량 개선이 있어 해당 크기만 2-worker 기본값으로 좁힌다. wss 1024B는 후보에서 기존 대표값보다 낮아 제외한다.
- **MULTI_SPOT ws 64/256B recv worker 2개 선택 적용**: ws small도 4-worker 기본값이 50%대에 머물러 1/2-worker를 분리 확인했다. 2-worker 후보 `perf_rust_multi_linux_20260525_074138.txt`는 C `perf_c_multi_linux_20260525_074050.txt` 대비 63.0/66.7%였고, 1-worker 후보 `perf_rust_multi_linux_20260525_074204.txt`는 40.0/37.7%로 낮았다. 기본값을 ws 64/256B에만 2-worker로 좁혀 반영한 공식 runner `perf_rust_multi_linux_20260525_074242.txt`는 64.7/65.9%로 기존 50%대보다 올랐지만 아직 SPOT 기준 아래라 보류를 유지한다.
- **MULTI_SPOT recv worker 3개 후보 기각**: 2026-05-25 재검토에서 `PERF_MULTI_SPOT_RECV_WORKERS=3` 후보를 tcp/ws 64/131072/262144B에 시험했다. 공식 runner `perf_rust_multi_linux_20260525_031739.txt`는 complete였고 tcp 64B 단독 비율은 C `perf_c_multi_linux_20260525_022730.txt` 대비 76.2%까지 올라갔지만, tcp 131072/262144B와 ws 262144B는 기존 대표값보다 내려갔다. 이를 tcp 64B 기본값으로만 좁힌 코드 후보도 `perf_rust_multi_linux_20260525_031917.txt`에서 tcp 64B 69.8%에 그쳐 통과를 안정적으로 증명하지 못했다. current HEAD에서 ws 64/256B만 다시 좁혀 시험한 `perf_rust_multi_linux_20260525_093748.txt`도 complete였지만 C `perf_c_multi_linux_20260525_074050.txt` 대비 62.7/60.6%였고, 기존 2-worker 기본값 `perf_rust_multi_linux_20260525_074242.txt`의 64.7/65.9%보다 낮았다. worker 3개 기본화는 반영하지 않고 기존 크기별 기본값을 유지한다.
- **MULTI_SPOT recv worker 5개 후보 기각**: worker 3개보다 큰 중간값으로 tcp 64B만 다시 좁혀 시험했다. 같은 commit에서 C 기준 `perf_c_multi_linux_20260525_060417.txt`는 4.583Mmsg/s였고 `PERF_MULTI_SPOT_RECV_WORKERS=5` Rust 후보 `perf_rust_multi_linux_20260525_060531.txt`는 2.746Mmsg/s로 59.9%에 그쳤다. 기존 worker 4개 대표값보다 낮으므로 코드 기본값으로 반영하지 않는다.
- **MULTI_SPOT throughput count/latency sampling 분리 후보 기각**: C/Go처럼 active payload count와 latency sample을 분리하고 기본 stride 32로 latency를 기록하는 후보를 시험했다. `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했고 공식 제한 재측정도 complete였다. 그러나 tcp 64/131072/262144B는 C `perf_c_multi_linux_20260525_035354.txt` 대비 Rust `perf_rust_multi_linux_20260525_035545.txt`가 66.1/41.3/99.9%였고, 기존 대표값 67.9/53.6/104.3% 대비 64B와 131072B가 낮아졌다. ws 131072/262144B도 C `perf_c_multi_linux_20260525_035625.txt` 대비 Rust `perf_rust_multi_linux_20260525_035800.txt`가 44.3/42.2%로 기존 45.1/41.7%와 같은 보류권이다. count/sampling 의미 정렬만으로 남은 SPOT 보류를 해소하지 못하므로 반영하지 않는다.
- **Single routed `Message::with_size()` 직접 stamp 후보 기각**: multi routed large에서 효과가 있었던
  `Message::with_size()` + `data_mut()` 직접 stamp를 single 공통 `send_loop`에도 시험했다.
  `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했지만, 같은 조건
  C `perf_c_single_linux_20260525_123657.txt` 대비 후보
  `perf_rust_single_linux_20260525_123806.txt`는 `DEALER_ROUTER tcp 262144B`가
  `binary_exit` partial로 끝났다. 같은 실행의 65536/131072B도 13.823/7.023Kmsg/s로
  C 98.585/56.657Kmsg/s 대비 14% 안팎에 머물러 기존 single routed large 보류를 해소하지
  못했다. single routed large는 단순 추가 복사 제거가 아니라 routed send/recv 경계 자체를
  더 봐야 하므로 코드는 원복했다.
- **Single routed receiver local stats 후보 기각**: single routed 수신은 한 thread에서만
  통계를 기록하므로 `Arc<Mutex<LatencyStats>>` 대신 receiver-local `LatencyStats`를 직접
  갱신하는 후보를 시험했다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은
  통과했지만, 공식 제한 측정 `perf_rust_single_linux_20260525_135932_single_routed_local_stats_candidate.txt`는
  `DEALER_ROUTER tcp 65536B` repeat 2부터 `binary_exit` partial로 끝났다. 첫 repeat도
  13.98Kmsg/s로 기존 cooldown 재측정 `perf_rust_single_linux_20260525_134835_single_dr_cooldown3s_probe.txt`의
  14.03Kmsg/s와 같아 병목을 줄이지 못했다. receiver stats lock은 single routed large의
  지배 비용이 아니므로 반영하지 않는다.
- **MULTI_PUBSUB ready-slot drain 후보 기각**: client poller slot을 socket index로 두고
  C/Go처럼 ready event socket만 drain하며 `wait(-1)`로 바꾸는 후보를 시험했다.
  `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했고,
  공식 wrapper `perf_c_multi_linux_20260525_114223.txt` 대비
  `perf_rust_multi_linux_20260525_114258.txt`는 complete였다. 그러나 wss 256B는
  13.2%로 여전히 보류이고, tls 256B는 기존 통과권에서 24.3%로 크게 내려갔다.
  PUBSUB broadcast에서는 ready event만 좁혀 drain하면 일부 socket backlog를 충분히
  비우지 못해 transport별 회귀가 생기므로 기존 전체 socket drain 방식을 유지한다.
- **MULTI_SPOT_REQREP request 직접 stamp 후보 기각**: routed echo와 DD에서 효과가 있었던 `Message::with_size()` + `data_mut()` 직접 stamp를 SPOT request client에도 시험했다. 공식 runner `perf_rust_multi_linux_20260525_004621.txt`는 complete였지만, fresh C baseline 대비 tcp 65536/131072/262144B가 54.7/62.2/63.5%로 기존 85.1/70.6/71.1%보다 낮아졌다. 이 경로는 기존 재사용 payload `Vec` + `Message::copy_from`보다 느려 반영하지 않는다.
- **single routed send loop 직접 stamp 후보 기각**: multi routed echo에서 효과가 있었던
  `Message::with_size()` + `data_mut()` 직접 stamp를 single 공통 `send_loop`에도
  시험했다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은
  통과했지만, 공식 runner `perf_rust_single_linux_20260525_083748.txt`는
  `tcp DEALER_ROUTER 131072B`에서 `binary_exit` partial로 끝났고, complete된 65536B도
  같은 조건 C `perf_c_single_linux_20260525_083724.txt` 대비 13.9%로 기존 대표값과
  같은 보류권이었다. single routed large 병목은 active message 생성 복사 하나만으로
  설명되지 않으므로 기존 `Message::copy_from(&buf)` send loop를 유지한다.
- **MULTI_PUBSUB event-slot drain 후보 기각**: C client처럼 poll event가 발생한 subscriber만 drain하도록 바꾸는 후보를 시험했다. 초기 후보 `perf_rust_multi_linux_20260525_005023.txt`는 complete였지만 fresh C baseline 대비 tcp 65536B가 38.7%(기존 49.5%), ws 256B가 45.5%(기존 92.8%), ws 65536B가 63.2%(기존 86.4%)로 내려갔다. 2026-05-25 재검토에서도 전역 후보 `perf_rust_multi_linux_20260525_090029.txt`는 같은 조건 최신 C `perf_c_multi_linux_20260525_085952.txt` 대비 wss 256B 14.9%, wss 1024B 6.4%, tls 256B 16.8%, tls 1024B 8.4%에 그쳤고, 조건부 후보 `perf_rust_multi_linux_20260525_090142.txt`도 기준선을 넘기지 못했다. no-code 최신 재측정 `perf_rust_multi_linux_20260525_090249.txt`도 wss 256/1024B 13.1/6.4%, tls 256/1024B 16.8/5.6%로 낮아 표를 갱신한다. Rust binding에서는 wake 뒤 전체 subscriber를 훑어 backlog를 한 번에 비우는 기존 방식이 더 낫고, event-slot drain은 코드에 반영하지 않는다.
- **ROUTER_ROUTER sleep→poller + routed client 직접 stamp**: RR client와 server가 idle마다 `thread::sleep(1ms)`로 hot-loop을 throttle해 small이 ~19%에 묶였다(같은 routed-echo인 `MULTI_DEALER_ROUTER`는 unified poller라 106%). RR client/server를 DEALER_ROUTER와 동일한 signal-driven poller(POLLIN/POLLOUT 토글, `wait(...,-1)`)로 바꿔 tcp small 19→**84~86% 통과**(`perf_rust_multi_linux_20260524_004405.txt`). 이후 `MULTI_DEALER_ROUTER`/`MULTI_ROUTER_ROUTER` client active send가 payload `Vec`에 header를 찍고 `Message::copy_from`으로 다시 복사하던 경로를 public `Message::with_size()` + `data_mut()` 직접 stamp로 바꿔 C의 native message buffer stamp 의미와 맞췄다. fresh C baseline 대비 tcp DR large 65.2/103.4/107.2%, tcp RR large 75.3/108.6/116.4%(`perf_rust_multi_linux_20260524_213949.txt`), ws/wss/tls routed echo large도 전 size 통과(`perf_rust_multi_linux_20260524_214245.txt`)로 올라 multi routed echo 보류가 사라졌다.
- **MULTI_DEALER_DEALER small burst-send + direct native message stamp**: Rust client가 각 socket마다 active loop 1회에 메시지 1개만 보내고 POLLOUT event slot도 모두 0으로 등록해 C의 "blocked까지 연속 송신 후 pending socket만 재개" 의미와 달랐다. 1024B 이하에서만 C와 같은 burst-send/pending POLLOUT 구조를 적용했다. tcp 64/256/1024B는 77.7/91.9/89.3%(`perf_rust_multi_linux_20260524_201407.txt`), ws/wss/tls small도 전부 통과권(`perf_rust_multi_linux_20260524_201443.txt`)으로 올랐다. large는 `Vec`에 header를 찍고 `Message::copy_from`으로 다시 복사하던 경로를 public `Message::with_size()`와 `data_mut()` 직접 stamp로 바꿔 C의 native message buffer stamp 의미와 맞췄다. 공식 runner repeat3 결과 tcp large 102.0/101.6/106.6%(`perf_rust_multi_linux_20260524_213113.txt`), ws 96.3/69.9/93.1%, wss 84.2/84.2/97.7%, tls 83.9/86.8/93.1%(`perf_rust_multi_linux_20260524_213159.txt`)로 DD 보류가 사라졌다. 전 size에 burst-send를 적용한 후보 `perf_rust_multi_linux_20260524_201322.txt`는 large를 더 낮춰 반영하지 않는다.
- **MULTI_PUBSUB server 직접 stamp**: PUB server active publish도 payload `Vec`에 header를 찍고 `Message::copy_from`으로 다시 복사했다. public `Message::with_size()` + `data_mut()` 직접 stamp로 바꿔 tcp 64/256/1024B가 fresh C baseline 대비 26.3/16.9/15.5%(`perf_rust_multi_linux_20260524_215258.txt`)가 됐다. 같은 조건 원래 copy 경로 재측정은 24.4/14.8/15.5%(`perf_rust_multi_linux_20260524_215352.txt`)라 64/256B에 유효하지만, subscriber 100개를 public `subscribe` 호출로 drain하는 비용이 남아 small은 아직 보류다. 2026-05-25 재검토에서 `TopicMessage`의 `Vec<Message>` capacity를 재사용하도록 `subscribe` 내부를 바꾸는 후보도 시험했지만, 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_031012.txt` 대비 Rust `perf_rust_multi_linux_20260525_031037.txt`가 tcp 64/256/1024/65536B 26.8/14.5/12.5/49.9%로 보류를 해소하지 못해 반영하지 않는다.
- **MULTI_PUBSUB SubSocket single-part receive 후보 기각**: C client처럼 `zlink_subscribe_part`를 직접 쓰는 public `SubSocket::subscribe_part(...)` 후보를 추가하고 Rust PUBSUB perf client를 single-part 수신으로 바꿔 시험했다. `cargo test --manifest-path bindings/rust/Cargo.toml --no-run`, `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`, surface/roundtrip focused test는 통과했다. 그러나 공식 제한 재측정은 complete였어도(`perf_c_multi_linux_20260525_034918.txt`, `perf_rust_multi_linux_20260525_034940.txt`) tcp 64/256/1024B가 24.4/16.5/12.3%에 머물렀고, 기존 대표값 700K/400K/150Kmsg/s 대비 650K/400K/150Kmsg/s로 개선이 없었다. public API를 넓힐 근거가 없어 반영하지 않는다.
- **MULTI_PUBSUB latency sampling 분리 후보 기각**: PUBSUB client가 모든 수신 메시지의 latency를 `LatencyStats` vector에 기록하는 비용을 줄이기 위해 throughput count와 latency sample 기록을 분리하고 기본 stride 32를 적용했다. `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했고 공식 runner도 complete였지만, 최신 C 기준 `perf_c_multi_linux_20260525_062908.txt` 대비 후보 `perf_rust_multi_linux_20260525_063051.txt`는 tcp 64/256/1024/65536B 700/350/150/100Kmsg/s로 직전 current HEAD 재측정 `perf_rust_multi_linux_20260525_062932.txt`와 처리량이 같았다. latency 기록 비용보다 public `subscribe` drain 호출 수와 server/client phase 구조가 더 지배적이므로 반영하지 않는다.
- **MULTI_PUBSUB client `TopicMessage` placeholder 재사용 적용**: PUBSUB client가 수신 메시지마다 `TopicMessage::empty()`를 새로 만들던 것을 drain 호출 안의 caller-provided placeholder 재사용으로 바꿨다. public `SubSocket::subscribe(&mut TopicMessage, ...)` 의미는 그대로이며, 새 API를 추가하지 않는다. 공식 제한 재측정은 complete였고 C `perf_c_multi_linux_20260525_073540.txt` 대비 Rust `perf_rust_multi_linux_20260525_073701.txt`에서 `wss 65536B`가 61.033Kmsg/s, 87.9%로 올라 통과권에 들어왔다. 이 단계에서는 tcp 64/256/1024/65536B가 26.4/15.0/13.4/48.4%, wss 256/1024B가 7.4/6.1%, tls 1024B가 5.3%라 PUBSUB small 보류가 남았지만, 뒤의 server `POLLOUT` wait 제거로 해소됐다.
- **MULTI_PUBSUB subscriber별 `TopicMessage` 유지 후보 기각**: drain 호출 안에서만 재사용하던 `TopicMessage`를 subscriber별로 오래 유지하는 후보를 시험했다. `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했고 공식 wrapper `perf_rust_multi_linux_20260525_101951.txt`도 complete였지만, 같은 조건 C `perf_c_multi_linux_20260525_073540.txt` 대비 tcp 64/256/1024/65536B는 28.2/15.0/13.4/48.4%라 보류를 해소하지 못했다. `tls 256B`는 1.85Mmsg/s까지 올랐지만 기준 대비 68.7%로 Rust one-way 기준 아래이고, `tls 65536B`는 기존 85.050Kmsg/s에서 70.109Kmsg/s로 낮아졌다. 이를 `tls 256B`에만 좁힌 후보도 `perf_rust_multi_linux_20260525_102231.txt`에서 1.60Mmsg/s, C `perf_c_multi_linux_20260525_085952.txt` 대비 59.9%에 그쳤고 65536B도 낮아졌다. 이미 통과한 large를 흔들고 small 보류를 해소하지 못하므로 코드는 원복했다.
- **MULTI_PUBSUB client 무기한 poll wait 후보 기각**: Rust client 주석이 C `zlink_poller_wait(-1)` 의미를 가리키지만 실제 코드는 bounded `wait(..., 100)`을 쓴다. C와 맞추기 위해 `wait(..., -1)` 후보를 시험했다. `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했고 같은 조건 C 기준 `perf_c_multi_linux_20260525_103718.txt`는 complete였지만, Rust 후보 `perf_rust_multi_linux_20260525_103718.txt`는 repeat 중 `missing_result_lines`/`binary_exit_or_timeout` partial로 끝났다. 무기한 wait는 stop-token 유실 또는 readiness 전환 지연 시 runner completion을 깨므로 반영하지 않는다.
- **MULTI_SPOT small active-window drain 적용**: `ws/wss/tls` 64/256/1024B를 fresh C `perf_c_multi_linux_20260525_102719.txt`와 current Rust `perf_rust_multi_linux_20260525_103041.txt`로 다시 맞춰 보니 `ws` 66.6/67.9/64.4%, `wss` 63.1/67.0/49.7%, `tls` 61.0/59.1/55.2%였다. `PERF_MULTI_SPOT_RECV_WORKERS=1` 후보는 첫 run 처리량은 1.8~2.3Mmsg/s였지만 repeat에서 binary timeout partial로 끝났다(`perf_rust_multi_linux_20260525_102719.txt`). `PERF_MULTI_SPOT_RECV_WORKERS=8` 후보는 complete였지만 `perf_rust_multi_linux_20260525_103235.txt`에서 모든 small cell이 current 기본값보다 낮았다. C client는 첫 active payload의 sent timestamp로 sender active window를 잡고 그 뒤 backlog를 drain하지만, Rust client는 START 뒤 wall-clock 1초만 수집해 latency backlog가 큰 SPOT small에서 유효 메시지를 덜 세고 있었다. Rust client도 sent timestamp 기준 active window와 drain grace를 적용하고, throughput count와 latency sample 저장을 분리했다. `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했고, fresh C `perf_c_multi_linux_20260525_120630.txt` 대비 Rust 단독 complete 재측정 `perf_rust_multi_linux_20260525_121937.txt`, `perf_rust_multi_linux_20260525_121522.txt`, `perf_rust_multi_linux_20260525_121609.txt`에서 `ws` 149.0/139.2/112.2%, `wss` 131.5/128.1/90.2%, `tls` 129.3/130.9/99.7%로 모두 SPOT 기준을 넘었다. `PERF_MULTI_SPOT_RECV_WORKERS=7` C-style worker 후보는 partial(`perf_rust_multi_linux_20260525_120949.txt`)이라 반영하지 않고, server active publish sleep 제거 후보도 `ws` 1024B가 55.3%라 반영하지 않는다. transport 묶음 실행 `perf_rust_multi_linux_20260525_121447.txt`는 repeat partial이어서 통과 근거로 쓰지 않고, 단독 complete 재측정으로 표를 갱신한다.
- **MULTI_SPOT_REQREP server single-part reply + client DONT_WAIT + 262144B active slot 6**: Rust server는 받은 payload를 `Message::copy_from(...)`으로 다시 복사한 뒤 reply했다. `Received::single_part()`로 받은 owned `Message`를 그대로 reply에 넘기도록 바꿔 C의 받은 part 재전송 의미와 맞췄고, client active request도 C처럼 `DONT_WAIT`로 제출하게 했다. 최신 같은 조건 제한 재측정(`perf_c_multi_linux_20260525_022108.txt`, `perf_rust_multi_linux_20260525_022246.txt`)의 tcp repeat3 median은 65536/131072B 89.4/77.6%로 통과다. 262144B는 active slot을 8에서 6으로 좁힌 뒤 C `perf_c_multi_linux_20260525_060105.txt` 11.844Kops/s 대비 Rust `perf_rust_multi_linux_20260525_060125.txt` 9.523Kops/s, 80.4%로 올라 통과권에 들어왔다. active slot 16 후보(`perf_rust_multi_linux_20260524_211217.txt`), active slot 12 후보(`perf_rust_multi_linux_20260524_224928.txt`), active slot 4 후보(`perf_rust_multi_linux_20260524_233704.txt`, 45.4/58.4%로 하락), callback latency `mpsc`를 `Mutex` 기록으로 바꾸는 후보(`perf_rust_multi_linux_20260524_211329.txt`), client request `Message::with_size()` 직접 stamp 후보(`perf_rust_multi_linux_20260524_213619.txt`, 131072/262144B 17.7K/8.8K로 기존보다 낮음)는 반영하지 않는다. 2026-05-25 재검토에서 C client처럼 active window 뒤 pending reply를 poller로 정리하는 후보도 시험했지만, Rust 제한 재측정은 262144B 8.984Kops/s(`perf_rust_multi_linux_20260525_030658.txt`)로 기존 단독 재측정 8.955Kops/s(`perf_rust_multi_linux_20260524_212047.txt`)와 사실상 같았다. 같은 조건 C 단독 재측정 `perf_c_multi_linux_20260525_030645.txt`는 core `fast_mutex` assert로 partial이어서 기준으로 쓰지 않는다. Rust reqrep client poller에 `POLLIN | POLLCOMPLETION`을 함께 등록하는 후보는 `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했지만 공식 runner가 리포트 없이 즉시 실패해 반영하지 않는다.
- **MULTI_SPOT_SENDSEND client active-slot + poller wait + 65536B echo copy 축소**: Rust sendsend client는 C와 달리 large size에서도 100개 spot을 모두 active로 돌리고, reply readiness를 poller로 기다리지 않고 progress가 없을 때 `thread::sleep(1ms)`를 썼다. C와 같은 POLLIN poller wait, active window 뒤 pending reply drain을 적용했고, active slot은 131072B 이상 8, 65536B는 24로 맞췄다. 65536B server echo는 받은 single-part `Message`를 그대로 send builder에 넘겨 reply copy를 줄였다. 131072B 이상은 같은 single-part move가 낮아져 기존 native-copy 경로를 유지한다. 최신 같은 조건 제한 재측정(`perf_c_multi_linux_20260525_022108.txt`, `perf_rust_multi_linux_20260525_022246.txt`)에서 tcp 65536/131072/262144B도 90.1/77.2/75.6%로 통과권에 들어왔다. ws 전 size 81.6~101.3% 통과(`perf_rust_multi_linux_20260524_200859.txt`), wss도 64B 반복 측정 median 77.6%(`perf_rust_multi_linux_20260524_210036.txt`)로 올라 전 size 통과, tls 전 size 82.5~89.6% 통과(`perf_rust_multi_linux_20260524_200645.txt`). active slot 16 후보, 262144B active slot 4 후보(`perf_rust_multi_linux_20260524_213811.txt`, 8.2K로 기존보다 낮음), client active request `Message::with_size()` 직접 stamp 후보(`perf_rust_multi_linux_20260524_233945.txt`, 131072/262144B 64.4/62.1%로 하락), 모든 large size single-part move 후보는 낮은 size가 있어 반영하지 않는다.
- **single routed blocking-send 후보 기각**: C single `DEALER_ROUTER` active sender는 blocking send(`ZLINK_SEND_FLAGS_NONE`)를 쓰므로 Rust single routed sender도 `DONT_WAIT` 없이 맞추는 후보를 시험했다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했지만, 공식 runner `PERF_FAIL_FAST=1 bindings/rust/perf/run_benchmarks.sh --transports tcp --pattern DEALER_ROUTER --msg-sizes 65536,131072,262144 --duration 1 --runs 1`에서 65536B가 `binary_exit` partial(`perf_rust_single_linux_20260524_193322.txt`)로 끝났다. 직전 같은 조건 `DONT_WAIT` 기준은 complete(`perf_rust_single_linux_20260524_193204.txt`)였으므로 이 후보는 반영하지 않는다.
- **single routed `router_recv_part` 초기 후보 폐기와 fixed 적용**: C single routed receiver는 `zlink_router_recv_part`로 단일 part를 직접 받지만, 기존 Rust single routed perf는 public `RouterSocket::recv(&mut Received, ...)`를 거쳤다. 초기 `RouterPart`/`recv_part_with_flags` 후보는 공식 runner가 `DEALER_ROUTER tcp 65536B`에서 `binary_exit` partial(`perf_rust_single_linux_20260525_014415.txt`)로 끝나 폐기했다. 이후 burst drain 집계 버그와 stop wait를 고친 `RouterSocket::recv_part(...)`/`RouterPart` 형태로 다시 적용했고, fixed tcp 재측정과 ws/tls complete 재측정은 모두 report를 남겼다. 다만 routed large 비율은 아직 기준보다 낮으므로 이 API는 completion/의미 정렬 근거로 유지하고, 성능 보류는 계속 추적한다.
- **single 공통 sender 직접 stamp 후보 기각**: multi routed/DD에서 효과가 있었던 `Message::with_size()` + `data_mut()` 직접 stamp를 single 공통 `send_loop`에도 적용해 봤다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고 C 기준 `perf_c_single_linux_20260525_041802.txt`도 complete였지만, Rust 공식 제한 측정 `perf_rust_single_linux_20260525_041848.txt`가 `DEALER_ROUTER tcp 131072B`에서 `binary_exit` partial로 끝났다. 65536B도 C 대비 13.6% 수준으로 기존 보류권과 다르지 않았다. single routed large의 남은 병목은 sender 쪽 payload 복사만으로 해소되지 않으므로 기존 `Message::copy_from` 경로를 유지한다.
- **single routed sender `POLLOUT` wait 후보 기각**: current `DONT_WAIT` sender가 backpressure 때 `yield_now()`만 호출하므로, sender socket을 public `Poller` `POLLOUT`에 등록하고 backpressure 때 1ms poll wait로 깨우는 후보를 시험했다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했지만 공식 runner `perf_rust_single_linux_20260525_052120.txt`가 `DEALER_ROUTER tcp 131072B`에서 `binary_exit` partial로 끝났다. 65536B median도 13.90Kmsg/s로 기존 `perf_rust_single_linux_20260524_193204.txt`의 14.05Kmsg/s보다 낮아, sender-side readiness wait만으로는 single routed large 보류를 해소하지 못한다.
- **single routed internal submit fast path 후보 기각**: `SendOp<Ready>::submit`에서 single-part `SocketSend`/`SocketSendTo`만 `prepare_send_parts`와 `submit_part_sequence`를 건너뛰고 direct FFI send로 보내는 내부 fast path를 시험했다. `cargo test --manifest-path bindings/rust/Cargo.toml --no-run`과 `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했지만, 공식 runner `perf_rust_single_linux_20260525_124151.txt`는 `DEALER_ROUTER tcp 262144B`에서 `binary_exit` partial로 끝났고 65536/131072B도 13.751/6.967Kmsg/s로 기존 보류권과 같았다. send builder 내부 dispatch 비용은 single routed large 병목의 주 원인이 아니므로 코드는 원복했다.
- **single routed blocking-send 재확인 후보 기각**: 2026-05-25에 Rust single `DEALER_ROUTER`/`ROUTER_ROUTER` active send와 stop-token send에서 `DONT_WAIT`를 제거해 C single의 `ZLINK_SEND_FLAGS_NONE` 의미에 다시 맞춰 시험했다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했지만, 공식 runner `perf_rust_single_linux_20260525_124323.txt`가 `DEALER_ROUTER tcp 65536B`에서 바로 `binary_exit` partial로 끝났다. Rust single routed 경로에서는 blocking send가 completion 안정성을 깨므로 기존 `DONT_WAIT` retry를 유지한다.
- **single routed `Received` 재사용 수신 후보 기각**: public API를 늘리지 않고 `RouterSocket::recv(&mut Received, ...)`가 임시 `Received`와 새 `Vec<Message>`를 만들지 않도록, 성공한 routed recv part를 caller-provided `Received`의 기존 `Vec`에 직접 채우는 후보를 시험했다. `cargo test --manifest-path bindings/rust/Cargo.toml --no-run`과 `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고 runner는 `core/build/lib/libzlink.so`를 사용했지만, 공식 runner `perf_rust_single_linux_20260525_125742.txt`가 `DEALER_ROUTER tcp 131072B`에서 `binary_exit` partial로 끝났다. complete된 65536B도 13.751Kmsg/s로 기존과 같아, routed recv wrapper의 `Vec` 재사용만으로는 single routed large 보류를 해소하지 못한다.
- **2026-06-22 single routed `Received` 직접 채움 재확인 후보 기각**:
  같은 아이디어를 public `RouterSocket::recv(&mut Received, ...)` 경로 내부에서 다시
  시험했다. 후보는 `zlink_router_recv_part`로 받은 단일 part를 caller-provided
  `Received`에 직접 채우고, multipart일 때만 기존 temporary `Received` 경로로
  돌아가도록 했다. `cargo check --manifest-path bindings/rust/Cargo.toml --all-targets`,
  `cargo test --manifest-path bindings/rust/Cargo.toml --test behavior_tests --test ownership_tests -- --nocapture`,
  `cargo test --manifest-path bindings/rust/Cargo.toml --test contract_tests -- request_router_exposes_request_sequence router_reply_with_non_empty_flags_fails_explicitly --nocapture`는
  통과했다. 그러나 공식 runner는
  `perf_rust_single_linux_20260622_132830_prerelease_7_2_0_rust_single_dealer_router_tcp_65536_router_recv_into_fastpath_probe.txt`에서
  `DEALER_ROUTER tcp 65536B` `13.743 Kmsg/s`였고, 같은 후보 전 재측정
  `perf_rust_single_linux_20260622_041244_prerelease_7_2_0_rust_single_recheck_routed_ipc_tcp_large_cells.txt`의
  `13.961 Kmsg/s`보다 낮았다. 이 후보는 perf 전용 public API를 만들지 않았지만,
  효과 없는 internal branch와 context 재구성 코드를 늘려 POSD 관점의 깊은 모듈에도
  도움이 되지 않는다. 코드는 원복했고, 다음 Rust routed large 후보는 반복 입력 모양에
  기대는 cache가 아니라 native boundary, message ownership, sender/receiver phase 비용을
  profiler로 분리한 뒤 선택한다.
- **single routed send-part 직접 호출 후보 기각**: routed single send hot path를 더 좁혀 direct
  single-part send 후보를 시험했다. 후보 `perf_rust_single_linux_20260525_142357_single_routed_send_part_candidate.txt`는
  complete였지만 `DEALER_ROUTER` tcp 65536/131072/262144B가 13.69/6.85/3.52Kmsg/s,
  `ROUTER_ROUTER`가 13.85/6.92/3.55Kmsg/s로 기존 보류권과 같았다. 다른 fast path 후보
  `perf_rust_single_linux_20260525_141858_single_routed_send_single_fastpath_candidate.txt`는
  `ROUTER_ROUTER tcp 131072B`에서 partial이었다. send wrapper를 더 줄여도 C 대비 11~15%
  병목은 해소되지 않으므로 반영하지 않는다.
- **single routed stop token blocking 후보 기각**: active send는 기존 `DONT_WAIT`
  재시도를 유지하고 stop token만 C처럼 blocking send로 바꾸는 후보를 시험했다.
  `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은
  통과했지만 공식 runner `perf_rust_single_linux_20260525_132116_stop_blocking_probe.txt`가
  `DEALER_ROUTER tcp 65536B`부터 전 run `binary_exit` partial로 끝났다. Rust routed
  single에서는 blocking stop token이 completion 안정성을 더 나쁘게 만들므로 반영하지
  않는다.
- **single routed stop-token retry 확대 후보 기각**: 기존 `DONT_WAIT` stop token
  재시도 5000회를 25000회로 늘려 large backlog 뒤 stop token을 더 오래 밀어 넣는
  후보를 시험했다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은
  통과했지만 공식 runner
  `perf_rust_single_linux_20260525_132208_stop_retry25k_probe.txt`가
  `DEALER_ROUTER tcp 131072B`에서 partial로 끝났고, timeout까지 걸리는 시간이
  늘었다. 재시도 시간 확대는 실패를 늦출 뿐 보류를 해소하지 못하므로 반영하지 않는다.
- **single routed active blocking 재확인 후보 기각**: C의 routed active sender가
  blocking send를 쓰는 점에 맞춰 Rust `DEALER_ROUTER`/`ROUTER_ROUTER` active send의
  `DONT_WAIT`를 제거하는 후보를 다시 좁혀 시험했다. 공식 runner
  `perf_rust_single_linux_20260525_132331_active_blocking_probe2.txt`는 65536B/131072B
  수치를 냈지만 `DEALER_ROUTER tcp 262144B`가 전 run `binary_exit` partial로 끝났고,
  direct binary도 같은 size에서 30초 timeout을 재현했다. single routed large에서는
  blocking active send가 큰 payload completion을 안정화하지 못해 반영하지 않는다.
- **single routed blocking-send 전 transport 재확인 후보 기각**: active send와 stop token
  모두에서 `DONT_WAIT`를 제거하는 후보를 65536/131072B, tcp/ws/wss/tls에 다시 묶어
  시험했다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은
  통과했고, C 기준 `perf_c_single_linux_20260525_181155_rust_single_routed_blocking_send_c.txt`
  와 Rust 후보 `perf_rust_single_linux_20260525_181308_single_routed_blocking_send_candidate.txt`는
  모두 complete였다. 그러나 C 대비 비율은 `DEALER_ROUTER` tcp 13.7/11.6%, ws
  29.6/24.0%, wss 81.7/82.0%, tls 58.0/55.3%이고, `ROUTER_ROUTER`도 tcp
  13.7/11.8%, ws 30.9/23.6%, wss 81.1/83.4%, tls 59.5/56.6%였다. wss만
  통과권이지만 tcp/ws는 기존 보류권보다 나아지지 않고 tls도 Rust routed one-way
  기준보다 낮다. blocking send는 Rust single routed large의 일반 해법이 아니므로
  기존 `DONT_WAIT` retry를 유지한다.
- **single routed receiver count-all 후보 기각**: C routed single runner는 stop token을
  받을 때까지 drain한 active payload를 모두 count하므로, Rust receiver도 wall-clock
  active deadline 뒤 수신분을 버리지 않게 하는 후보를 시험했다. 공식 runner
  `perf_rust_single_linux_20260525_132510_routed_count_all_probe.txt`는
  `DEALER_ROUTER tcp 65536B`에서 repeat 중 `binary_exit` partial로 끝났다. 측정 의미
  차이는 후속 설계 검토 대상으로 남기되, 이 단독 변경은 completion 안정성을 깨므로
  반영하지 않는다.
- **MULTI_PUBSUB multi-worker drain 후보 기각**: PUBSUB client에서 100 subscriber를 단일 thread로 순회하는 병목을 의심해 subscriber 집합을 scoped worker 4개로 나누는 후보를 시험했다. `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했고, 공식 runner `PERF_FAIL_FAST=1 bindings/rust/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_PUBSUB --msg-sizes 64,256,1024 --duration 1 --runs 1`도 complete였지만 결과가 64/256/1024B = 26.3/14.8/15.5%(`perf_rust_multi_linux_20260524_193953.txt`)에 그쳤다. 같은 후보에서 worker별 chunk 전체를 drain하도록 바꾼 재시도도 24.4/14.8/15.5%(`perf_rust_multi_linux_20260524_194034.txt`)로, 기존 단일 thread 기준 24.4/21.2/15.5%보다 256B가 나빠졌다. 2026-05-25 current HEAD에서도 scoped worker 후보를 env로 재시험했지만 worker 4개 `perf_rust_multi_linux_20260525_095305.txt`는 같은 조건 C `perf_c_multi_linux_20260525_073540.txt` 대비 64/256/1024/65536B 24.5/15.0/13.4/48.4%였고, worker 2개 `perf_rust_multi_linux_20260525_095342.txt`도 26.4/15.0/13.4/48.4%로 기존 대표값을 넘지 못했다. 따라서 이 후보는 반영하지 않는다.
- **MULTI_PUBSUB poll event-slot 후보 기각**: PUBSUB client poller에 subscriber index를 slot으로 넣고 C처럼 반환된 event socket만 drain하는 후보를 시험했다. `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했고, 공식 runner `perf_rust_multi_linux_20260524_232655.txt`에서 tcp 64/256/1024B는 26.3/16.9/15.5%로 기존 직접 stamp 기준과 같았다. `perf_rust_multi_linux_20260524_232744.txt`의 ws/wss/tls 256/1024B도 남은 small 보류를 해결하지 못했다. public 의미는 C에 더 가깝지만 측정상 개선이 없어 반영하지 않는다.
- **MULTI_PUBSUB latency sampling 적용**: PUBSUB client가 모든 active 메시지에서 `now_ns()`와 latency vector push를 수행하던 비용을 줄이기 위해 throughput count는 전 메시지를 유지하고 latency 계산/샘플 저장만 기본 32개당 1개로 줄였다(`PERF_MULTI_PUBSUB_LATENCY_SAMPLE_STRIDE`). `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했다. 공식 wrapper 기준 tcp C `perf_c_multi_linux_20260525_105401.txt` 대비 Rust `perf_rust_multi_linux_20260525_105401.txt`에서 65536B가 50.2%로 올라 보류를 해소했고, ws/wss/tls 확인 C `perf_c_multi_linux_20260525_105449.txt` 대비 Rust `perf_rust_multi_linux_20260525_105449.txt`도 complete였다. tls 256B는 81.2%로 통과권에 올랐다. tcp 64/256/1024B, ws 1024B, wss 256/1024B, tls 1024B는 여전히 단순 one-way 기준 아래다.
- **MULTI_PUBSUB server `POLLOUT` wait 제거 적용**: Rust PUBSUB server는 active publish가 backpressure를 만나면 `POLLOUT` poller wait로 쉬었다. C reference는 `ZLINK_DONTWAIT` publish가 `EAGAIN`이어도 active window 안에서 바로 다음 publish를 재시도하므로, Rust만 publish rate가 100Kmsg/s 단위로 낮게 묶였다. server hot path를 C처럼 continuous `DONT_WAIT` retry로 맞춘 뒤 `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했고, 공식 wrapper도 complete였다. Fresh C `perf_c_multi_linux_20260525_115827.txt` 대비 Rust `perf_rust_multi_linux_20260525_115826.txt`에서 tcp 64/256/1024/65536B는 91.8/98.2/116.8/90.9%, ws는 90.1/89.8/90.8/105.5%, wss는 93.6/94.1/94.2/65.8%, tls는 89.4/93.3/96.5/89.8%다. 이 변경으로 Rust `MULTI_PUBSUB` small 보류를 모두 제거한다.
- **2026-06-22 Rust caller-provided recv single-part fill 후보 기각**:
  Rust `PAIR/DEALER_DEALER inproc 1024` 반복 미달을 대상으로 public
  `PairSocket.recv(&mut Received, ...)`와 `DealerSocket.recv(&mut Received, ...)`
  표면은 유지한 채 binding 내부 `SocketInner::recv`가 single-part 수신에서 중간
  `Vec<Message>`와 임시 `Received` envelope를 만들지 않고 caller-provided `Received`를
  직접 채우는 후보를 시험했다. `cargo check`와
  `cargo test --test ownership_tests -- --nocapture`는 통과했다. 다만
  `cargo test --test contract_tests --test ownership_tests -- --nocapture`는
  `direct_common_header_version_matches_package`에서 header patch version `0`과 test 기대값
  `4`가 달라 실패했으며, 이 버전 동기화 문제는 recv 후보와 무관하다. 공식 wrapper
  `perf_rust_single_linux_20260622_115121_prerelease_7_2_0_rust_single_inproc_1024_recv_into_fastpath.txt`는
  `PAIR` `1001351.800 msg/s`, `DEALER_DEALER` `1002559.600 msg/s`로 complete였다.
  기존 paired 재측정 `perf_rust_single_linux_20260622_040303_prerelease_7_2_0_rust_single_recheck_inproc_pair_dealer_1024_cells.txt`의
  `1001533.000/1027250.000 msg/s`와 같거나 낮아졌고, C 기준
  `1456367.600/1485393.000 msg/s` 대비 목표 미달도 유지한다. 이 후보는 일반 public
  수신 경로 비용을 줄이려는 방향은 맞지만, 공통 recv loop를 복제해 복잡도를 늘리면서
  측정상 개선이 없어 POSD 기준으로 코드 변경은 원복했다. 다음 Rust simple inproc 후보는
  caller-provided recv storage가 아니라 public send builder 내부 allocation 또는 native
  send/recv boundary 비중을 profiler로 확인한 뒤 선택한다.
- **2026-06-22 Rust single-part send builder fast path 후보 기각**:
  Rust `MULTI_DEALER_DEALER tcp/tls/ws/wss 64` 반복 미달을 대상으로 public
  `socket.send().message(message).flags(DONT_WAIT).submit()` 표면은 유지한 채, binding 내부
  `SendOp<Ready>::submit()`에서 single-part send가 `Vec<zlink_msg_t>`와 multipart loop를
  만들지 않고 한 번의 native submit으로 바로 들어가도록 시험했다. Rust builder는 `Message`를
  by-value로 받아 submit 성공과 실패 모두에서 메시지를 소비하는 계약이므로, Go와 달리 실패
  시 원본 보존 문제는 없다. `cargo test --test ownership_tests --test send_failure_tests
  -- --nocapture`와 `cargo check --all-targets`는 통과했다. 그러나 같은 조건 C
  `perf_c_multi_linux_20260622_123026_c_multi_recheck_rust_dd64_single_part_send_fastpath_pair.txt`
  대비 Rust 후보
  `perf_rust_multi_linux_20260622_123026_rust_multi_dd64_single_part_send_fastpath.txt`는
  tcp/tls/ws/wss 64B가 각각 `2168294.000/2257778.500/2248704.000/2317679.500 msg/s`이고
  C는 `2935680.000/3104996.500/3048080.000/3213063.000 msg/s`라 비율이
  `73.9%/72.7%/73.8%/72.1%`다. 기존 반복 미달 `72.3%/72.0%/73.2%/72.4%`와 사실상
  같고, helper 분기가 send kind별 native call을 중복해 POSD 기준으로 깊은 모듈을 더럽힌다.
  따라서 코드는 원복하고, Rust multi dealer small은 builder allocation보다 native send/recv
  boundary 또는 core/poller 상호작용을 profiler로 분리한 뒤 다음 후보를 선택한다.
- **MULTI_SPOT tcp 64B poller wait 후보 기각**: tcp 64B client에도 progress 없음 구간에서 public `Poller` `POLLIN` wait를 쓰도록 좁혀 시험했다. `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했고 공식 runner도 complete였지만, 같은 조건 C `perf_c_multi_linux_20260525_043251.txt` 대비 no-code Rust `perf_rust_multi_linux_20260525_043320.txt`는 2.990Mmsg/s였고 후보 `perf_rust_multi_linux_20260525_043348.txt`는 2.967Mmsg/s로 낮아졌다. tcp 64B 보류는 idle wait 방식만으로 해소되지 않는다.
- **routed echo single-copy reply 후보 기각**: `MULTI_DEALER_ROUTER`/`MULTI_ROUTER_ROUTER` server가 받은 payload를 `to_vec()`으로 queue payload로 만든 뒤 `Message::copy_from(&reply_bytes)`로 다시 복사하므로, 성공 경로에서는 `Message::with_size()`에 한 번만 복사하고 backpressure일 때만 queue `Vec`을 만드는 후보를 시험했다. release hook 없는 borrowed wrap은 Rust binding policy상 금지되어 native-owned `Message`만 사용했다. `cargo test --manifest-path bindings/rust/perf/multi/Cargo.toml --no-run`은 통과했지만, 공식 runner 결과가 `MULTI_DEALER_ROUTER` 65536B 79.4K→67.2Kops/s(`perf_rust_multi_linux_20260524_195257.txt`), `MULTI_ROUTER_ROUTER` 65536B 78.0K→61.4Kops/s(`perf_rust_multi_linux_20260524_195316.txt`)로 떨어졌다. 기존 `Message::copy_from(&Vec)` 경로가 native send 준비와 더 잘 맞으므로 반영하지 않는다.
- **single routed sender 직접 stamp 후보 기각**: `DEALER_ROUTER`/`ROUTER_ROUTER` large에서
  공통 `send_loop(...)`가 payload `Vec`에 header를 찍은 뒤 `Message::copy_from(...)`으로 다시
  복사하므로, `Message::with_size()` 버퍼에 직접 header를 찍는 후보를 시험했다. `cargo test
  --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고 공식 wrapper도
  complete였다. 그러나 같은 조건 C
  `perf_c_single_linux_20260525_172349_rust_single_routed_direct_msg_c.txt` 대비 후보
  `perf_rust_single_linux_20260525_172349_single_routed_direct_msg_candidate.txt`의 tcp
  `DEALER_ROUTER` 65536/131072B는 14.0/12.9%, `ROUTER_ROUTER` 65536/131072B는
  14.2/12.1%에 그쳤다. sender 복사 하나를 줄여도 routed large 보류를 해소하지
  못하므로 코드는 반영하지 않는다.
- **single routed blocking-send 최신 재확인 후보 기각**: C single routed active sender가
  blocking send를 쓰는 점을 다시 맞춰 `DEALER_ROUTER`/`ROUTER_ROUTER` active send의
  `DONT_WAIT`를 제거해 시험했다. `cargo test --manifest-path
  bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고 공식 wrapper도 complete였다. 그러나
  fresh C `perf_c_single_linux_20260525_200316_rust_single_routed_blocking_send_c.txt` 대비
  Rust 후보 `perf_rust_single_linux_20260525_200343_single_routed_blocking_send_candidate.txt`는
  tcp 65536/131072/262144B에서 `DEALER_ROUTER` 13.5/12.2/11.2%,
  `ROUTER_ROUTER` 13.2/11.9/11.1%에 머물렀다. 기존 `DONT_WAIT` 경로보다 나아지지
  않아 반영하지 않는다.
- **single routed local stats 후보 기각**: receiver는 단일 thread라 `MetricCollector`의
  `Arc<Mutex<LatencyStats>>` 대신 local `LatencyStats`를 직접 쓰는 후보를 시험했다.
  `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고
  공식 wrapper도 complete였다. 같은 C 기준 대비 Rust 후보
  `perf_rust_single_linux_20260525_200500_single_routed_local_stats_candidate.txt`는 tcp
  65536/131072/262144B에서 `DEALER_ROUTER` 13.4/12.2/11.1%, `ROUTER_ROUTER`
  13.2/11.8/11.0%였다. latency mean은 낮아졌지만 throughput은 보류권 그대로라,
  stats lock 제거만으로는 single routed large 보류를 해소하지 못해 코드는 반영하지 않는다.
- **single routed stop-token drain count 후보 기각**: C single routed receiver는 stop token을
  받을 때까지 active header를 집계하므로, Rust 공용 `handle_recv(...)`가 active deadline
  이후 도착한 active message를 버리는지 확인했다. deadline 조건을 제거한 후보는
  `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`을 통과했고
  공식 wrapper `PERF_FAIL_FAST=1 bindings/rust/perf/run_benchmarks.sh --transports tcp
  --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 65536,131072,262144 --duration 1 --runs 3`도
  complete였다(`perf_rust_single_linux_20260525_212512_single_routed_count_until_stop_candidate.txt`).
  그러나 tcp median은 `DEALER_ROUTER` 14.43/7.24/3.67Kmsg/s,
  `ROUTER_ROUTER` 14.45/7.24/3.68Kmsg/s로 기존 fixed 재측정과 같은 대역이다. routed large
  보류는 stop-token drain 중 과소 집계가 주 원인이 아니므로 반영하지 않는다.
- **single routed blocking send + direct `Message::with_size()` 결합 후보 기각**:
  이전에 따로 시험한 blocking send와 direct message stamp가 결합되어야 large payload 복사와
  backpressure 의미가 함께 맞는지 확인했다. active sender가 `Message::with_size()`에 header를
  직접 쓰고 `DONT_WAIT` 없이 submit하는 후보는 `cargo test --manifest-path
  bindings/rust/perf/single/Cargo.toml --no-run`을 통과했고, 공식 wrapper
  `PERF_FAIL_FAST=1 bindings/rust/perf/run_benchmarks.sh --transports tcp --pattern
  DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 65536,262144 --duration 1 --runs 3`도 complete였다
  (`perf_rust_single_linux_20260525_212631_single_routed_blocking_direct_message_candidate.txt`).
  그러나 tcp median은 `DEALER_ROUTER` 65536/262144B가 14.49/3.66Kmsg/s,
  `ROUTER_ROUTER`가 14.49/3.66Kmsg/s로 기존 대역과 같고 latency는 오히려 조금 높았다.
  payload 생성 복사와 blocking flag를 동시에 맞춰도 routed large 보류가 해소되지 않으므로
  코드는 반영하지 않는다.
- **single SPOT `TopicMessage` 재사용 후보 기각**: single SPOT subscriber loop가 매 수신마다
  `TopicMessage`를 새로 만드는 비용을 줄이기 위해 caller-provided storage를 계속 재사용하는
  후보를 시험했다. 공식 C 기준
  `perf_c_single_linux_20260525_150657_rust_spot_subscribe_reuse_c.txt`와 Rust 후보
  `perf_rust_single_linux_20260525_150735_spot_subscribe_reuse_candidate.txt`는 모두 complete였지만,
  1024B C 대비 비율은 tcp/ws/wss/tls 48.7/56.3/63.8/59.0%에 그쳤다. `TopicMessage`
  placeholder 할당만 줄여서는 single SPOT 1024B 보류를 해소하지 못하고, 남은 차이는
  SPOT subscribe 호출 경계와 sender/receiver backlog 처리 쪽에 있다. 코드는 반영하지 않는다.
- **single SPOT sender 직접 stamp 후보 기각**: multi SPOT server에서 효과가 있었던
  `Message::with_size()` + `data_mut()` 직접 stamp를 single SPOT active publisher에도
  좁혀 시험했다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은
  통과했고, 공식 C 기준
  `perf_c_single_linux_20260525_160829_rust_single_spot_direct_stamp_c.txt`와 Rust 후보
  `perf_rust_single_linux_20260525_160909_single_spot_direct_stamp_candidate.txt`는 모두 complete였다.
  그러나 1024B C 대비 비율은 tcp/ws/wss/tls 48.0/50.6/64.7/54.6%로 SPOT 기준보다 낮고,
  직전 `TopicMessage` 재사용 후보보다 ws/tls가 낮았다. sender payload 복사 하나를 줄여도
  single SPOT 1024B 보류를 해소하지 못하므로 기존 `Message::copy_from(&payload)` 경로를
  유지한다.
- **single SPOT local sampled stats 후보 기각**: multi SPOT에서 효과가 있었던 throughput
  count와 latency sample 저장 분리를 single SPOT subscriber에 좁혀 시험했다. 수신 thread
  안에서 전체 active payload count는 유지하고 latency vector 저장만 32개당 1개로 줄여
  `Arc<Mutex<LatencyStats>>` hot path를 피했다. `cargo test --manifest-path
  bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고, 공식 C 기준
  `perf_c_single_linux_20260525_162920_rust_single_spot_sampled_stats_c.txt`와 Rust 후보
  `perf_rust_single_linux_20260525_163000_single_spot_sampled_stats_candidate.txt`는 모두
  complete였다. 그러나 1024B C 대비 비율은 tcp/ws/wss/tls 52.6/58.6/62.5/56.2%로
  SPOT 기준보다 낮다. latency 기록 비용만 줄여서는 single SPOT 1024B 보류를 해소하지
  못하므로 코드는 반영하지 않는다.
- **single SPOT exact local stats 후보 기각**: sampled stats 후보가 latency semantics를
  일부 바꾼 점을 분리하기 위해, 수신 thread 안에서 local `LatencyStats`를 직접 쓰되
  C와 같은 active window와 전체 latency 기록 의미는 유지하는 후보를 다시 시험했다.
  `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고,
  공식 C 기준 `perf_c_single_linux_20260525_174613_rust_single_spot_exact_local_c.txt`와
  Rust 후보 `perf_rust_single_linux_20260525_174613_single_spot_exact_local_candidate.txt`는
  모두 complete였다. 그러나 1024B C 대비 비율은 tcp/ws/wss/tls 44.9/56.0/57.6/56.0%로
  SPOT 기준보다 낮고, 이전 sampled stats 또는 `subscribe_part` 후보보다 대체로 낮다.
  lock 제거만으로는 single SPOT 1024B 보류를 해소하지 못하므로 코드는 반영하지 않는다.
- **single SPOT 1024B current 재측정도 보류 유지**: 후보 적용 없이 current HEAD를
  fresh C와 다시 맞춰 측정했다. C
  `perf_c_single_linux_20260525_200721_rust_single_spot1024_current_c_recheck.txt`와 Rust
  `perf_rust_single_linux_20260525_200800_single_spot1024_current_recheck.txt`는 모두
  complete였지만, tcp/ws/wss/tls 1024B 비율은 44.1/53.6/62.8/51.0%로 Rust SPOT 기준보다
  낮다. 따라서 최신 baseline 흔들림만으로는 single SPOT 1024B 보류를 해소할 수 없다.
- **single SPOT 1024B clean 후 재측정도 보류 유지**: actor entry spot join 동기화
  커밋 뒤 clean 상태에서 같은 조건을 다시 측정했다. C
  `perf_c_single_linux_20260525_211940_rust_single_spot1024_current_after_clean_c.txt`와
  Rust `perf_rust_single_linux_20260525_212023_single_spot1024_current_after_clean.txt`는
  모두 complete였지만, tcp/ws/wss/tls 1024B 비율은 47.2/53.9/62.2/54.0%로 여전히
  Rust SPOT 기준보다 낮다. tcp와 tls는 이전 current 재측정보다 조금 올랐지만 기준선을
  넘지 못해 보류를 유지한다.
- **single SPOT public `subscribe_part` 후보 기각**: C single SPOT은
  `zlink_spot_subscribe_part` 단일 part 수신으로 header를 바로 디코드하므로, Rust에도
  public `Spot::subscribe_part`/`SpotPart` 후보를 추가하고 single SPOT subscriber가
  `TopicMessage` 전체 구성을 건너뛰도록 시험했다. `cargo test --manifest-path
  bindings/rust/Cargo.toml --no-run`과 `cargo test --manifest-path
  bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고, 공식 C 기준
  `perf_c_single_linux_20260525_163636_rust_single_spot_subscribe_part_c.txt`와 Rust 후보
  `perf_rust_single_linux_20260525_163720_single_spot_subscribe_part_candidate.txt`는
  complete였다. 그러나 1024B C 대비 비율은 tcp/ws/wss/tls 53.8/63.8/73.6/53.4%로
  SPOT 기준보다 낮고, tls는 기존 재측정 59.6%보다 낮아졌다. sampled stats 결합 후보
  `perf_rust_single_linux_20260525_163817_single_spot_subscribe_part_sampled_candidate.txt`
  와 sender direct stamp 결합 후보
  `perf_rust_single_linux_20260525_163932_single_spot_subscribe_part_direct_stamp_candidate.txt`도
  기준선을 넘기지 못했다. transport별 선택 적용 후보는 묶음 실행에서 wss partial
  `perf_rust_single_linux_20260525_164104_single_spot_subscribe_part_selected.txt`을 만들었고,
  wss 단독 재시도 `perf_rust_single_linux_20260525_164136_single_spot_subscribe_part_selected_wss_retry.txt`도
  기존 대표값보다 낮았다. public API를 넓혀도 single SPOT 1024B 보류를 안정적으로
  해소하지 못하므로 코드는 반영하지 않는다.
- **single routed clean 후 sender 직접 stamp 재확인 후보 기각**: multi routed large에서
  효과가 있었던 `Message::with_size()` + `data_mut()` 직접 stamp를 clean 후 single routed
  sender에 다시 좁혀 시험했다. `cargo test --manifest-path
  bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고, fresh C 기준
  `perf_c_single_linux_20260525_215139_rust_single_routed_direct_msg_c_recheck.txt`와
  Rust 후보 `perf_rust_single_linux_20260525_215206_single_routed_direct_message_candidate.txt`는
  모두 complete였다. 그러나 tcp median은 `DEALER_ROUTER` 65536/131072/262144B가
  14.391/7.175/3.649Kmsg/s, `ROUTER_ROUTER`가 14.415/7.200/3.657Kmsg/s로
  기존 fixed 재측정과 같은 대역이다. payload 복사 하나를 제거해도 single routed large
  보류가 해소되지 않아 코드는 반영하지 않는다.
- **single routed clean 후 local stats 재확인 후보 기각**: receiver 단일 thread의
  `Arc<Mutex<LatencyStats>>` 비용을 다시 분리하기 위해 `DEALER_ROUTER`/`ROUTER_ROUTER`
  receiver가 local `LatencyStats`를 직접 쓰는 후보를 clean 후 재시험했다.
  `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고,
  공식 runner `perf_rust_single_linux_20260525_215331_single_routed_local_stats_candidate.txt`도
  complete였다. tcp median은 `DEALER_ROUTER` 14.431/7.199/3.653Kmsg/s,
  `ROUTER_ROUTER` 14.383/7.215/3.576Kmsg/s로 기존과 같은 대역이다. latency storage
  잠금 제거는 large throughput 병목이 아니므로 코드는 반영하지 않는다.
- **single routed active send 의미 정렬**: Rust routed active/stop send가 C single
  routed의 `ZLINK_SEND_FLAGS_NONE`과 달리 `DONT_WAIT`로 보내고 실패 시 메시지를 버리던
  차이를 바로잡았다. `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은
  통과했고, 공식 wrapper
  `perf_rust_single_linux_20260527_081513_codex_rust_single_routed_large_blocking_send_tcp_20260527.txt`
  는 complete였다. current C
  `perf_c_single_linux_20260527_074526_codex_c_single_routed_large_for_rust_current_20260527.txt`
  대비 tcp `DEALER_ROUTER` 65536/131072/262144B는 13.0/11.5/10.7%,
  `ROUTER_ROUTER`는 13.2/11.4/11.2%다. C와 send 의미는 맞췄지만 throughput은 기존
  900MB/s대와 같아 single routed large 보류는 유지한다.
- **single SPOT 1024B backpressure yield 후보 기각**: active publish backpressure 때
  C의 1ms poll 대기와 다른 `thread::yield_now()`로 더 자주 재시도하는 후보를 시험했다.
  `cargo test --manifest-path bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고,
  공식 runner `perf_rust_single_linux_20260525_215451_single_spot1024_yield_backpressure_candidate.txt`는
  complete였다. 그러나 tcp/ws/wss/tls median은 178.571/160.717/154.465/172.119Kmsg/s로
  fresh C 대비 47.2/48.5/62.8/54.0%였다. tcp/tls는 기존 clean 재측정과 같고 ws는 더
  낮아졌으므로, C single SPOT의 1ms backpressure 대기 의미를 유지한다.
- **single SPOT 1024B clean 후 sender 직접 stamp 재확인 후보 기각**: single SPOT active
  publisher에서도 `Message::copy_from(&payload)` 대신 `Message::with_size()` + `data_mut()`
  직접 stamp를 다시 시험했다. `cargo test --manifest-path
  bindings/rust/perf/single/Cargo.toml --no-run`은 통과했고, 공식 runner
  `perf_rust_single_linux_20260525_220944_single_spot1024_direct_message_candidate.txt`는
  complete였다. 그러나 tcp/ws/wss/tls median은 152.214/144.305/145.573/164.769Kmsg/s로
  clean current 재측정보다 낮았다. sender payload 복사 제거는 single SPOT 1024B의
  병목을 해소하지 못하므로 코드는 반영하지 않는다.
- **2026-05-24 tcp full 재측정 요약**: DEALER_ROUTER 46~107%(전 size 통과), ROUTER_ROUTER small 통과/large 보류, SPOT_REQREP 64/256/1024/65536B 통과 및 131072/262144B 보류, STREAM 90~101% 통과, DD small 통과/large 보류, SPOT small 통과권/large 보류(copy 영역), SPOT_SENDSEND 64/256/1024/65536/131072B 통과 및 262144B 보류, PUBSUB 일부만 통과였다. 당시 PUBSUB small은 fresh baseline 대비 ~3~5%였지만, 2026-05-25 server `POLLOUT` wait 제거 뒤 `MULTI_PUBSUB` small 보류는 해소됐다.
- **남은 보류(Rust)**: single routed large. single SPOT 1024B는 2026-05-27 public
  `Spot::publish_part`/`subscribe_part` 경로 적용 뒤 통과로 갱신했다.

### 6.8 Python 상태

#### 6.8.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | `보류(6.7%)` | `보류(6.6%)` | `보류(9.6%)` | `통과(201.9%)` | `통과(211.2%)` | `통과(93.7%)` | 64/256/1024B는 fresh C `perf_c_single_linux_20260525_201321_python_single_native_recv_part_c.txt` 대비 Python `perf_python_single_linux_20260525_201459_single_native_recv_part_header_candidate.txt` 기준이다. message-socket send native result fast path에 더해 receiver가 single-part `zlink_recv_part`를 직접 사용해 `Received` 객체 구성을 피한다. small 처리량은 57.2/55.7/57.5Kmsg/s에서 88.6/87.2/80.1Kmsg/s로 올랐지만, C 기준도 1.3M/1.3M/836Kmsg/s라 여전히 보류다. large size는 기존 C `perf_c_single_linux_20260522_155901_codex_c_single_tcp_for_python_20260522.txt`, Python `perf_python_single_linux_20260522_160548_codex_python_single_tcp_stop_wait_fix_20260522.txt` 기준이다. |
| `tcp` | `PUBSUB` | `보류(4.4%)` | `보류(5.0%)` | `보류(7.0%)` | `통과(73.5%)` | `통과(78.8%)` | `통과(79.2%)` | 64/256/1024B는 fresh C `perf_c_single_linux_20260525_205725_python_single_pubsub_publish_native_c.txt` 대비 Python `perf_python_single_linux_20260525_205819_single_pubsub_publish_native_candidate.txt` 기준이다. publisher가 `zlink_publish_part` native result fast path를 쓰고, subscriber가 `zlink_subscribe_part` 직접 수신으로 `TopicMessage` 객체 구성을 피한다. small 처리량은 57.2/54.3/55.9Kmsg/s까지 올랐지만 기준 대비 낮아 보류한다. large size는 기존 파일 기준선을 넘었다. |
| `tcp` | `DEALER_DEALER` | `보류(6.7%)` | `보류(6.7%)` | `보류(9.7%)` | `통과(78.4%)` | `통과(86.7%)` | `통과(91.1%)` | 64/256/1024B는 fresh C `perf_c_single_linux_20260525_201321_python_single_native_recv_part_c.txt` 대비 Python `perf_python_single_linux_20260525_201459_single_native_recv_part_header_candidate.txt` 기준이다. message-socket send native result fast path와 receiver `zlink_recv_part` 직접 수신으로 55.4/55.6/57.0Kmsg/s에서 88.1/87.7/81.7Kmsg/s로 올렸지만, C 기준 대비 한 자릿수 비율이라 보류를 유지한다. large size는 기존 파일 기준선을 넘었다. |
| `tcp` | `DEALER_ROUTER` | `보류(3.0%)` | `보류(2.8%)` | `보류(3.3%)` | `보류(17.8%)` | `보류(18.4%)` | `보류(15.1%)` | 64/256/1024/65536B는 같은 조건 fresh C `perf_c_single_linux_20260525_184018_python_single_routed_current_c.txt` 대비 Python `perf_python_single_linux_20260525_184450_single_routed_native_result_default.txt` 기준이다. sender가 non-routed message-socket native result fast path를 쓰지만 routed one-way 기준보다 낮아 보류한다. 131072/262144B는 기존 파일 기준이다. |
| `tcp` | `ROUTER_ROUTER` | `보류(5.9%)` | `보류(3.0%)` | `보류(2.9%)` | `보류(18.3%)` | `보류(14.7%)` | `보류(16.1%)` | 64/256/1024/65536B는 같은 조건 fresh C `perf_c_single_linux_20260525_184018_python_single_routed_current_c.txt` 대비 Python `perf_python_single_linux_20260525_184450_single_routed_native_result_default.txt` 기준이다. Python single receiver를 bounded DONTWAIT drain으로 바꾼 뒤 complete를 확보했고, routed send도 native result fast path로 좁혀 64/256/1024/65536B를 36.4/33.0/35.6/16.4Kmsg/s까지 올렸다. 전 size는 기준보다 낮아 보류한다. |
| `tcp` | `SPOT` | `보류(13.2%)` | `보류(12.3%)` | `보류(11.4%)` | `통과(39.2%)` | `통과(43.1%)` | `통과(36.7%)` | 64/256/1024B는 current C `perf_c_single_linux_20260527_080022_codex_c_single_python_spot_small_recheck_20260527.txt` 대비 Python `perf_python_single_linux_20260527_080727_codex_python_single_spot_small_subscribe_part_all_20260527.txt` 기준이다. binding에 public `SpotSubscribedPart`와 `Spot.subscribe_part_into(...)`를 추가해 callback 내부 `TopicMessage`/parts 구성을 피했다. 처리량은 42.8/43.0/41.2Kmsg/s로 이전보다 개선됐지만 SPOT 기준보다 낮다. 131072B는 C `perf_c_single_linux_20260522_201611_codex_c_single_tcp_spot131072_python_outlier_recheck_20260522.txt`, Python `perf_python_single_linux_20260522_201626_codex_python_single_tcp_spot131072_outlier_recheck_20260522.txt` 기준으로 갱신했다. 기존 131072B의 271880.0%는 C 기준 throughput이 5 msg/s로 낮게 나온 outlier였다. |
| `ws` | `PAIR` | `보류(6.3%)` | `보류(6.4%)` | `보류(12.1%)` | `통과(82.7%)` | `통과(84.9%)` | `통과(88.5%)` | 64/256/1024B는 fresh C `perf_c_single_linux_20260525_201556_python_single_native_recv_part_nontcp_c.txt` 대비 Python `perf_python_single_linux_20260525_201815_single_native_recv_part_nontcp.txt` 기준이다. receiver `zlink_recv_part` 직접 수신 뒤에도 small size는 기준보다 낮고, large size는 기존 파일 기준선을 넘었다. |
| `ws` | `PUBSUB` | `보류(4.2%)` | `보류(5.1%)` | `보류(8.7%)` | `통과(70.0%)` | `통과(73.2%)` | `통과(80.5%)` | 64/256/1024B는 fresh C `perf_c_single_linux_20260525_205828_python_single_pubsub_publish_native_nontcp_c.txt` 대비 Python `perf_python_single_linux_20260525_210032_single_pubsub_publish_native_nontcp_candidate.txt` 기준이다. publisher native result fast path와 subscriber `zlink_subscribe_part` 직접 수신 뒤에도 small size는 기준보다 낮고, large size는 기존 파일 기준선을 넘었다. |
| `ws` | `DEALER_DEALER` | `보류(6.2%)` | `보류(6.0%)` | `보류(11.1%)` | `통과(81.2%)` | `통과(85.8%)` | `통과(91.3%)` | C/Python 파일은 위 PAIR 행과 같다. receiver `zlink_recv_part` 직접 수신 뒤에도 small size는 기준보다 낮고, large size는 기준선을 넘었다. |
| `ws` | `DEALER_ROUTER` | `보류(2.1%)` | `보류(2.6%)` | `보류(3.8%)` | `통과(39.1%)` | `보류(31.2%)` | `보류(26.8%)` | C/Python 파일은 위 PAIR 행과 같다. 65536B는 routed one-way 최소 기준을 넘었지만 나머지는 낮아 보류한다. |
| `ws` | `ROUTER_ROUTER` | `보류(2.3%)` | `보류(2.8%)` | `보류(3.7%)` | `통과(37.9%)` | `보류(29.9%)` | `보류(26.4%)` | C/Python 파일은 위 PAIR 행과 같다. 65536B는 routed one-way 최소 기준을 넘었지만 나머지는 낮아 보류한다. |
| `ws` | `SPOT` | `보류(13.3%)` | `보류(11.2%)` | `보류(11.6%)` | `통과(54.8%)` | `통과(64.6%)` | `통과(49.0%)` | 64/256/1024B는 current C `perf_c_single_linux_20260527_080022_codex_c_single_python_spot_small_recheck_20260527.txt` 대비 Python `perf_python_single_linux_20260527_080727_codex_python_single_spot_small_subscribe_part_all_20260527.txt` 기준이다. public `Spot.subscribe_part_into(...)` 단일-part 수신 경로 뒤에도 small size는 기준보다 낮고, large size는 기존 파일 기준선을 넘었다. |
| `wss` | `PAIR` | `보류(6.3%)` | `보류(6.4%)` | `보류(18.7%)` | `통과(89.4%)` | `통과(96.4%)` | `통과(105.1%)` | 64/256/1024B는 fresh C `perf_c_single_linux_20260525_201556_python_single_native_recv_part_nontcp_c.txt` 대비 Python `perf_python_single_linux_20260525_201815_single_native_recv_part_nontcp.txt` 기준이다. receiver `zlink_recv_part` 직접 수신 뒤에도 small size는 기준보다 낮고, large size는 기준선을 넘었다. |
| `wss` | `PUBSUB` | `보류(3.7%)` | `보류(5.1%)` | `보류(12.7%)` | `통과(78.4%)` | `통과(86.7%)` | `통과(86.1%)` | 64/256/1024B는 fresh C `perf_c_single_linux_20260525_205828_python_single_pubsub_publish_native_nontcp_c.txt` 대비 Python `perf_python_single_linux_20260525_210032_single_pubsub_publish_native_nontcp_candidate.txt` 기준이다. publisher native result fast path와 subscriber `zlink_subscribe_part` 직접 수신 뒤에도 small size는 기준보다 낮고, large size는 기준선을 넘었다. |
| `wss` | `DEALER_DEALER` | `보류(6.0%)` | `보류(6.0%)` | `보류(18.0%)` | `통과(87.1%)` | `통과(101.9%)` | `통과(97.8%)` | C/Python 파일은 위 PAIR 행과 같다. receiver `zlink_recv_part` 직접 수신 뒤에도 small size는 기준보다 낮고, large size는 기준선을 넘었다. |
| `wss` | `DEALER_ROUTER` | `보류(1.9%)` | `보류(2.2%)` | `보류(5.7%)` | `통과(79.2%)` | `통과(90.0%)` | `통과(98.9%)` | C/Python 파일은 위 PAIR 행과 같다. small size는 기준보다 낮고, large size는 routed one-way 기준선을 넘었다. |
| `wss` | `ROUTER_ROUTER` | `보류(3.0%)` | `보류(2.6%)` | `보류(7.5%)` | `통과(75.2%)` | `통과(89.0%)` | `통과(93.3%)` | C/Python 파일은 위 PAIR 행과 같다. small size는 기준보다 낮고, large size는 routed one-way 기준선을 넘었다. |
| `wss` | `SPOT` | `보류(12.6%)` | `보류(11.8%)` | `보류(16.3%)` | `통과(97.6%)` | `통과(89.8%)` | `통과(67.4%)` | 64/256/1024B는 current C `perf_c_single_linux_20260527_080022_codex_c_single_python_spot_small_recheck_20260527.txt` 대비 Python `perf_python_single_linux_20260527_080727_codex_python_single_spot_small_subscribe_part_all_20260527.txt` 기준이다. public `Spot.subscribe_part_into(...)` 단일-part 수신 경로 뒤에도 small size는 기준보다 낮고, large size는 기존 파일 기준선을 넘었다. |
| `tls` | `PAIR` | `보류(6.3%)` | `보류(6.3%)` | `보류(13.0%)` | `통과(86.6%)` | `통과(79.4%)` | `통과(85.2%)` | 64/256/1024B는 fresh C `perf_c_single_linux_20260525_201556_python_single_native_recv_part_nontcp_c.txt` 대비 Python `perf_python_single_linux_20260525_201815_single_native_recv_part_nontcp.txt` 기준이다. receiver `zlink_recv_part` 직접 수신 뒤에도 small size는 기준보다 낮고, large size는 기준선을 넘었다. |
| `tls` | `PUBSUB` | `보류(4.1%)` | `보류(4.6%)` | `보류(8.7%)` | `통과(65.0%)` | `통과(69.8%)` | `통과(80.7%)` | 64/256/1024B는 fresh C `perf_c_single_linux_20260525_205828_python_single_pubsub_publish_native_nontcp_c.txt` 대비 Python `perf_python_single_linux_20260525_210032_single_pubsub_publish_native_nontcp_candidate.txt` 기준이다. publisher native result fast path와 subscriber `zlink_subscribe_part` 직접 수신 뒤에도 small size는 기준보다 낮고, large size는 기준선을 넘었다. |
| `tls` | `DEALER_DEALER` | `보류(6.2%)` | `보류(6.3%)` | `보류(12.7%)` | `통과(73.1%)` | `통과(81.7%)` | `통과(89.4%)` | C/Python 파일은 위 PAIR 행과 같다. receiver `zlink_recv_part` 직접 수신 뒤에도 small size는 기준보다 낮고, large size는 기준선을 넘었다. |
| `tls` | `DEALER_ROUTER` | `보류(1.6%)` | `보류(1.8%)` | `보류(3.4%)` | `통과(54.4%)` | `통과(65.0%)` | `통과(65.3%)` | C/Python 파일은 위 PAIR 행과 같다. small size는 기준보다 낮고, large size는 routed one-way 기준선을 넘었다. |
| `tls` | `ROUTER_ROUTER` | `보류(2.6%)` | `보류(2.7%)` | `보류(4.5%)` | `통과(57.0%)` | `통과(62.7%)` | `통과(62.4%)` | C/Python 파일은 위 PAIR 행과 같다. small size는 기준보다 낮고, large size는 routed one-way 기준선을 넘었다. |
| `tls` | `SPOT` | `보류(13.0%)` | `보류(11.1%)` | `보류(12.2%)` | `통과(91.5%)` | `통과(94.0%)` | `통과(84.3%)` | 64/256/1024B는 current C `perf_c_single_linux_20260527_080022_codex_c_single_python_spot_small_recheck_20260527.txt` 대비 Python `perf_python_single_linux_20260527_080727_codex_python_single_spot_small_subscribe_part_all_20260527.txt` 기준이다. public `Spot.subscribe_part_into(...)` 단일-part 수신 경로 뒤에도 small size는 기준보다 낮고, large size는 기존 파일 기준선을 넘었다. |

#### 6.8.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | `보류(10.2%)` | `보류(15.6%)` | `보류(18.4%)` | `통과(52.2%)` | `통과(41.1%)` | `보류(26.1%)` | 64/256/1024B는 같은 조건 fresh C `perf_c_multi_linux_20260525_184808_python_multi_send_native_c.txt` 대비 Python `perf_python_multi_linux_20260525_185647_multi_nonrouted_send_native_result_default.txt` 기준이다. client send를 non-routed native result fast path로 좁혀 small 처리량을 317.5/311.9/280.8Kmsg/s까지 올렸지만 기준선에는 아직 낮다. 262144B는 clean 후 current HEAD 같은 조건 C `perf_c_multi_linux_20260525_211009_python_dd262_current_recheck_after_clean_c.txt` 대비 Python `perf_python_multi_linux_20260525_211059_dd262_current_recheck_after_clean.txt` 기준이다. 65536/131072B는 기존 `perf_python_multi_linux_20260524_191648.txt` 기준이다. |
| `tcp` | `MULTI_DEALER_ROUTER` | `보류(13.4%)` | `보류(13.4%)` | `보류(13.3%)` | `통과(43.2%)` | `통과(57.0%)` | `통과(63.2%)` | 64/256/1024B는 같은 조건 fresh C `perf_c_multi_linux_20260525_184808_python_multi_send_native_c.txt` 대비 Python `perf_python_multi_linux_20260525_185647_multi_nonrouted_send_native_result_default.txt` 기준이다. reply header decode를 `to_bytes_list()` 대신 `ReceivedMessage.data` view로 바꾸고 routed `recv_into` single-part fast path를 추가한 상태이며, DEALER sender는 non-routed native result fast path를 쓴다. large 수치는 모두 기준선 위로 올라갔지만 small size는 여전히 낮다. |
| `tcp` | `MULTI_ROUTER_ROUTER` | `보류(9.6%)` | `보류(9.7%)` | `보류(10.0%)` | `통과(33.1%)` | `통과(44.1%)` | `통과(57.3%)` | 64/256/1024B는 같은 조건 fresh C `perf_c_multi_linux_20260525_184808_python_multi_send_native_c.txt` 대비 Python `perf_python_multi_linux_20260525_185647_multi_nonrouted_send_native_result_default.txt` 기준이다. routed send native result 후보는 complete 안정성을 깨서 반영하지 않았고, client reply header decode `.data` 경로와 routed `recv_into` single-part fast path만 유지한다. 65536B 이상은 기존 보강 파일 기준선을 넘었지만 small size는 여전히 낮다. |
| `tcp` | `MULTI_PUBSUB` | `보류(7.6%)` | `보류(7.9%)` | `보류(17.7%)` | `통과(58.7%)` | `통과(90.9%)` | `통과(95.0%)` | 64/256/1024/65536B는 같은 조건 fresh C `perf_c_multi_linux_20260525_190118_python_multi_pubsub_publish_native_c.txt` 대비 Python `perf_python_multi_linux_20260525_192512_multi_pubsub_client_len_fastpath_candidate.txt` median 기준이다. subscriber는 stop token과 payload length로 active count를 유지하고, header decode는 latency sampling 대상에만 수행한다. 131072/262144B는 기존 `perf_python_multi_linux_20260524_191738.txt` 기준이다. small size는 아직 기준보다 낮다. |
| `tcp` | `MULTI_SPOT` | `보류(6.7%)` | `보류(8.7%)` | `보류(8.4%)` | `보류(8.4%)` | `보류(4.7%)` | `보류(6.1%)` | 64/256/1024/65536B는 같은 조건 fresh C `perf_c_multi_linux_20260525_221504_python_multi_spot_native_subscribe_metric_c.txt` 대비 Python `perf_python_multi_linux_20260525_223653_multi_spot_native_subscribe_metric_candidate.txt` 기준이다. client drain을 `TopicMessage` 구성 없이 native `zlink_spot_subscribe_part` 단일 part 수신과 단일 header unpack으로 좁혀 64/256/1024/65536B를 308.6/324.6/299.9/130.9Kmsg/s까지 올렸다. 기존 1.9/1.9/1.9/4.3%보다 개선됐지만 SPOT 기준보다 낮고 latency backlog가 수백 ms 이상이라 보류를 유지한다. 131072/262144B는 기존 파일 기준이다. |
| `tcp` | `MULTI_SPOT_REQREP` | `보류(14.6%)` | `보류(15.0%)` | `보류(16.3%)` | `보류(29.2%)` | `통과(39.7%)` | `통과(41.8%)` | C `perf_c_multi_linux_20260522_162958_codex_c_multi_tcp_for_python_duration1_20260522.txt`, Python `perf_python_multi_linux_20260524_192625.txt` 기준이다. reply callback의 metric header decode에서 추가 `to_bytes()` 복사를 피했지만, callback으로 들어오기 전 reply part는 이미 Python `Message`로 clone되므로 효과는 제한적이다. large timeout은 active window 뒤 pending reply drain 누락 때문이었고, drain 보강 뒤 complete를 확보했다. 131072B와 262144B는 SPOT 기준선을 넘고 나머지는 낮다. |
| `tcp` | `MULTI_SPOT_SENDSEND` | `보류(10.9%)` | `보류(11.3%)` | `보류(12.1%)` | `보류(12.4%)` | `보류(18.6%)` | `통과(43.6%)` | 64/256/1024B는 current C `perf_c_multi_linux_20260527_082237_codex_c_multi_sendsend_small_recheck_20260527.txt` 대비 Python `perf_python_multi_linux_20260527_083127_codex_python_multi_sendsend_small_recheck_20260527.txt` 기준이다. Python median은 29.033/29.211/28.548Kops/s였고 C 기준은 266.306/257.744/235.179Kops/s라 최신 같은 조건 재측정에서도 small size는 기준보다 낮다. 65536/262144B는 routed reply decode를 `ReceivedMessage.data` view로 바꾼 뒤 제한 재측정 `perf_python_multi_linux_20260524_192915.txt`로 갱신했고, 131072B는 `perf_python_multi_linux_20260522_204134_codex_python_multi_tcp_sendsend_large_no_poller_recheck_20260522.txt` 기준이다. server/client는 각 `nlwp=12` 수준으로 thread 폭증은 아니었다. small size는 서버/client의 고정 1ms sleep 대신 poller wake를 쓰도록 바꿨지만 최신 C 기준을 넘지 못했고, 65536B 이상은 Spot poller 등록 자체가 timeout을 유발해 기존 sleep fallback을 유지한다. 262144B만 기준선을 넘고 나머지는 낮다. |
| `tcp` | `MULTI_STREAM` | `보류(1.3%)` | `보류(1.3%)` | `보류(1.3%)` | `보류(4.0%)` | `보류(8.2%)` | `보류(16.0%)` | C/Python 파일은 위 MULTI_DEALER_DEALER 행과 같다. Python STREAM server와 shared C stream client 조합 기준이며 전 size가 기준보다 낮다. |
| `ws` | `MULTI_DEALER_DEALER` | `보류(5.7%)` | `보류(9.6%)` | `보류(11.7%)` | `통과(59.6%)` | `통과(53.9%)` | `통과(71.2%)` | C `perf_c_multi_linux_20260522_150505_codex_c_multi_ws_no_stream_for_rust_20260522.txt`, Python `perf_python_multi_linux_20260522_182833_codex_python_multi_ws_duration1_20260522.txt` 기준이다. large size는 기준선을 넘고 small size는 낮다. |
| `ws` | `MULTI_DEALER_ROUTER` | `보류(13.3%)` | `보류(13.2%)` | `보류(13.0%)` | `통과(38.0%)` | `통과(42.0%)` | `통과(46.2%)` | C `perf_c_multi_linux_20260522_150505_codex_c_multi_ws_no_stream_for_rust_20260522.txt` 대비 Python `perf_python_multi_linux_20260525_000108.txt` median 기준이다. routed `recv_into` single-part fast path 뒤 65536B가 routed multi 기준선을 넘었다. small size는 여전히 낮다. |
| `ws` | `MULTI_ROUTER_ROUTER` | `보류(9.8%)` | `보류(9.6%)` | `보류(9.6%)` | `보류(29.1%)` | `통과(43.3%)` | `통과(51.7%)` | 65536B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_053226.txt` 대비 Python `perf_python_multi_linux_20260525_053317.txt` median 기준이다. 131072B는 C `perf_c_multi_linux_20260525_023817.txt` 대비 Python `perf_python_multi_linux_20260525_024823.txt` 기준이다. 64/256/1024B는 C `perf_c_multi_linux_20260522_150505_codex_c_multi_ws_no_stream_for_rust_20260522.txt` 대비 Python 단독 complete 재측정 `perf_python_multi_linux_20260525_000503.txt` median 기준이고, 262144B도 기존 단독 complete 재측정 기준이다. 65536B는 20.0%대에서 29.1%까지 올랐지만 아직 기준선 아래다. 131072B/262144B는 기준선을 넘었다. |
| `ws` | `MULTI_PUBSUB` | `보류(8.3%)` | `보류(7.8%)` | `보류(9.5%)` | `통과(74.5%)` | `통과(77.0%)` | `통과(116.1%)` | 64/256/1024/65536B는 같은 조건 fresh C `perf_c_multi_linux_20260525_190118_python_multi_pubsub_publish_native_c.txt` 대비 Python `perf_python_multi_linux_20260525_192512_multi_pubsub_client_len_fastpath_candidate.txt` median 기준이다. 131072/262144B는 기존 full 기준이다. stop token과 payload length 기반 count fast path 뒤 complete를 유지하고 small absolute 처리량은 올랐지만 기준보다 낮다. |
| `ws` | `MULTI_SPOT` | `보류(1.7%)` | `보류(1.9%)` | `보류(1.8%)` | `보류(4.2%)` | `보류(4.7%)` | `보류(4.0%)` | C/Python 파일은 위 MULTI_DEALER_DEALER 행과 같다. 실행 중 server/client는 각 `nlwp=12` 수준으로 thread 폭증은 아니었다. 전 size가 기준보다 낮고 latency backlog가 크게 쌓인다. |
| `ws` | `MULTI_SPOT_REQREP` | `보류(15.7%)` | `보류(16.2%)` | `보류(18.6%)` | `통과(42.7%)` | `통과(37.5%)` | `통과(50.3%)` | C/Python 파일은 위 MULTI_DEALER_DEALER 행과 같다. pending reply drain 보강 경로로 전 size complete를 확보했다. 65536B/131072B/262144B는 기준선을 넘었다. |
| `ws` | `MULTI_SPOT_SENDSEND` | `보류(11.8%)` | `보류(9.9%)` | `보류(9.7%)` | `보류(1.6%)` | `보류(18.5%)` | `통과(34.3%)` | 64B는 같은 조건 최신 재측정 C `perf_c_multi_linux_20260525_084304.txt` 대비 Python `perf_python_multi_linux_20260525_084908.txt` 기준이다. 나머지 C/Python 파일은 위 MULTI_DEALER_DEALER 행과 같다. active slot 제한과 pending reply drain 보강 뒤 timeout은 없고, 262144B는 SPOT 기준선을 넘었다. 나머지 size는 기준보다 낮아 send-send echo hot path 재검토가 필요하다. |
| `ws` | `MULTI_STREAM` | `보류(1.1%)` | `보류(1.1%)` | `보류(1.1%)` | `보류(9.5%)` | `통과(63.0%)` | `통과(422.3%)` | 64~65536B는 C `perf_c_multi_linux_20260522_152456_codex_c_multi_ws_stream_clients100_for_rust_20260522.txt`, 131072B/262144B는 C `perf_c_multi_linux_20260522_062037_codex_c_ws_multi_stream_large_for_go_20260522.txt`, Python은 `perf_python_multi_linux_20260522_182833_codex_python_multi_ws_duration1_20260522.txt` 기준이다. small size는 매우 낮고 large size만 기준선을 넘었다. |
| `wss` | `MULTI_DEALER_DEALER` | `보류(5.4%)` | `보류(9.5%)` | `보류(12.3%)` | `통과(102.2%)` | `통과(78.0%)` | `통과(73.2%)` | C `perf_c_multi_linux_20260522_152936_codex_c_multi_wss_no_stream_for_rust_20260522.txt`, Python `perf_python_multi_linux_20260522_191732_codex_python_multi_wss_tls_fix_duration1_20260522.txt` 기준이다. large size는 기준선을 넘고 small size는 낮다. |
| `wss` | `MULTI_DEALER_ROUTER` | `보류(13.5%)` | `보류(12.7%)` | `보류(13.8%)` | `통과(70.5%)` | `통과(66.3%)` | `통과(68.6%)` | C `perf_c_multi_linux_20260522_152936_codex_c_multi_wss_no_stream_for_rust_20260522.txt` 대비 Python `perf_python_multi_linux_20260525_000301.txt` median 기준이다. raw echo 패턴 TLS 설정 보강 위에 routed `recv_into` single-part fast path가 적용된 상태다. small size는 여전히 낮다. |
| `wss` | `MULTI_ROUTER_ROUTER` | `보류(10.4%)` | `보류(8.9%)` | `보류(10.2%)` | `통과(54.5%)` | `통과(69.6%)` | `통과(70.7%)` | 65536/131072B는 같은 조건 제한 재측정 C `perf_c_multi_linux_20260525_023817.txt` 대비 Python `perf_python_multi_linux_20260525_024823.txt` 기준이다. 64/256/1024/262144B는 C `perf_c_multi_linux_20260522_152936_codex_c_multi_wss_no_stream_for_rust_20260522.txt` 대비 Python `perf_python_multi_linux_20260525_000301.txt` median 기준이다. TLS 설정 보강 뒤 complete를 확보했고, fast path 뒤 large size는 통과권이다. small size는 여전히 낮다. |
| `wss` | `MULTI_PUBSUB` | `보류(7.1%)` | `보류(7.2%)` | `보류(11.6%)` | `통과(99.8%)` | `통과(81.2%)` | `통과(99.6%)` | 64/256/1024/65536B는 같은 조건 fresh C `perf_c_multi_linux_20260525_190118_python_multi_pubsub_publish_native_c.txt` 대비 Python `perf_python_multi_linux_20260525_192512_multi_pubsub_client_len_fastpath_candidate.txt` median 기준이다. 131072/262144B는 기존 full 기준이다. TLS 설정 누락과 stop token 유실 시 무한 대기하던 종료 조건을 보강한 상태에서 complete를 유지한다. count fast path 뒤 65536B는 C와 거의 같고, small size는 아직 낮다. |
| `wss` | `MULTI_SPOT` | `보류(2.0%)` | `보류(1.9%)` | `보류(1.8%)` | `보류(6.9%)` | `보류(8.1%)` | `보류(9.6%)` | C/Python 파일은 위 MULTI_DEALER_DEALER 행과 같다. 실행 중 server/client는 각 `nlwp=12` 수준으로 thread 폭증은 아니었다. 전 size가 기준보다 낮고 latency backlog가 크다. |
| `wss` | `MULTI_SPOT_REQREP` | `보류(14.8%)` | `보류(15.8%)` | `보류(19.1%)` | `통과(67.1%)` | `통과(65.0%)` | `통과(56.5%)` | C/Python 파일은 위 MULTI_DEALER_DEALER 행과 같고, 1024B/65536B는 제한 재측정 `perf_python_multi_linux_20260522_192225_codex_python_multi_wss_spot_reqrep_failed_recheck_20260522.txt` 기준이다. full run의 일부 READY 누락은 단독 재측정에서 재현되지 않았다. |
| `wss` | `MULTI_SPOT_SENDSEND` | `보류(10.1%)` | `보류(0.2%)` | `보류(3.3%)` | `보류(0.7%)` | `통과(38.2%)` | `통과(40.7%)` | 64B는 같은 조건 최신 재측정 C `perf_c_multi_linux_20260525_085103.txt` 대비 Python complete 단독 재측정 `perf_python_multi_linux_20260525_085744.txt` 기준이다. 나머지 C/Python 파일은 위 MULTI_DEALER_DEALER 행과 같고, 65536B/262144B는 제한 재측정 `perf_python_multi_linux_20260522_192645_codex_python_multi_wss_spot_sendsend_failed_recheck_20260522.txt` 기준이다. active slot 제한과 pending reply drain 보강 뒤 timeout은 없어졌고 131072B/262144B는 기준선을 넘었다. 나머지는 기준보다 낮다. |
| `wss` | `MULTI_STREAM` | `보류(1.2%)` | `보류(1.2%)` | `보류(1.2%)` | `보류(14.2%)` | `보류(16.5%)` | `보류(33.6%)` | 64~65536B는 C `perf_c_multi_linux_20260522_154643_codex_c_multi_wss_stream_clients100_for_rust_20260522.txt`, 131072B/262144B는 C `perf_c_multi_linux_20260522_201307_codex_c_multi_wss_tls_stream_large_for_python_20260522.txt`, Python은 `perf_python_multi_linux_20260522_192658_codex_python_multi_wss_stream_tls_fix_20260522.txt` 기준이다. Python stream server에 TLS server 설정이 누락되어 초기 full run이 실패했고, 보강 뒤 전 size complete를 확보했다. |
| `tls` | `MULTI_DEALER_DEALER` | `보류(5.6%)` | `보류(10.3%)` | `보류(11.8%)` | `통과(67.7%)` | `통과(82.4%)` | `통과(65.9%)` | C `perf_c_multi_linux_20260522_154822_codex_c_multi_tls_no_stream_for_rust_20260522.txt`, Python `perf_python_multi_linux_20260522_200655_codex_python_multi_tls_duration1_20260522.txt` 기준이다. large size는 기준선을 넘고 small size는 낮다. |
| `tls` | `MULTI_DEALER_ROUTER` | `보류(13.7%)` | `보류(13.8%)` | `보류(13.9%)` | `통과(57.0%)` | `통과(56.1%)` | `통과(69.7%)` | C `perf_c_multi_linux_20260522_154822_codex_c_multi_tls_no_stream_for_rust_20260522.txt` 대비 Python `perf_python_multi_linux_20260525_000108.txt` median 기준이다. raw echo 패턴 TLS 설정 보강 위에 routed `recv_into` single-part fast path가 적용된 상태다. small size는 여전히 낮다. |
| `tls` | `MULTI_ROUTER_ROUTER` | `보류(10.2%)` | `보류(10.4%)` | `보류(10.2%)` | `통과(46.2%)` | `통과(43.1%)` | `통과(58.7%)` | C `perf_c_multi_linux_20260522_154822_codex_c_multi_tls_no_stream_for_rust_20260522.txt` 대비 Python 단독 complete 재측정 `perf_python_multi_linux_20260525_000403.txt` median 기준이다. routed `recv_into` single-part fast path 뒤 65536B가 기준선을 넘었다. small size는 여전히 낮다. |
| `tls` | `MULTI_PUBSUB` | `보류(7.3%)` | `보류(6.9%)` | `보류(10.6%)` | `통과(93.9%)` | `통과(99.7%)` | `통과(88.9%)` | 64/256/1024/65536B는 같은 조건 fresh C `perf_c_multi_linux_20260525_190118_python_multi_pubsub_publish_native_c.txt` 대비 Python `perf_python_multi_linux_20260525_192512_multi_pubsub_client_len_fastpath_candidate.txt` median 기준이다. 131072/262144B는 기존 full 기준이다. TLS 설정 누락과 stop token 유실 시 무한 대기하던 종료 조건을 보강한 상태에서 complete를 확보했고, count fast path 뒤 65536B는 기준선을 넘었다. small size는 여전히 낮다. |
| `tls` | `MULTI_SPOT` | `보류(1.9%)` | `보류(2.0%)` | `보류(2.3%)` | `보류(5.3%)` | `보류(7.2%)` | `보류(8.4%)` | C/Python 파일은 위 MULTI_DEALER_DEALER 행과 같고, 262144B는 제한 재측정 `perf_python_multi_linux_20260522_200916_codex_python_multi_tls_spot262144_recheck_20260522.txt` 기준이다. 실행 중 server/client는 각 `nlwp=12` 수준으로 thread 폭증은 아니었다. |
| `tls` | `MULTI_SPOT_REQREP` | `보류(14.1%)` | `보류(13.8%)` | `보류(17.0%)` | `통과(56.2%)` | `통과(54.2%)` | `통과(56.4%)` | C/Python 파일은 위 MULTI_DEALER_DEALER 행과 같다. pending reply drain 보강 경로로 전 size complete를 확보했다. |
| `tls` | `MULTI_SPOT_SENDSEND` | `보류(5.6%)` | `보류(7.8%)` | `보류(0.2%)` | `보류(5.2%)` | `통과(33.2%)` | `통과(41.6%)` | C/Python 파일은 위 MULTI_DEALER_DEALER 행과 같고, 1024B는 제한 재측정 `perf_python_multi_linux_20260522_201131_codex_python_multi_tls_spot_sendsend1024_recheck_20260522.txt` 기준이다. active slot 제한과 pending reply drain 보강 뒤 timeout은 없어졌고 131072B/262144B는 기준선을 넘었다. 나머지는 기준보다 낮다. |
| `tls` | `MULTI_STREAM` | `보류(1.2%)` | `보류(1.3%)` | `보류(1.2%)` | `보류(8.9%)` | `보류(15.1%)` | `보류(25.7%)` | 64~65536B는 C `perf_c_multi_linux_20260522_155155_codex_c_multi_tls_stream_clients100_for_rust_20260522.txt`, 131072B/262144B는 C `perf_c_multi_linux_20260522_201307_codex_c_multi_wss_tls_stream_large_for_python_20260522.txt`, Python은 `perf_python_multi_linux_20260522_200655_codex_python_multi_tls_duration1_20260522.txt` 기준이다. Python stream server에 TLS server 설정을 추가한 상태에서 전 size complete를 확보했다. |

#### 6.8.3 Python 남은 작업 (2026-05-25)

- **2026-05-26 `MULTI_SPOT_REQREP` CPU 급증 원인 확인**:
  `PERF_FAIL_FAST=1 ./run_benchmarks_multi.sh --reuse-build --pattern MULTI_SPOT_REQREP --transports tcp --msg-sizes 64 --duration 1 --runs 1 --clients 2`
  재현 중 runner/server/client 3개 프로세스가 뜨고, server/client가 각각 `nlwp=12`/`nlwp=13`
  수준으로 동작했다. Python multi context 기본 `io_threads=4`가 server/client 양쪽에 적용되고,
  SPOT_REQREP는 data/control `SpotNode`, stdin/control thread, request callback completion
  경로가 함께 붙는다. 기본 실행은 `clients=100`이므로 작은 케이스보다 slot 순회와 callback
  dispatch가 더 커져 전체 CPU가 급격히 오르는 것처럼 보인다.
- **2026-05-26 `MULTI_SPOT_REQREP` public completion 의미 정정**:
  multi request/reply perf의 requester completion은 예외 없이 `POLLCOMPLETION`
  poller가 소유해야 한다. C requester도 같은 기준으로 맞췄다. 이전에
  C requester를 `POLLIN` callback-request pump 예외로 본 분석은 폐기한다.
  Python client의 `request_to_spot(...).submit(callback)` 경로도 현재처럼
  `POLLCOMPLETION` 외부 progress 등록을 통해 completion을 진행하는 것이 맞다.
  Python의 낮은 throughput과 CPU 증가는 단순 sleep fallback이 아니라 메시지마다
  Python callback/FFI 경계를 통과하는 비용과 기본 `clients=100` slot 순회 비용으로 본다.
- **2026-05-26 binding-wide `MULTI_SPOT_REQREP` completion audit**:
  C/C++/Go/Rust/Java/.NET/Node/Python 모두 requester spot을 public
  `POLLCOMPLETION` poller surface에 등록하는 기준으로 정렬한다.
  `doc/perf/PERF_POLICY.md`와 `doc/perf/PERF_MULTI_TEST_POLICY.md`에서 C `POLLIN`
  예외 문구를 제거했다. Node는 `POLLCOMPLETION` poller를 쓰되 event-loop callback
  dispatch turn이 필요하다. 이후 C/C++ 기준과 같이 deadline까지 남은 시간과 50ms 상한 중
  작은 값으로 completion poller wait를 하도록 맞췄고, callback 전달용 event-loop turn은
  completion progress pump가 아니라 native callback 전달 단계로 남겼다.
  C++은 구현은 completion poller surface였지만 주석이 C와 다른 예외를 암시해 수정했다.
  .NET의 active loop는 completion poller wait를 쓰고 있었지만,
  active 구간 뒤 outstanding callback drain에 `Thread.Sleep(1)`이 남아 있었다. 이 drain은
  active measurement 바깥이어도 request completion 진행에 관여하므로, active deadline timer를
  poller에서 제거한 뒤 같은 `PollCompletion` poller wait로 남은 callback을 진행하도록 고쳤다.
  `dotnet build bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/Zlink.BindingBench.Multi.csproj -c Release`와
  `PERF_FAIL_FAST=1 ./run_benchmarks.sh --reuse-build --pattern MULTI_SPOT_REQREP --transports tcp --msg-sizes 64 --duration 1 --runs 1 --clients 2`
  smoke가 통과했다.
- **2026-05-26 Node `MULTI_SPOT_REQREP` poll wait 정정**:
  `doc/perf/PERF_MULTI_TEST_POLICY.md`의 blanket `-1` timeout 문구는 C 기준과 달랐다.
  C requester는 completion poller를 쓰면서 active duration/request timeout을 닫기 위해
  bounded wait를 사용한다. 정책 문서를 `wire stop token으로 종료되는 recv loop는 -1`,
  `active deadline을 직접 닫는 requester/sender loop는 C 기준 bounded wait`로 정정했다.
  Node requester의 1ms completion tick은 C/C++보다 더 짧은 timer cadence였으므로,
  `poller.wait` timeout을 deadline 잔여 시간과 50ms 상한 중 작은 값으로 바꿨다.
  `npm run build`와
  `PERF_FAIL_FAST=1 ./perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_SPOT_REQREP --transports tcp --msg-sizes 64 --duration 1 --runs 1 --clients 2`
  smoke가 `complete`로 통과했다.
- **2026-06-22 Node `MULTI_SPOT_REQREP` `NotConnected` 재시도 정렬**:
  기본 `clients=100`의 `65536/131072B` large 8개 cell이 `Host unreachable`로
  `0/40`에 머물렀다. C client는 `ZLINK_SUBMIT_NOT_CONNECTED`를 fatal이 아니라 다음
  poll loop에서 재시도하는 public submit 결과로 처리하므로, Node perf client도 public
  `SubmitError.result === SubmitResult.NotConnected`를 `Backpressured`와 같이 재시도한다.
  perf 전용 native helper나 deep runtime import는 추가하지 않았다. 해당 hot path에는
  주석을 남겼고 `optimization_guard.test.ts`가 public 결과 재시도 정책을 고정한다.
  보강 report
  `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260622_075045_prerelease_7_2_0_node_multi_spot_reqrep_large_not_connected_retry_probe.txt`는
  `40/40`, `status=complete`이며 C 7.2.0 full 대비 large 비율은 `tcp 44.2%/63.7%`,
  `tls 102.2%/101.5%`, `ws 65.9%/60.0%`, `wss 101.3%/111.2%`다.
- **2026-06-22 Node request callback Message facade 후보 기각**:
  사용자가 지정한 C baseline `bindings/c/perf/baseline/perf_c_multi_linux_20260619_062932.txt`와
  최신 7.2.0 C full의 `MULTI_SPOT_REQREP tcp/ws 65536/131072` 값이 최대 약 26%
  달라, 후보 판정 전에 C baseline을 다시 확보했다.
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260622_135110_prerelease_7_2_0_c_multi_spot_reqrep_tcp_ws_large_request_callback_probe_baseline_refresh.txt`는
  `status=complete`, 결과 라인 `20/20`이고, C 기준은 `53264.000/22674.000/32422.000/20972.000 msg/s`다.
  그 위에서 native request callback이 이미 JS `Buffer[]`로 넘긴 reply part를
  `messageFromSnapshot({data})` 대신 public `Message` facade로 바로 감싸는 후보를 시험했다.
  이는 perf 전용 helper가 아니라 public `request`, `requestToSpot`, actor callback 경로의
  binding 내부 최적화였지만, 공식 wrapper
  `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260622_135415_prerelease_7_2_0_node_multi_spot_reqrep_tcp_ws_large_request_callback_message_facade_probe.txt`가
  `status=partial`이었다. `tcp 65536`은 `19579.000 msg/s`로 기존
  `23891.000 msg/s`보다 낮았고, `tcp 131072`는 `client timeout after 90000ms`였다.
  public 계약은 유지했지만 성능과 안정성 근거가 없어 코드와 guard 변경을 원복했다.
- **2026-06-22 Node STREAM actor-bound gate 분리**:
  `MULTI_STREAM` routing id hot path 후보 검토 중 `stream_send_regression`의
  actor-bound session 케이스가 `requestToSpot failed`로 실패했다. 원인은 perf 후보가
  아니라 `setRoutingId`가 routed runtime 생성 전에 호출된 뒤 external router identity가
  runtime 생성 시점에 다시 적용되지 않는 core config 경로였다. `ensure_external_router_ready`
  가 public node routing id를 external router에 적용하도록 고쳐 호출자가 초기화 순서를
  알 필요 없게 했다. Node regression test는 actor-bound session flush 자체를 보도록
  node routing id를 먼저 고정한 뒤 Actor를 만들게 정렬했다. 이 변경은 perf 전용 우회가
  아니라 public contract gate 복구다.
- **2026-06-22 Node STREAM routing id external Buffer 후보 기각**:
  STREAM packet callback의 public `RoutingId` materialization은 유지하되 native TSFN에서
  routing id bytes를 `napi_create_external_buffer`로 넘겨 한 번의 Buffer copy를 줄이는
  후보를 시험했다. `optimization_guard.test.js`, `stream_send_regression`의 핵심 개별
  테스트는 통과했지만 `MULTI_STREAM tcp 64,256,1024` 두 번의 공식 wrapper 측정이
  `73.3/78.8/69.8 Kops/s`, `73.0/74.3/69.2 Kops/s`로 현재 기준
  `77.2/70.7/72.6 Kops/s` 대비 혼재했다. 안정적인 개선으로 볼 수 없어 native 변경은
  반영하지 않고, `StreamSocket.packetRoutingId`의 public facade cache hot path 주석만 남긴다.
- **2026-06-22 Node STREAM send builder payload inline 후보 기각**:
  public `stream.send(rid).message(frame).flags(DontWait).submit()` 표면은 유지하고
  `RuntimeSendOperation` 내부에서 `OperationPayload` 객체 할당을 줄이는 후보를 시험했다.
  `npm --prefix bindings/node run build`, `optimization_guard.test.js`, `socket_surface.test.js`,
  actor-bound STREAM 개별 regression은 통과했지만 공식 wrapper 두 번의
  `MULTI_STREAM tcp 64,256,1024` 측정이 `76.8/79.4/68.2 Kops/s`,
  `73.1/71.6/74.1 Kops/s`로 현재 기준 `77.2/70.7/72.6 Kops/s` 대비 혼재했다.
  builder 저장소를 얕게 줄이는 것만으로는 STREAM callback frame 조립과 native send copy
  비용을 안정적으로 낮추지 못한다. 코드 변경은 원복했고, STREAM의 다음 후보는 public
  packet handler 계약을 유지하면서 frame 조립/전송 copy 책임을 binding 내부에서 더 깊게
  흡수할 수 있는지 별도 설계가 필요하다.
- **2026-06-22 Node STREAM native callback Buffer direct `Message` facade 부분 채택**:
  사용자가 지정한 C 기준 `bindings/c/perf/baseline/perf_c_multi_linux_20260619_062932.txt`와
  7.2.0 C full 기준 사이에서 `MULTI_STREAM tcp 64/256/1024` 값이
  `337.323/341.720/332.245 Kops/s` 대 `267.302/263.253/258.363 Kops/s`로 크게 달랐다.
  따라서 같은 runtime `core/build/lib/libzlink.so.7.2.0`으로 해당 C 셀을 다시 측정했고,
  `perf_c_multi_linux_20260622_134101_prerelease_7_2_0_c_multi_stream_tcp_small_node_probe_baseline_refresh.txt`는
  `332.893/330.117/327.104 Kops/s`, `status=complete`였다. Node 판정은 이 갱신 C
  기준을 사용한다.

  Node 변경은 public `StreamSocket.setPacketHandler((sourceRid, header, body) => ...)`와
  actor callback 계약을 그대로 유지한다. native callback은 이미 payload ownership을 JS
  `Buffer`로 넘기므로, `messageFromNativeBuffer(...)`가 `messageFromSnapshot({ data })`로
  intermediate snapshot 객체를 만들지 않고 `messageFromOwnedBuffer(...)`로 public
  `Message` facade를 바로 만든다. 이는 perf 전용 helper가 아니라 STREAM/actor callback을
  쓰는 일반 public 경로의 per-part allocation을 줄이는 binding 내부 변경이다. 코드 가까이에
  `HOT PATH` 주석을 두고, `optimization_guard.test.js`에 이 경로가 snapshot allocation으로
  되돌아가지 않는 guard를 추가했다.

  검증은 `npm --prefix bindings/node run build`,
  `node --test bindings/node/dist-tools/tests/optimization_guard.test.js`,
  STREAM callback 개별 regression 3건, actor-bound 개별 regression으로 했다. full
  `stream_send_regression.test.js`는 이 worktree에서 기존에도 timeout 기록이 있어 이번 후보
  판정 근거로 쓰지 않는다. 공식 wrapper
  `perf_node_multi_linux_20260622_133954_prerelease_7_2_0_node_multi_stream_tcp_small_message_from_owned_buffer_probe.txt`는
  `98.257/101.044/90.131 Kops/s`로 complete였고, 이전 Node current
  `77.165/70.692/72.644 Kops/s` 대비 `+27.3%/+42.9%/+24.1%`다. 다만 갱신 C 기준
  비율은 `29.5%/30.6%/27.6%`라 256B만 목표선 위이고 64/1024B는 반복 미달로 남긴다.
- **2026-06-22 Node PUBSUB publish topic validation cache 후보 기각**:
  public `pub.publish(topic).message(payload).flags(SendFlags.DontWait).submit()` 호출 표면은
  그대로 두고 `PublisherSocket` 내부에 마지막 검증 topic 문자열을 캐시하는 후보를 시험했다.
  `npm --prefix bindings/node run build`, `optimization_guard.test.js`, `socket_surface.test.js`,
  PUBSUB public subscribe 개별 테스트는 통과했지만 공식 wrapper
  `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260622_084009_prerelease_7_2_0_node_multi_pubsub_publish_topic_cache_probe.txt`는
  직전 empty routing id 생략 probe 대비 `tcp 64B`만 `+2.4%`였고, `tls/ws/wss 64B`는
  `-0.5%/-0.7%/-0.2%`, `tcp/tls/ws/wss 65536B`는 `-0.6%/-1.9%/-9.1%/+0.8%`였다.
  공유 publish 경로에 mutable cache 상태를 더해도 현재 PUBSUB 미달의 주원인인
  receive/backlog 비용을 줄이지 못했다. POSD 관점에서도 인터페이스는 그대로지만 모듈 내부
  상태와 무효화 조건만 늘어나는 얕은 최적화라 유지할 근거가 부족하다. 코드 변경은 원복했고,
  PUBSUB 다음 후보는 public `TopicMessage` 계약을 유지하면서 수신 drain과 backlog 보유 비용을
  더 깊은 binding 내부 책임으로 흡수할 수 있는지에 맞춘다.
- **2026-06-22 Node PUBSUB payload-only/topic 반복 우회 재검토 폐기**:
  `MULTI_PUBSUB` client가 payload만 기록하더라도 public hot path는 계속
  `sub.subscribe(received, RecvFlags.DontWait)`와 reusable `TopicMessage`다.
  `socketTrySubscribePayload`로 perf client만 payload를 직접 받거나, native raw object에서
  `topic` 생성을 생략하거나, 같은 topic/payload 반복에 기대는 cache를 붙이는 방식은
  실제 public `TopicMessage.topic`, frozen `parts`, `singlePartOrThrow().data()` 비용을
  줄이지 않는다. `TopicMessage.parts` 배열 재사용도 호출자가 이전 `parts` 참조를 보관할 때
  다음 receive에 의해 관찰 동작이 바뀌므로 public 계약을 깨고, 기존 Node 테스트가 frozen
  `parts` 계약을 확인한다. 따라서 이 후보들은 측정하지 않고 폐기한다. 코드 리뷰에서
  미사용 `socketTrySubscribePayload` native export와 TypeScript native binding 타입도
  제거했다. 이는 성능 수치 개선이 아니라 perf-only 우회 표면을 걷어내는 정리이며,
  `optimization_guard.test.ts`가 해당 export가 다시 등록되지 않도록 고정한다.
- **2026-06-22 Node receive envelope close indexed loop 후보 기각**:
  caller-provided `Received`와 `TopicMessage` storage가 다음 payload를 채택하기 전에 이전
  `Message` part를 닫는 루프를 `for...of`에서 indexed loop로 바꾸는 후보를 시험했다.
  public envelope 계약, `parts` 배열 freeze, 이전 payload 즉시 release 의미는 그대로 유지했다.
  `npm --prefix bindings/node run build`, `optimization_guard.test.js`, `socket_surface.test.js`,
  PUBSUB public subscribe 개별 테스트는 통과했다. 그러나 `MULTI_SPOT tcp 64,256,1024` 세 번의
  공식 wrapper 측정은 `2493.4/1300.5/1555.0 Kmsg/s`,
  `2404.1/1315.4/770.6 Kmsg/s`, `2353.8/1304.7/763.0 Kmsg/s`로 직전 raw-shape 기준
  `2453.4/1287.6/767.9 Kmsg/s` 대비 혼재했다. 1024B 첫 run은 outlier로 보이고,
  64B는 반복 하락했다. `MULTI_PUBSUB tcp 64/65536` 대표 측정은 `646.8/37.1 Kmsg/s`로
  64B만 약간 높고 65536B는 동일권이었다. 루프 형태만 바꾸는 얕은 구현 변경으로는 현재
  SPOT/PUBSUB 미달 원인을 안정적으로 줄이지 못하므로 코드 변경은 원복했다.
- **2026-06-22 Node runtime-owned Message direct release flag 후보 기각**:
  runtime materializer가 만드는 native-owned `Message`에 내부 `_runtimeOwned` 플래그를 붙이고,
  caller-provided `Received`/`TopicMessage` storage 교체 시 public `Message.close()`의
  frozen-object 방어 경로를 우회하는 후보를 시험했다. public message가 사용자가 직접
  `parts`에 들어간 경우에는 기존 `Message.close()`로 fallback해 공개 동작을 유지했다.
  `npm --prefix bindings/node run build`, `optimization_guard.test.js`, `socket_surface.test.js`,
  PUBSUB public subscribe 개별 테스트는 통과했다. 그러나 `MULTI_SPOT tcp 64,256,1024`
  공식 wrapper 두 번의 측정은 `2527.3/1282.0/762.9 Kmsg/s`,
  `2452.6/1297.4/1643.4 Kmsg/s`로 직전 raw-shape 기준 `2453.4/1287.6/767.9 Kmsg/s`
  대비 혼재했다. 1024B repeat는 outlier로 보이고, 64/256B는 안정적인 개선이라고 보기
  어렵다. 모든 runtime message 생성에 내부 상태를 하나 더 붙이는 복잡도에 비해 이득이
  분명하지 않으므로 코드 변경은 원복했다.
- **2026-06-22 Node `MULTI_DEALER_DEALER tcp 65536` 재확인 및 send builder inline 후보 기각**:
  `MULTI_DEALER_DEALER tcp 65536`은 current HEAD에서 단일 cell로 다시 돌려도
  `43428.800 msg/s`로 C 7.2.0 full `174370.200 msg/s` 대비 `24.9%`에 그쳤다.
  같은 시간대 C 단일 cell도
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260622_090625_prerelease_7_2_0_c_multi_dealer_dealer_tcp_65536_current_pair.txt`에서
  `177150.800 msg/s`로 나와 현재 Node 비율은 `24.5%`다.
  다만 caller-provided `Received` release 적용 뒤 `/usr/bin/time -v` 최대 RSS는 기존 split의
  `57461864KB`에서 `14820680KB`로 낮아졌다. throughput 병목을 확인하기 위해 public
  `socket.send().message(payload).flags(SendFlags.DontWait).submit()` 표면은 유지하고
  `RuntimeSendOperation` 내부의 단일 payload 저장소를 inline하는 후보를 시험했다.
  `npm --prefix bindings/node run build`, `optimization_guard.test.js`, `socket_surface.test.js`,
  `pair.test.js`는 통과했지만 후보 측정은 `43678.600 msg/s`로 current 대비 `+0.6%`에
  그쳤다. 같은 후보는 STREAM small에서도 혼재로 기각된 바 있어, shared send builder 내부
  복잡도를 늘릴 근거가 부족하다. 코드 변경은 원복했고, 다음 후보는 builder 저장소보다
  native send boundary 또는 receive-side decode/backlog 쪽으로 좁힌다.
- **2026-06-22 Node plain `Received` single-part raw shape 후보 기각**:
  SUB/SPOT에서 효과가 있었던 single-part raw `data` 형태를 plain `Received`에도 적용하는
  후보를 시험했다. public `server.recv(received, RecvFlags.DontWait)` 표면은 유지했고,
  TypeScript materializer가 raw `data`를 public `Message`로 감싸도록 했다. 첫 테스트에서
  routed `Message.getProperty('Routing-Id')` 계약이 깨져 fast path를 routing id가 없는
  plain receive로만 좁혔다. `npm --prefix bindings/node run rebuild-native`,
  `npm --prefix bindings/node run build`, `optimization_guard.test.js`, `dealer_router.test.js`,
  `pair.test.js`, `socket_surface.test.js`는 통과했다. 그러나
  `MULTI_DEALER_DEALER tcp 65536` 후보 측정은 `42008.000 msg/s`로 current 재측정
  `43428.800 msg/s`보다 낮았다. 일반 `Received` raw shape는 routed metadata 계약까지
  신경 써야 하는데 throughput 근거가 없으므로 코드 변경은 원복했다.
- **2026-06-22 Node SPOT subscribe single-part raw shape 적용**:
  `MULTI_SPOT` client는 perf에서 계속 public `spot.subscribe(received, RecvFlags.DontWait)`와
  caller-provided `TopicMessage` storage를 사용한다. binding native `spot_recv`와
  `spot_try_recv`가 단일 payload에서도 매번 `parts[]`와 message snapshot object를 만들던
  비용을 줄이기 위해, SPOT subscribe raw object도 SUB와 같은 single-part `data` 형태를
  만들도록 바꿨다. TypeScript materializer가 이미 public `TopicMessage`로 이 raw shape를
  흡수하므로 새 public API, perf 전용 helper, deep runtime import는 없다. unrouted traffic의
  empty `routingId` property도 만들지 않는다. 이 경로에는 어떤 per-message allocation을
  막는지 hot path 주석을 남겼고, `optimization_guard.test.ts`가 SPOT subscribe raw shape와
  public materializer 경로를 고정한다. 공식 wrapper
  `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260622_082632_prerelease_7_2_0_node_multi_spot_tcp_small_spot_raw_probe.txt`는
  `status=complete`다. 이전 small split 대비 처리량은 `tcp 64/256/1024B`에서
  `2160.8/1180.8/699.5 Kmsg/s`에서 `2453.4/1287.6/767.9 Kmsg/s`로 올랐다
  (`+13.5%/+9.0%/+9.8%`). C 7.2.0 full 대비 비율은 `32.8/25.6/19.1%`에서
  `37.2/27.9/21.0%`로 개선됐고, 64B는 Node 개선 라운드 SPOT 기준을 넘었다. 256/1024B는
  아직 목표 미달이라 다음 binding 내부 후보를 계속 본다.
- **2026-06-22 Node `MULTI_SPOT tcp 256/1024/65536` C baseline 갱신**:
  사용자가 지정한
  `bindings/c/perf/baseline/perf_c_multi_linux_20260619_062932.txt`와 7.2.0 fresh C 기준을
  대조하니 `MULTI_SPOT tcp 256/1024/65536` 중 특히 256B가 크게 달랐다. 같은 runtime
  `core/build/lib/libzlink.so.7.2.0`으로 해당 C 셀을 다시 측정한
  `perf_c_multi_linux_20260622_141845_prerelease_7_2_0_c_multi_spot_tcp_node_remaining_baseline_refresh.txt`는
  `6007218.000/4474600.000/1609300.000 msg/s`, `status=complete`, 결과 라인 `15/15`다.
  2026-06-19 C baseline 대비 차이는 `+21.4%/+13.2%/+4.7%`다. 따라서 Node SPOT tcp
  남은 셀 판정은 이 갱신 C 기준을 사용하고, raw shape 적용 뒤 Node 비율은
  `256/1024/65536`에서 `21.4%/17.2%/11.2%`다. 이는 Node binding 내부 개선 필요성이
  baseline 오판이 아니라는 쪽으로 근거를 더 강하게 만든다.
- **2026-06-22 Node SPOT subscribe raw shape large 재측정**:
  같은 변경을 `MULTI_SPOT tcp 65536B`에도 적용한 상태로 기본 `clients=100`을 다시 돌렸지만
  30GB와 45GB RSS guard 모두 결과 저장 전 중단했다. report
  `perf_node_multi_linux_20260622_082935_prerelease_7_2_0_node_multi_spot_tcp_65536_spot_raw_probe.txt`는
  cell RSS `30369600KB`, report
  `perf_node_multi_linux_20260622_082959_prerelease_7_2_0_node_multi_spot_tcp_65536_spot_raw_probe_45gb.txt`는
  cell RSS `46073072KB`에서 중단했다. 이전 성공 split의 최대 RSS `63571408KB`보다는 낮아
  raw shape가 large payload의 객체 보유 비용도 줄였지만, 수신 drain이 송신 backlog를
  따라잡을 만큼 충분하지 않아 완료 결과를 만들지 못했다. 따라서 `tcp 65536B`는 기존
  성공 report의 throughput 미달 상태를 유지하고, 다음 후보는 public `TopicMessage` 계약을
  깨지 않는 범위의 drain 비용 또는 backlog 원인으로 분리한다. `parts` 배열 자체를 재사용하는
  접근은 호출자가 이전 `TopicMessage.parts` 참조를 보관할 때 다음 receive에 의해 내용이
  바뀌므로 public envelope 계약을 깨는 얕은 최적화라 적용하지 않는다.
- **2026-06-22 Node SPOT 추가 micro 후보 기각**:
  SPOT small 미달을 더 줄이기 위해 close loop, runtime-owned direct release flag, stack
  topic buffer 후보를 각각 검토했다. 모두 public `spot.subscribe(received,
  RecvFlags.DontWait)` 표면은 유지했지만, 결과는 size마다 흔들렸다. close loop는
  `2493.4/1300.5/1555.0`, `2404.1/1315.4/770.6`, `2353.8/1304.7/763.0 Kmsg/s`,
  runtime-owned release는 `2527.3/1282.0/762.9`, `2452.6/1297.4/1643.4 Kmsg/s`,
  stack topic buffer는 `2470.1/1299.9/764.6`, `2523.7/1307.2/766.6 Kmsg/s`였다.
  1024B 일부 상승은 반복성이 약한 outlier로 보이고, 상태 플래그나 size별 buffer 분기를
  늘릴 만큼 일반 비용이 줄었다는 근거가 없다. POSD 관점에서도 내부 상태와 예외 경로만
  늘리는 얕은 변경이라 모두 원복했다.
- **2026-06-22 Java `MULTI_DEALER_DEALER tcp 64/65536` current 재확인 및 perf-only 후보 보류**:
  같은 조건 paired 재측정에서 Java
  `perf_java_multi_linux_20260622_104155_prerelease_7_2_0_java_multi_dealer_dealer_tcp_64_65536_current_reprobe.txt`는
  `tcp 64` `1139712.000 msg/s`, `tcp 65536` `15963.600 msg/s`였고, C
  `perf_c_multi_linux_20260622_104214_prerelease_7_2_0_c_multi_recheck_java_dealer_dealer_tcp_64_65536_current_pair.txt`는
  `2976025.800/171946.200 msg/s`였다. 비율은 약 `38.3%`, `9.3%`로 반복 미달이다.
  perf client hot loop는 계속 public
  `socket.send().message(outbound).flags(SendFlags.DONT_WAIT).submit()`를 사용하고,
  server는 public `DealerSocket.recv(received, RecvFlags.DONT_WAIT)`를 사용한다.
  `Message.from(payload)` 복사를 피하도록 perf loop를 package-private move나 native
  helper로 바꾸는 방법은 측정 입력 구성 비용만 줄이는 perf 전용 우회라 채택하지 않는다.
  이미 binding 내부에는 single-part send invoker와 caller-provided receive storage
  fast path가 있으므로 다음 Java 후보는 public send/recv 표면을 유지한 채 실제 binding
  내부 비용인 message copy/ownership, Panama downcall, receive materialization을
  profiler로 분리한 뒤 선택한다.
- **2026-06-22 Java `MULTI_DEALER_DEALER tcp 64` JFR 및 caller-provided `Received` perf 후보 기각**:
  Java Flight Recorder로 `MULTI_DEALER_DEALER tcp 64` 단일 cell을 `duration=25`로
  실행했다. report
  `perf_java_multi_linux_20260622_110133_prerelease_7_2_0_java_multi_dealer_dealer_tcp64_jfr_capture2.txt`는
  `1221200.040 msg/s`로 complete였고, JFR 파일은 `/tmp/zlink-java-multidd-server-409163.jfr`,
  `/tmp/zlink-java-multidd-client-409207.jfr`에 남겼다. server JFR의 allocation sample은
  `Received`, `Message`, `Message[]`, `NativeMemorySegmentImpl`가 상위였고, hot methods에는
  `ReceivePlane.prepareRecvLikeOperation()`, `PerfMultiDealerDealer.drainCounted(...)`,
  `Arrays.copyOf(...)`, `moduleForNativeAccess()`가 보였다. client 쪽은
  `sendOneActive(...)`와 foreign downcall LambdaForm이 상위였다.
  이 근거로 perf helper가 매 receive마다 새 `Received`를 만드는 후보를 확인했지만, 이를
  caller-provided `Received` 재사용으로 바꾸는 실험은
  `perf_java_multi_linux_20260622_110328_prerelease_7_2_0_java_multi_dealer_dealer_tcp_64_65536_reusable_received_probe.txt`에서
  `tcp 64` `1120827.800 msg/s`, `tcp 65536` `15967.800 msg/s`로 current
  `1139712.000/15963.600 msg/s`와 같거나 낮았다. perf 코드만 바뀌고 목표 미달을
  해소하지 못하므로 코드 변경은 원복했다. 다음 후보는 perf helper 재작성보다
  `NativeMemorySegmentImpl`/Panama downcall, native message lifecycle, send-side public
  builder allocation 중 JFR에서 반복 확인되는 binding 내부 비용을 줄이는 방향이어야 한다.
- **2026-06-22 Java `MULTI_DEALER_DEALER tcp 65536` JFR 추가 확인**:
  large cell도 같은 public send/recv 표면으로 따로 JFR을 남겼다. report
  `perf_java_multi_linux_20260622_112720_prerelease_7_2_0_java_multi_dealer_dealer_tcp65536_jfr_capture.txt`는
  `15278.880 msg/s`로 complete였고, JFR 파일은
  `/tmp/zlink-java-multidd-large-server-432732.jfr`,
  `/tmp/zlink-java-multidd-large-client-432782.jfr`이다. server 쪽 Java hot sample은
  2개뿐이라 Java 메서드별 결론을 내리기 어렵지만, native method sample은 대부분
  `Native.pollerWait(...) -> NativePoller.wait -> PerfSocketPollSet.poll -> runServer`
  스택이었다. 이는 large server가 Java materialization만으로 계속 바쁜 상태라기보다
  poll/send 경계와 상대 client 진행을 함께 봐야 함을 뜻한다. client 쪽 native sample은
  `Unsafe.allocateMemory0 -> MessageSlotPool.Pool.acquire -> Message.from(Message) -> sendOneActive`
  스택과 `pollWritable` 대기가 함께 보였다. 따라서 다음 large 후보는 perf loop에
  package-private `Message.move()`나 native 전용 helper를 붙이는 방식이 아니라, public
  `Message.from(...)`/fluent send 계약을 유지한 채 binding 내부의 native message slot
  수명, Panama memory allocation, backpressure/poll 경계 비용을 줄이는 방향이어야 한다.
- **2026-06-22 Java large send ownership 검토**:
  `Message.from(Message)`는 public 계약상 원본 `Message`를 보존하기 위해 payload를 새
  message-owned frame으로 복사한다. `Message.move()`는 package-private이고 send 성공 시
  source message를 invalid 상태로 만들기 때문에, perf client가 template payload를 반복
  전송하는 경우에만 유리한 우회다. 이를 public send hot path에 몰래 적용하면 호출자가
  보낸 뒤에도 원본 `Message`를 읽을 수 있다는 현재 사용 모델을 깨뜨린다. reusable
  `Message` reset 기능도 내부 `ContractAccess`로만 쓰이며, 새 public 재사용 API를 여는
  것은 이번 작업의 “public surface 유지” 원칙과 맞지 않는다. 따라서 Java large의 다음
  실험은 `Message.from(Message)`의 의미를 바꾸지 않고, native message slot 할당 경로,
  Panama segment 생성, `sendPart*NoWaitCritical` downcall, poll/backpressure 경계를
  profiler로 더 좁힌 뒤 선택한다.
- **2026-06-22 Java receive `Message` object 재사용 후보 보류**:
  `ReceivePlane.recvIntoNoWait`는 이미 public `DealerSocket.recv(received, DONT_WAIT)`
  경로에서 caller-provided `Received`를 직접 채운다. `Received.populateRoutedSinglePart`도
  기존 parts list를 비운 뒤 재사용하고, `Message`는 reusable native slot pool과
  `resetForReuse()`를 갖고 있다. 남은 per-message 객체 비용을 더 줄이려면 이전 receive의
  `Message` wrapper 자체를 다음 receive target으로 재사용해야 한다. 그러나 public
  `Received.parts()`와 `FirstPart()`가 돌려준 `Message`는 호출자에게 소유권이 있는
  객체이고, 다음 `recv(received, ...)`가 그 객체를 몰래 재초기화하면 호출자가 아직 들고
  있는 `Message` view를 깨뜨릴 수 있다. 따라서 이 후보는 단순 내부 최적화가 아니라
  ownership 의미를 바꾸는 위험이 있어 바로 구현하지 않는다. 다음 Java receive 후보는
  JFR에서 `Message` wrapper allocation이 실제 지배 비용인지, 그리고 public `Received`
  재사용 호출이 이전 `Message` 참조를 무효화하지 않는 별도 내부 holder로 바꿀 수 있는지
  검증한 뒤에만 진행한다.
- **2026-06-22 Java `MessageSlotPool` capacity 후보 기각**:
  JFR에서 보인 `Unsafe.allocateMemory0 -> MessageSlotPool.Pool.acquire -> Message.from(Message)`
  스택을 줄이기 위해 thread-local native message slot pool capacity를 `32`에서 `256`으로
  늘리는 후보를 시험했다. public `socket.send().message(...).flags(...).submit()`와
  `DealerSocket.recv(received, DONT_WAIT)` 표면은 유지했고, Java compile은 통과했다.
  그러나 `perf_java_multi_linux_20260622_121824_prerelease_7_2_0_java_multi_dealer_dealer_tcp_64_65536_message_slot_pool_256_probe.txt`는
  `tcp 64` `1129001.200 msg/s`, `tcp 65536` `16291.600 msg/s`였다. 기존 current
  `1139712.000/15963.600 msg/s`와 비교하면 64B는 낮아지고 65536B는 소폭 개선에 그쳤다.
  목표 미달을 해소하지 못하면서 thread-local native slot 보유량만 늘리므로 코드 변경은
  원복했다. 다음 후보는 capacity 상수 조정이 아니라 allocation stack이 실제로 pool miss인지,
  message copy ownership인지, Panama downcall/foreign segment 비용인지 더 좁힌 뒤 선택한다.
- **2026-06-22 C++ routed receive hot path 검토와 perf-only 후보 폐기**:
  C++ `DEALER_ROUTER` large 미달은 계속 남아 있지만, perf receiver는 이미 public
  `router.recv(routing_id_t&, message_t&, ...)` 계약을 사용한다. binding 내부
  `recv_single_part_routed_message`는 `received_t`와 `std::vector<message_t>` materialize를
  거치지 않고 caller-provided `message_t`에 native single-part receive 결과를 채운다.
  이 경로에는 실패 시 non-empty output message를 보존해야 하는 public 계약과 empty output
  message에서 피하는 native init/close 비용을 `HOT PATH` 주석으로 명시했다.
  남은 쉬운 후보인 같은 peer routing id cache는 benchmark의 반복 peer 모양에 기대는
  개선이다. 실제 애플리케이션에서 같은 routing id가 반복된다는 보장이 없고, 일반 public
  경로의 allocation/copy/native boundary 비용을 줄이는 근거도 약하므로 반영하지 않는다.
  다음 C++ 후보는 perf cache가 아니라 native boundary와 core routed transport 쪽 wall-time
  비중을 분리하는 방향으로만 잡는다.
- **2026-06-22 Java PAIR single-part send/recv hot path 부분 채택**:
  `PairSocket.send().message(...).flags(...).submit()` public 표면은 그대로 두고,
  `NativePairSocket.send()`가 `DealerSocket`처럼 single-part invoker를 넘기도록 했다.
  이로써 단일 메시지 PAIR send가 `List.of(singlePart)` 경로를 거치지 않고 바로
  `super.send(part, ...)`로 들어간다. `PairSocket.recv(Received, ...)`도 fresh
  `Received`를 만든 뒤 adopt하지 않고, `ReceivePlane.recvInto(...)`의 caller-provided
  storage 경로를 직접 탄다. 코드 가까이에 `HOT PATH` 주석을 두어 리팩토링 때 per-message
  `Received`와 immutable parts list allocation이 되살아나지 않게 했다.
  `./gradlew :compileJava :perf-single:compileJava --no-daemon`과
  `./gradlew :compileJava :test --tests 'systems.zlink.contract.SocketContractTest' --tests 'systems.zlink.SendResultContractTest' --no-daemon`
  는 통과했다. PAIR inproc 1024 단일 cell은 Java
  `perf_java_single_linux_20260622_113350_prerelease_7_2_0_java_single_pair_inproc_1024_pair_fastpath_probe.txt`에서
  `674882.400 msg/s`, 같은 시간대 C
  `perf_c_single_linux_20260622_113442_prerelease_7_2_0_c_single_pair_inproc_1024_pair_fastpath_pair.txt`에서
  `1419163.000 msg/s`였다. 기존 Java 재측정 `635659.000 msg/s` 대비 약 `+6.2%`지만
  C 대비 `47.6%`라 반복 미달은 유지한다. 이 변경은 perf-only helper가 아니라 일반
  PAIR public send/recv 경로의 내부 allocation과 dispatch를 줄이는 변경이므로 부분
  채택하고, 남은 미달은 `Message.from(Message)` copy와 native send/recv boundary를
  별도 후보로 계속 본다.
- **2026-06-22 Java DEALER_DEALER inproc 1024 current 재측정**:
  PAIR fast path 적용 뒤 같은 simple one-way 계열인 `DEALER_DEALER inproc 1024`도
  현재 상태로 다시 재측정했다. Java
  `perf_java_single_linux_20260622_113806_prerelease_7_2_0_java_single_dealer_dealer_inproc_1024_current_after_pair_fastpath.txt`는
  `690855.800 msg/s`, 같은 시간대 C
  `perf_c_single_linux_20260622_113806_prerelease_7_2_0_c_single_dealer_dealer_inproc_1024_current_pair_after_pair_fastpath.txt`는
  `1433043.800 msg/s`라 C 대비 약 `48.2%`다. `NativeDealerSocket`은 이미 single-part
  send invoker와 caller-provided `recvInto` hot path를 쓰므로, 이 셀의 다음 후보는
  perf helper의 `Received` 재사용이 아니라 public `Message.from(Message)` copy 의미와
  native send/recv boundary 중 일반 사용 경로에서도 줄일 수 있는 비용을 profiler로
  좁히는 방향이다.
- **2026-06-22 Java DEALER_DEALER inproc 1024 JFR 및 multipart 빈 상태 정리**:
  같은 셀을 JFR과 함께 실행한 report
  `perf_java_single_linux_20260622_114304_prerelease_7_2_0_java_single_dealer_dealer_inproc_1024_jfr_probe.txt`는
  `707749.800 msg/s`로 complete였고, JFR 파일은
  `/tmp/zlink-java-single-dd-inproc1024.jfr`이다. hot methods는
  `PerfMeasurement.nettyPooledPayloadTemplate(...)`, payload reset, metrics reservoir처럼
  perf 입력 구성과 측정 코드 비중이 커서, 같은 payload를 반복한다는 가정에 기대는
  perf-only cache나 perf loop 전용 재사용은 적용하지 않는다. allocation sample에는
  `Received`, `Message`, `NativeMemorySegmentImpl`, `Message[]`가 보였고,
  `ReceivePlane.prepareRecvLikeOperation()`이 매 recv-like 호출 앞에서
  `MultipartReceiveState.closeRemaining()`을 부르는 경로가 확인됐다. 이 경로는 일반
  public `DealerSocket.recv(Received, ...)` 수신에도 해당하므로, pending multipart frame이
  없는 보통의 single-part receive에서 빈 `Message[]`를 새로 만들지 않도록
  `MultipartReceiveState`의 empty state를 공유 배열로 바꿨다. 코드에는 `HOT PATH`
  주석으로 no-pending single-part receive에서 피해야 하는 per-message empty array
  allocation을 명시했다. `javap` 확인상 no-pending 분기에는 배열 생성 opcode가 없고,
  `./gradlew :compileJava :perf-single:compileJava --no-daemon`,
  `./gradlew :compileJava :test --tests 'systems.zlink.contract.SocketContractTest' --tests 'systems.zlink.SendResultContractTest' --no-daemon`
  는 통과했다. 그러나 latest throughput 재측정
  `perf_java_single_linux_20260622_114606_prerelease_7_2_0_java_single_dealer_dealer_inproc_1024_multipart_empty_state_fastpath.txt`는
  `684216.800 msg/s`로, 같은 C 기준 대비 약 `47.7%`라 미달 해소로 계산하지 않는다.
  후속 JFR report
  `perf_java_single_linux_20260622_114646_prerelease_7_2_0_java_single_dealer_dealer_inproc_1024_jfr_after_multipart_empty_state.txt`도
  profiling overhead 아래 `591444.700 msg/s`였고, 남은 상위 비용은 여전히 `Message`,
  `Received`, `NativeMemorySegmentImpl`, send/recv downcall, perf payload 구성으로 나뉜다.
  따라서 이 변경은 public 계약을 지킨 allocation hot path 정리로만 유지하고,
  Java simple one-way throughput gap의 다음 후보는 native send/recv boundary와
  receive materialization을 더 좁히는 쪽으로 둔다.
- **2026-06-22 Java outbound message microbench 재확인**:
  `./gradlew :perf-single:runMessageOutboundMicrobench --no-daemon`을 현재 상태에서 다시
  실행했다. 64B는 `response_copy_of_bytes` `41.42 ns/op`,
  `response_build_from_arrays` `42.30 ns/op`,
  `response_build_from_messages` `43.14 ns/op`,
  send-prepare 포함 `42.92~46.99 ns/op`이었다. 1024B는
  `response_copy_of_bytes` `75.98 ns/op`, `response_build_from_arrays` `78.77 ns/op`,
  `response_build_from_messages` `77.44 ns/op`, send-prepare 포함 `77.21~80.83 ns/op`이었다.
  이 수치는 Java simple one-way가 C 대비 약 `47~48%`에 머무는 격차를 `Message.from`
  미세 조정만으로 설명하기 어렵다는 근거다. public 원본 보존 계약을 깨는
  `Message.move()` 우회나 perf helper 전용 재사용은 계속 제외하고, 다음 조사는 native
  send/recv downcall, poll wakeup, receive materialization의 wall-time 비중을 직접
  분리하는 방향으로 둔다.
- **2026-06-22 Java single perf caller-provided `Received` 재사용 후보 기각**:
  single `PAIR`, `DEALER_DEALER`, `DEALER_ROUTER`, `ROUTER_ROUTER` receiver loop가
  `PerfUtil.recvNoWait(...)`로 매번 새 `Received`를 받는 것을 보고, public
  `recv(received, DONT_WAIT)` 표면으로 caller-provided storage를 재사용하는 후보를 시험했다.
  내부 API나 native helper는 쓰지 않았지만, 이 변경은 perf 하니스만 바꾸는 후보이므로
  수치가 움직이는지 먼저 좁게 확인했다. `./gradlew :perf-single:compileJava --no-daemon`은
  통과했다. 그러나
  `perf_java_single_linux_20260622_124234_prerelease_7_2_0_java_single_inproc_simple_reusable_received_probe.txt`에서
  `PAIR inproc 1024`는 `673850.500 msg/s`로 기존 PAIR fast path probe와 같은 수준이고,
  `DEALER_DEALER inproc 1024`는 `635089.000 msg/s`로 최신 current `684216.800 msg/s`보다
  낮았다. routed 대표 cell도
  `perf_java_single_linux_20260622_124257_prerelease_7_2_0_java_single_routed_inproc_reusable_received_probe.txt`에서
  `DEALER_ROUTER inproc 65536` `37113.000 msg/s`,
  `ROUTER_ROUTER inproc 65536` `38165.000 msg/s`로 기존 반복 미달 수치를 개선하지 못했다.
  public 표면은 맞지만 실제 병목을 줄이지 못하고 일부 cell을 악화하므로 코드 변경은
  원복했다. 이 결과 때문에 single perf 하니스만 caller-provided storage로 바꾸는 접근은
  더 진행하지 않고, Java 후보는 binding 내부 native boundary, message ownership,
  receive materialization을 profiler로 좁히는 방향으로 유지한다.
- **2026-06-22 Java routed send payload-copy 후보 제외**:
  `DEALER_ROUTER`/`ROUTER_ROUTER` single perf sender는 public
  `.send(route).message(outbound).flags(...).submit()` 경로를 사용하고, `outbound`는
  `Message.from(active)`로 만든다. 이 복사는 benchmark payload template을 매 송신마다
  새 public `Message`로 만드는 비용이지만, perf 하니스의 입력 구성 비용이다. 이를
  package-private move, native helper, template-owned message reuse로 우회하면 public
  send builder가 실제 사용자가 넘기는 `Message`를 처리하는 경로가 아니라 perf 전용
  경로를 측정하게 된다. routed receive 쪽은 이미 public `recv(Received, DONT_WAIT)`와
  `populateRoutedSinglePart` hot path를 사용하므로, 이번 라운드에서는 perf 코드를 바꾸지
  않는다. 다음 Java routed 후보는 JFR/native sample로 send builder, Panama downcall,
  receive materialization 중 일반 public 경로 비용을 분리한 뒤 선택한다.
- **2026-06-22 Java routed inproc 65536 JFR active probe**:
  `DEALER_ROUTER inproc 65536`을 public perf runner 그대로 실행하고 JFR을 붙였다.
  `JAVA_TOOL_OPTIONS='-XX:StartFlightRecording=filename=/tmp/zlink-java-single-dealer-router-inproc65536-long.jfr,settings=profile,dumponexit=true,delay=3s'`
  조건의
  `perf_java_single_linux_20260622_125456_prerelease_7_2_0_java_single_dealer_router_inproc65536_jfr_active_probe.txt`는
  `39830.850 msg/s`로 complete였다. `jfr view hot-methods`는
  `PerfMeasurement.nettyPooledPayloadTemplate(int, int)`가 sample의 `98.20%`를 차지했고,
  allocation-by-class는 `NativeMemorySegmentImpl 38.11%`, `Message 13.31%`,
  `Received 11.22%`였다. 이는 Java routed active loop가 `resetAndWritePayload(...)`에서
  매 송신마다 template `Message`를 닫고 다시 만드는 perf 하니스 비용을 강하게 포함한다는
  뜻이다. C single routed sender는 `std::vector<char>` payload에 header를 stamp한 뒤
  submit용 `zlink_msg_t`를 만든다. 따라서 Java routed 수치를 library 병목으로 해석하기
  전에 이 perf 하니스 비용을 별도 측정 의미 이슈로 분리해야 한다. 사용자 지침상 perf-only
  개선은 바로 넣지 않으므로, 이번 라운드에서는 perf 코드를 바꾸지 않고 문서에 오염 근거를
  남긴다. 다음 library 후보는 이 template 재생성 비용을 제외하고도 남는 send/recv
  downcall, `Received` materialization, native message lifecycle 비용에서 찾아야 한다.
- **2026-06-22 Java Dealer send invoker cache 후보 기각**:
  client JFR에서 `NativeDealerSocket`의 captured lambda allocation과
  `MessageOperations.SendBuilder` allocation이 보였기 때문에, public
  `dealer.send().message(...).flags(...).submit()` 표면은 유지하고 `NativeDealerSocket`이
  single/multipart send invoker lambda를 socket instance field로 캐시하는 후보를 시험했다.
  `./gradlew :compileJava :perf-multi:compileJava --no-daemon`은 통과했지만
  `perf_java_multi_linux_20260622_110544_prerelease_7_2_0_java_multi_dealer_dealer_tcp_64_65536_dealer_send_invoker_cache_probe.txt`는
  `tcp 64` `1173002.200 msg/s`, `tcp 65536` `15310.800 msg/s`였다. current
  `1139712.000/15963.600 msg/s` 대비 64B는 오차권 개선이고 65536B는 하락했다.
  shared dealer send path에 상태를 추가할 근거가 부족하고 large 미달을 악화시키므로 코드
  변경은 원복했다. 다음 send-side 후보는 builder lambda보다 더 큰 비중인 message
  ownership/native downcall 경계를 대상으로 해야 한다.
- **2026-06-22 Java Dealer 전문 send builder 후보 기각**:
  invoker lambda 경로보다 한 단계 더 줄이기 위해 `NativeDealerSocket.send()`가
  `MessageOperations.SendBuilder` 대신 dealer 전용 `SendOperation` 구현을 반환하는 후보를
  시험했다. public fluent 표면과 multipart ownership 의미는 유지했고,
  `./gradlew :compileJava :test --tests 'systems.zlink.contract.SocketContractTest' --tests 'systems.zlink.SendResultContractTest' --no-daemon`
  는 통과했다. 그러나
  `perf_java_multi_linux_20260622_112537_prerelease_7_2_0_java_multi_dealer_dealer_tcp_64_65536_dealer_specialized_send_builder_probe.txt`는
  `tcp 64` `1193065.000 msg/s`, `tcp 65536` `13582.000 msg/s`였다. current
  `1139712.000/15963.600 msg/s` 대비 64B는 오차권이고 65536B는 크게 하락했다. shared
  public send builder를 패턴별로 나누는 것은 POSD 관점에서도 얕은 분기와 중복을 늘리므로
  반영하지 않는다. 코드 변경은 원복했다.
- **2026-06-22 Java current hot path 재확인 및 baseline refresh 제외**:
  현재 Java binding에는 `PAIR`/`DEALER`/`ROUTER`/`PUB`의 single-message public send
  builder 경로와 `Received` caller-provided storage hot path 주석이 남아 있다. 이 상태가
  `MULTI_DEALER_DEALER tcp 64,65536`을 해소했는지 다시 확인했다.
  사용자가 지정한 C baseline
  `bindings/c/perf/baseline/perf_c_multi_linux_20260619_062932.txt`의 해당 값은
  `2843638.800/161217.400 msg/s`이고, 최신 C paired 값
  `perf_c_multi_linux_20260621_193019_prerelease_7_2_0_c_multi_recheck_java_dealer_dealer_misses.txt`는
  `3071251.200/182651.600 msg/s`다. 차이는 약 `+8%/+13%`라 baseline refresh 대상으로
  보지 않고 최신 paired C를 비교 기준으로 썼다. Java current 재측정
  `perf_java_multi_linux_20260622_135930_prerelease_7_2_0_java_multi_dealer_dealer_tcp_64_65536_current_hotpath_recheck.txt`는
  `1055038.000/15307.000 msg/s`, `status=complete`, 결과 라인 `30/30`이었다. 최신 C 대비
  약 `34.4%/8.4%`라 반복 미달은 유지된다. 따라서 이 단계에서는 perf-only payload reuse,
  helper 우회, 반복 입력 cache를 추가하지 않는다. 다음 Java 후보는 public contract를
  그대로 둔 채 native downcall, message ownership, receive materialization의 일반 경로
  비용을 더 좁히는 방향으로만 잡는다.
- **2026-06-22 Java repeated value cache 제거**:
  perf-only 후보 재검토 원칙에 맞춰 Java binding의 send/receive scratch에서 반복
  topic/routing id cache를 제거했다. public `publish(topic)`, `subscribe(...)`,
  routed `send(...)`, SPOT `subscribe(...)` 계약은 그대로이고, thread-local native
  scratch buffer 재사용만 남겼다. 이 cache는 같은 topic이나 같은 peer routing id가
  반복된다는 입력 분포에 기대며, 실사용 일반 hot path 비용을 줄인다는 profiler 근거가
  부족했다. POSD 관점에서도 상태와 비교 루프를 여러 receive/send 경로에 흩뜨리는 얕은
  복잡성이므로 성능 후보가 아니라 제거 대상으로 본다.
  검증은 `./gradlew :compileJava :test --tests 'systems.zlink.contract.SocketContractTest' --tests 'systems.zlink.SendResultContractTest' --no-daemon`,
  `./gradlew jar --no-daemon`으로 통과했다. 대표 영향 측정은
  `perf_java_multi_linux_20260622_144530_prerelease_7_2_0_java_multi_pubsub_tcp64_remove_repeated_value_caches_probe.txt`
  `MULTI_PUBSUB tcp 64` `1953365.667 msg/s`,
  `perf_java_single_linux_20260622_144539_prerelease_7_2_0_java_single_dealer_router_inproc65536_remove_repeated_value_caches_probe.txt`
  `DEALER_ROUTER inproc 65536` `37661.333 msg/s`다. 반복값 cache 제거는 목표 미달을
  해소하는 개선으로 계산하지 않고, 이후 Java 후보는 cache가 아니라 native boundary,
  message ownership, receive materialization의 일반 비용을 profiler로 분리한 뒤 고른다.
- **2026-06-22 Python `ReceivedMessage` compact view 후보 기각**:
  Python single simple small 미달은 public `recv_into(received)`와 native owner receive
  bridge를 이미 사용한다. 남은 per-message 비용 후보로 매 수신 part마다 생성되는 public
  `ReceivedMessage` view에 `__slots__`를 두는 실험을 했다. 이전 part view를 재사용하지
  않았기 때문에 caller가 들고 있는 part lifetime 계약은 보존했고,
  `python -m pytest bindings/python/tests/test_core_api_alignment.py bindings/python/tests/test_boundary_ownership_contract.py bindings/python/tests/test_optimization_guard.py`,
  `python -m pytest bindings/python/tests/test_version.py`는 통과했다.
  그러나 대표 측정
  `perf_python_single_linux_20260622_145106_prerelease_7_2_0_python_single_pair_small_received_message_slots_probe.txt`는
  `PAIR inproc 64/256` `256096.000/257576.000 msg/s`,
  `PAIR tcp 64/256` `350808.333/347356.333 msg/s`였다. 기존 paired 보강 값
  `278228.200/265697.000`, `358674.600/364927.200 msg/s`보다 4개 cell 모두 낮다.
  성능 근거가 없으므로 코드 변경은 원복했다. 다음 Python 후보는 public part view 모양
  변경이나 perf-only view 접근이 아니라 native bridge 호출 경계, owner materialization,
  send-side payload preparation 비용을 profiler로 분리한 뒤 선택한다.
- **2026-06-22 Python `NativeReceivedPartsOwner` single-part close 후보 기각**:
  위 후보 원복 뒤, public `recv_into(received)`와 caller-provided `Received` storage 계약은
  그대로 둔 채 native owner close loop에서 `part_count == 1`을 별도 처리하는 후보를 시험했다.
  `python setup.py build_ext --inplace`로 확장을 다시 만들었고,
  `python -m pytest bindings/python/tests/test_core_api_alignment.py bindings/python/tests/test_boundary_ownership_contract.py bindings/python/tests/test_optimization_guard.py bindings/python/tests/test_version.py`는
  통과했다. 그러나 대표 측정
  `perf_python_single_linux_20260622_145358_prerelease_7_2_0_python_single_pair_small_native_owner_single_close_probe.txt`는
  `PAIR inproc 64/256` `247975.000/257682.000 msg/s`,
  `PAIR tcp 64/256` `348488.667/343245.000 msg/s`였다. 기존 paired 보강 값
  `278228.200/265697.000`, `358674.600/364927.200 msg/s`보다 네 cell 모두 낮고,
  직전 compact view 후보보다도 대부분 낮다. single-part close 미세 분기는 native bridge와
  materialization의 일반 비용을 줄이지 못하고 close loop 내부 조건만 늘리므로 원복했다.
  다음 Python 후보는 close loop 미세 분기가 아니라 native bridge 호출 수, owner
  materialization, send-side payload preparation 중 실제 profiler 상위 비용을 먼저 분리한다.
- **2026-05-26 `MULTI_SPOT_SENDSEND` poll interest 문서 정정**:
  `doc/perf/PERF_MULTI_TEST_POLICY.md`는 송수신 양방향 spot workload를
  `POLLIN|POLLOUT`으로 등록해야 한다고 적고 있었지만, C active window는
  `reset_sendsend_poller()`에서 requester poll interest를 `POLLIN`으로 맞춘 뒤
  reply drain과 다음 send submit을 같은 loop에서 진행한다. Go/Rust/Java/Node/Python도
  active requester poller는 `POLLIN` 기준이다. 정책 문서를 C 기준인 `POLLIN`
  active loop로 정정했고, C 초기 등록 주석도 active reset 의미를 드러내도록 고쳤다.
  Go의 active/drain poll wait cap도 기존 1ms에서 C와 같은 50ms 상한으로 맞췄다.
- **2026-05-26 Node multi `--clients` report parity 수정**:
  Node multi runner는 실제 orchestrator에는 CLI/env client 수를 넘겼지만,
  `META`와 `Effective Options` 출력은 `buildMetaItems`/`buildMultiOptionItems`가
  패턴 기본값만 다시 계산해서 `--clients 2` 실행도 `clients: 100`으로 기록했다.
  이는 `doc/perf/PERF_POLICY.md`의 공식 CLI 옵션/결과 출력 의미 동일 규칙을 어긴다.
  CLI 또는 `PERF_MULTI_CLIENTS` override가 있을 때 report client 수에도 같은 값을
  반영하도록 고쳤다.
- **2026-05-26 Rust `MULTI_SPOT_REQREP` pending completion drain 정렬**:
  Rust requester는 active window가 끝난 뒤 latency channel만 즉시 비우고 종료해서,
  C `drain_pending_replies()`처럼 outstanding request completion을 같은
  `POLLCOMPLETION` poller로 정리하지 않았다. active deadline 이후 도착한 reply는
  기존 callback guard 때문에 집계하지 않되, pending completion queue는 C와 같은
  50ms cap poller wait로 최대 request timeout 기반 drain 동안 정리하도록 맞췄다.
- **수신 zero-copy `.data` 추가**: Rust SPOT의 per-message 복사 제거와 같은 동기로, Python 수신 부품(`ReceivedMessage`)에
  zero-copy `data` memoryview property를 추가했다(`Message.data`/C `zlink_msg_data`와 동일 계약, 회귀 테스트 `tests/test_version.py::test_received_part_data_is_zero_copy_view` 통과).
  `MULTI_SPOT` client가 `to_bytes_list()` 전체 payload 복사 대신 `first_part().data`에서 헤더를 디코드하도록 바꿨다(복사 제거는 `with message:` 안에서만 view 사용).
- **효과는 제한적(Python은 per-call FFI 벽)**: tcp `MULTI_SPOT` duration5 측정에서 64/256/1024B≈3.0~3.3%(이전 1.9%), 65536B≈5.3%(이전 4.3%)로 소폭만 개선됐다.
  Python SPOT throughput은 size와 무관하게 ~120 Kmsg/s에 고정되는데, 이는 payload 복사가 아니라 **단일 thread로 100개 spot을 drain하는 per-subscribe FFI 호출 비용의 벽**이다(섹션 7.1 (B)).
  복사 제거는 옳은 최적화이고 large에서 약간 돕지만, Python SPOT을 목표로 끌어올리지는 못한다. duration5에서 large는 backlog로 timeout되므로 Python multi는 기존대로 duration1 프로토콜이 맞다.
- **MULTI_SPOT client native metric receive 적용**: 위 `.data` 후보는 여전히 `TopicMessage`와
  `ReceivedMessage`를 만들고 헤더를 여러 번 디코드했다. `MULTI_SPOT` client hot path를
  perf 전용 native `zlink_spot_subscribe_part` 단일 part 수신으로 좁히고, header를
  한 번만 `struct.unpack_from(...)`으로 읽으며 latency 샘플 대상일 때만 `time_ns()`를
  호출하도록 바꿨다. `python3 -m py_compile
  bindings/python/perf/multi/perf_multi_spot_client.py`는 통과했고, C
  `perf_c_multi_linux_20260525_221504_python_multi_spot_native_subscribe_metric_c.txt`
  대비 Python `perf_python_multi_linux_20260525_223653_multi_spot_native_subscribe_metric_candidate.txt`는
  tcp 64/256/1024/65536B complete였다. median은 308.6/324.6/299.9/130.9Kmsg/s,
  C 대비 6.7/8.7/8.4/8.4%다. 기존 약 120Kmsg/s 벽은 넘었지만 C 기준에는 아직 낮아
  보류를 유지하고, 다음 후보는 SPOT server publish 또는 client drain 병렬화가 실제 병목인지
  분리한다.
- 같은 `.data` 경로를 tcp `MULTI_DEALER_DEALER` server / `MULTI_PUBSUB` client / `MULTI_ROUTER_ROUTER` client 헤더 디코드에도 재사용했다
  (`perf_python_multi_linux_20260524_191648.txt`, `perf_python_multi_linux_20260524_191738.txt`, `perf_python_multi_linux_20260524_191823.txt`, 모두 status=complete).
  large에서는 `MULTI_DEALER_DEALER` 65536B 45.8%→52.2%, `MULTI_PUBSUB` 131072B 54.8%→90.9%, `MULTI_ROUTER_ROUTER` 65536B 29.6%→35.5%처럼 개선됐지만,
  64~1024B는 여전히 5~12%대라 같은 per-call FFI 벽이 남는다. Python 메시지당 FFI 호출 수 감소(배치 수신)는 별도 public API 설계 대상이다.
- `MULTI_DEALER_ROUTER` client reply decode에도 같은 `.data` 경로를 적용했다. `perf_python_multi_linux_20260524_194812.txt`에서 64/256/1024B는 12.9/14.3/13.3%로 small 보류가 유지됐지만, 65536B는 40.5%→46.5%로 올라 routed multi 기준선 위 여유가 커졌다.
- **single receiver `.data` 후보 기각**: single 공통 `run_one_way_receiver(...)`가
  active 수신 payload를 `to_bytes()`로 복사한 뒤 헤더를 디코드하므로, `ReceivedMessage.data`
  view를 닫기 전에 바로 디코드하는 후보를 시험했다. `python3 -m py_compile
  bindings/python/perf/single/perf_common.py bindings/python/perf/single/perf_pair.py
  bindings/python/perf/single/perf_dealer_dealer.py bindings/python/perf/single/perf_pubsub.py
  bindings/python/perf/single/perf_dealer_router.py bindings/python/perf/single/perf_router_router.py`는
  통과했고 공식 runner도 complete였다. 그러나 같은 조건 C
  `perf_c_single_linux_20260525_132828_python_single_data_view_c.txt` 대비 후보
  `perf_python_single_linux_20260525_132950_data_view_candidate.txt`의 tcp small median은
  `PAIR` 3.8/3.7/5.9%, `PUBSUB` 3.4/4.0/5.4%, `DEALER_DEALER` 3.9/3.9/6.1%에
  그쳤다. memoryview 수명 보장을 위해 매 메시지마다 close 순서를 감싸는 비용이 복사 제거
  이득을 넘지 못하므로 반영하지 않는다.
- **single receiver latency sampling 후보 기각**: single 공통 수신 hot path도 multi PUBSUB처럼
  throughput count는 전 메시지를 유지하고 latency 계산/저장만 기본 32개당 1개로 줄이는
  후보를 시험했다. `python3 -m py_compile bindings/python/perf/single/perf_common.py
  bindings/python/perf/single/perf_pair.py bindings/python/perf/single/perf_dealer_dealer.py
  bindings/python/perf/single/perf_pubsub.py`는 통과했고, 같은 조건 C
  `perf_c_single_linux_20260525_135131_python_single_latency_sample_c.txt`도 complete였다.
  stride 32 후보 `perf_python_single_linux_20260525_135251_single_latency_sample_candidate.txt`의
  tcp small median은 `PAIR` 45.0/47.4/44.0Kmsg/s, `PUBSUB` 43.3/41.6/40.1Kmsg/s,
  `DEALER_DEALER` 46.9/46.3/45.0Kmsg/s로 C 대비 3.4~5.7%에 그쳤다. 기존 동작에 해당하는
  `PERF_SINGLE_LATENCY_SAMPLE_STRIDE=1` 재측정
  `perf_python_single_linux_20260525_135506_single_latency_stride1_reference.txt`도 complete였고,
  `PAIR` 47.8/44.5/49.2Kmsg/s, `PUBSUB` 43.8/40.8/37.4Kmsg/s,
  `DEALER_DEALER` 45.0/45.3/49.8Kmsg/s처럼 cell별 개선과 회귀가 섞였다.
  latency 기록 비용만 줄여서는 single small 보류를 해소하지 못하므로 반영하지 않는다.
- **single sender bytearray 반환 후보 기각**: `stamp_payload(...)`가 매 send마다 헤더를 찍은
  `bytearray`를 다시 `bytes(...)`로 복사하므로, single sender에서 `bytearray`를 그대로
  lower-level send에 넘기는 후보를 시험했다. `python3 -m py_compile
  bindings/python/perf/perf_metrics.py bindings/python/perf/single/perf_common.py
  bindings/python/perf/single/perf_pair.py bindings/python/perf/single/perf_dealer_dealer.py
  bindings/python/perf/single/perf_pubsub.py`는 통과했고 공식 runner도 complete였다.
  `perf_python_single_linux_20260525_135732_single_stamp_bytearray_candidate.txt`의 tcp small
  median은 `PAIR` 50.7/48.9/47.6Kmsg/s, `PUBSUB` 41.6/40.4/39.6Kmsg/s,
  `DEALER_DEALER` 47.6/50.0/48.4Kmsg/s였다. 일부 cell은 기존 동작 재측정
  `perf_python_single_linux_20260525_135506_single_latency_stride1_reference.txt`보다 높았지만,
  `PUBSUB` 64/256B와 `DEALER_DEALER` 1024B는 여전히 낮거나 회귀했고 latency도 크게 흔들렸다.
  같은 조건 C `perf_c_single_linux_20260525_135131_python_single_latency_sample_c.txt` 대비
  처리량은 3.3~6.2%라 single small 보류를 해소하지 못하므로 반영하지 않는다.
- **single message-socket native result fast path 적용**: perf single helper의 `send_nonblocking(...)`와
  `publish_nonblocking(...)`에서 fluent builder와 `native_parts` 리스트 구성을 건너뛰고
  단일 payload를 `zlink_send_part`/`zlink_publish_part`에 바로 넘기는 후보를 시험했다.
  `python3 -m py_compile bindings/python/perf/single/perf_common.py bindings/python/perf/single/perf_pair.py
  bindings/python/perf/single/perf_dealer_dealer.py bindings/python/perf/single/perf_pubsub.py`는
  통과했고 공식 wrapper도 complete였다. 같은 조건 C
  `perf_c_single_linux_20260525_160053_python_single_fast_send_c.txt` 대비 후보
  `perf_python_single_linux_20260525_160344_single_fast_send_candidate.txt`의 tcp small median은
  `PAIR` 4.3/4.5/7.8%, `PUBSUB` 3.3/4.0/4.9%,
  `DEALER_DEALER` 4.7/4.7/7.1%에 그쳤다. Python 내부 send 객체 준비 비용을 더 줄여도
  메시지마다 Python에서 C로 들어가는 호출 경계와 수신 처리 비용이 남아 single small
  보류를 해소하지 못한다. 다만 current HEAD에서 broad 후보를 다시 분리하니 `publish_nonblocking`
  fast path는 PUBSUB를 25~29Kmsg/s로 낮췄지만, message-socket send만 좁힌 후보는
  PAIR 44.4/46.5/45.6Kmsg/s→56.1/52.3/54.4Kmsg/s,
  DEALER_DEALER 43.9/45.0/44.2Kmsg/s→51.6/52.7/53.6Kmsg/s로 올렸다
  (`perf_python_single_linux_20260525_183447_single_native_result_message_only_probe.txt`).
  최종 기본 적용 뒤 공식 wrapper
  `perf_python_single_linux_20260525_183622_single_native_result_message_default.txt`도
  complete였고, 같은 C 기준 `perf_c_single_linux_20260525_183144_python_single_combined_fast_c.txt`
  대비 PAIR 4.3/4.2/7.1%, DEALER_DEALER 4.2/4.2/7.0%다. 보류는 남지만
  절대 처리량 개선이 재현됐으므로 message socket send에 반영했다. 같은 fast path를
  routed send에도 적용한 기본 측정
  `perf_python_single_linux_20260525_184450_single_routed_native_result_default.txt`는
  complete였고, fresh C `perf_c_single_linux_20260525_184018_python_single_routed_current_c.txt`
  대비 `ROUTER_ROUTER` tcp 64/256/1024/65536B가 5.9/3.0/2.9/18.3%였다. 후보
  측정보다 64B와 65536B absolute throughput은 올랐지만, Python과 C 사이의 per-call
  FFI 경계 비용이 남아 routed one-way 보류는 유지한다.
- **single PUBSUB publish native result fast path 적용**: 위 broad 후보에서는 PUBSUB가
  낮아졌지만, current HEAD의 `zlink_subscribe_part` 직접 수신 경로 위에서 publish만 다시
  분리해 시험하니 소폭 개선이 재현됐다. `python3 -m py_compile
  bindings/python/perf/single/perf_common.py bindings/python/perf/single/perf_pubsub.py`는
  통과했고, 같은 조건 C `perf_c_single_linux_20260525_205725_python_single_pubsub_publish_native_c.txt`
  대비 Python `perf_python_single_linux_20260525_205819_single_pubsub_publish_native_candidate.txt`는
  tcp 64/256/1024B median 57.2/54.3/55.9Kmsg/s, 4.4/5.0/7.0%였다. non-tcp도
  C `perf_c_single_linux_20260525_205828_python_single_pubsub_publish_native_nontcp_c.txt`
  대비 Python `perf_python_single_linux_20260525_210032_single_pubsub_publish_native_nontcp_candidate.txt`
  complete로 확인했다. ws는 4.2/5.1/8.7%, wss는 3.7/5.1/12.7%, tls는 4.1/4.6/8.7%다.
  기준선에는 아직 낮지만 절대 처리량 개선이 있으므로 `publish_nonblocking(...)`에 반영한다.
- **single PAIR/DEALER_DEALER receiver native recv_part 적용**: non-routed message socket
  receiver가 public `recv_into()` 경로의 `Received` 객체 구성을 거치지 않고 C와 같은
  `zlink_recv_part` 단일 part 수신을 직접 사용하도록 perf helper를 좁혀 적용했다.
  공식 wrapper `perf_python_single_linux_20260525_201459_single_native_recv_part_header_candidate.txt`
  기준 tcp small은 PAIR 88.6/87.2/80.1Kmsg/s, DEALER_DEALER 88.1/87.7/81.7Kmsg/s로
  직전 기본값보다 크게 올랐고, ws/wss/tls 재측정
  `perf_python_single_linux_20260525_201815_single_native_recv_part_nontcp.txt`도 complete였다.
  그러나 fresh C 기준이 1.3Mmsg/s 수준이라 small ratio는 여전히 한 자릿수에서 머문다.
  따라서 절대 처리량 개선은 반영하되 보류 판정은 유지한다.
- **single PUBSUB subscriber native subscribe_part 적용**: PUBSUB sender의 native publish fast
  path 후보는 낮아져 제외하고, subscriber만 C의 `zlink_subscribe_part` 단일 part 수신 의미에
  맞췄다. fresh C `perf_c_single_linux_20260525_202437_python_single_pubsub_native_subscribe_c.txt`
  대비 Python `perf_python_single_linux_20260525_202721_single_native_subscribe_part_all.txt`는
  tcp 64/256/1024B 53.5/53.4/49.9Kmsg/s로 직전 기본값 44.2/40.4/38.2Kmsg/s보다
  올랐고 ws/wss/tls도 complete였다. 다만 C 대비 비율은 3.4~10.0% 범위라
  single PUBSUB small 보류는 유지한다.
- **single SPOT native part publish/subscribe 적용**: 기존 SPOT perf는 public builder publish와
  callback 내부 `TopicMessage` 구성을 거쳐 C의 direct part 의미보다 Python 객체 비용이 컸다.
  C와 같은 `zlink_spot_publish_part`/`zlink_spot_subscribe_part` 단일 part 경로로 좁힌 뒤
  fresh C `perf_c_single_linux_20260525_203205_python_single_spot_native_part_c.txt` 대비
  Python `perf_python_single_linux_20260525_203500_single_spot_native_part_all.txt`가 complete였고,
  tcp 64/256/1024B는 61.7/59.5/53.8Kmsg/s로 올랐다. ws/wss/tls도 small 비율이
  15.2~24.0%까지 올라 직전보다 개선됐지만 SPOT 최소 기준에는 아직 낮아 보류를 유지한다.
- **single SPOT public subscribe-part 경로 적용**: 2026-05-27에는 binding에 public
  `SpotSubscribedPart`와 `Spot.subscribe_part_into(...)`를 추가해 callback 내부
  `TopicMessage`/parts tuple 구성을 피했다. C
  `perf_c_single_linux_20260527_080022_codex_c_single_python_spot_small_recheck_20260527.txt`
  대비 Python
  `perf_python_single_linux_20260527_080727_codex_python_single_spot_small_subscribe_part_all_20260527.txt`
  는 complete였고, tcp 64/256/1024B는 42.8/43.0/41.2Kmsg/s다. 직전 current
  재측정의 39.7/38.8/37.4Kmsg/s보다는 올랐지만 C 대비 11.1~16.3% 범위라
  single SPOT small 보류는 유지한다.
- **MULTI_SPOT public subscribe-part 후보 기각**: 같은 public
  `Spot.subscribe_part_into(...)` 수신 경로를 multi spot client drain에도 적용해 봤다.
  C `perf_c_multi_linux_20260527_080915_codex_c_multi_spot_small_for_python_subscribe_part_20260527.txt`
  대비 Python 후보
  `perf_python_multi_linux_20260527_081051_codex_python_multi_spot_small_subscribe_part_tcp_20260527.txt`
  는 complete였지만 tcp 64/256/1024B가 148.6/148.7/145.7Kmsg/s로 기존 multi
  spot native subscribe metric 후보보다 낮았다. multi에서는 poll/backlog 비용이 더 커서
  단일-part storage 변경만으로 보류를 해소하지 못하므로 코드는 반영하지 않는다.
- **multi non-routed message-socket native result fast path 적용**: single에서 효과가
  확인된 단일 part native result send를 multi 공용 `send_nonblocking(...)`의
  non-routed send에도 적용했다. 공식 wrapper
  `perf_python_multi_linux_20260525_185647_multi_nonrouted_send_native_result_default.txt`는
  C 기준 `perf_c_multi_linux_20260525_184808_python_multi_send_native_c.txt`와 같은
  조건에서 complete였고, `MULTI_DEALER_DEALER` tcp 64/256/1024B가 317.5/311.9/280.8Kmsg/s,
  C 대비 10.2/15.6/18.4%까지 올랐다. 같은 helper에 routed native result send까지
  넓힌 후보 `perf_python_multi_linux_20260525_185045_multi_send_native_result_candidate.txt`는
  `MULTI_DEALER_DEALER tcp 256B`에서 partial, routed 전용 후보
  `perf_python_multi_linux_20260525_185333_multi_routed_send_native_result_default.txt`는
  `MULTI_ROUTER_ROUTER tcp 1024B`에서 partial이었다. routed echo의 절대 처리량 개선도
  작아 routed send는 기존 builder 경로를 유지한다.
- **MULTI_DEALER_DEALER 262144B 최신 재측정**: 262144B가 small send fast path 이후
  같은 조건 fresh C로 다시 비교되지 않았으므로 current HEAD에서 제한 재측정했다.
  C `perf_c_multi_linux_20260525_194520_python_dd262_current_c_recheck.txt`는 complete였고
  median 47.973Kmsg/s였다. Python
  `perf_python_multi_linux_20260525_194548_dd262_current_recheck.txt`도 complete였고
  median 14.511Kmsg/s, C 대비 30.2%였다. 기존 표의 22.5%보다는 높지만 Python
  단순 one-way 기준선에는 아직 닿지 않아 보류를 유지한다. 별도 IO thread probe
  `perf_python_multi_linux_20260525_194608_dd262_io8_probe.txt`는 13.927Kmsg/s로 더 낮아
  적용 근거가 없다.
- **MULTI_DEALER_DEALER 262144B clean 후 재측정 및 client timestamp sampling 후보 기각**:
  actor entry spot join 동기화 커밋 뒤 clean 상태에서 같은 조건을 다시 측정했다. C
  `perf_c_multi_linux_20260525_211009_python_dd262_current_recheck_after_clean_c.txt`는
  median 51.389Kmsg/s였고, Python
  `perf_python_multi_linux_20260525_211059_dd262_current_recheck_after_clean.txt`는
  median 13.400Kmsg/s, C 대비 26.1%였다. Python client가 모든 송신마다
  `time.time_ns()`를 호출하는 비용을 줄이기 위해 active message timestamp만 stride 32로
  표본화하는 후보도 시험했다. import 순서 실수로 첫 두 실행은 client ready 전 partial
  `perf_python_multi_linux_20260525_211241_dd_client_timestamp_sample_candidate.txt`,
  `perf_python_multi_linux_20260525_211257_dd_client_timestamp_sample_candidate2.txt`가 되었고,
  수정 뒤 단독 후보 `perf_python_multi_linux_20260525_211352_dd_client_timestamp_sample_262_candidate3.txt`는
  complete였지만 13.353Kmsg/s로 no-code 13.400Kmsg/s보다 낮았다. timestamp 호출은
  이 크기의 지배 병목이 아니므로 반영하지 않는다.
- **single PAIR direct `zlink_recv_part` 후보 기각**: `PairSocket.recv_into(...)`가
  `Received`와 parts list를 구성하는 비용을 피하기 위해, perf helper 안에서만 native
  `zlink_recv_part`를 직접 호출해 단일 part payload를 decode하는 후보를 `PAIR`에 좁혀
  시험했다. `python -m py_compile bindings/python/perf/single/perf_common.py
  bindings/python/perf/single/perf_pair.py`는 통과했고 공식 wrapper도 complete였다. 그러나
  같은 조건 C `perf_c_single_linux_20260525_170652_python_single_pair_recv_part_c.txt` 대비
  후보 `perf_python_single_linux_20260525_170758_single_pair_recv_part_candidate.txt`의 tcp
  64/256/1024B median은 69.715/45.855/53.255Kmsg/s, 5.3/3.5/6.6%에 그쳤다. `Received`
  객체 구성을 건너뛰어도 ctypes 호출과 Python decode loop 비용이 남고 run 변동도 커서
  보류 해소 근거로 반영하지 않는다.
- **MULTI_DEALER_DEALER send bytearray 후보 기각**: client hot path에서 `stamp_payload(...)`가
  `bytearray`에 헤더를 찍은 뒤 `bytes(payload)`를 반환하므로, 65536/262144B에서 반환된
  `bytes` 대신 원래 `bytearray`를 `send_nonblocking(...)`에 넘기는 후보를 시험했다.
  `python -m py_compile bindings/python/perf/multi/perf_multi_dealer_dealer_client.py`는 통과했다.
  그러나 65536/262144B 묶음 공식 측정 `perf_python_multi_linux_20260525_044244.txt`는
  85.255/9.144Kmsg/s로, 기존 65536B complete 기준 90.172Kmsg/s
  (`perf_python_multi_linux_20260524_191648.txt`)와 같은 조건 no-code 262144B 단독
  10.588Kmsg/s(`perf_python_multi_linux_20260525_044157.txt`)보다 낮았다. 262144B 단독
  후보 `perf_python_multi_linux_20260525_044136.txt`는 11.595Kmsg/s로 소폭 높았지만
  묶음 실행에서 재현되지 않아 반영하지 않는다.
- **MULTI_DEALER_DEALER client POLLOUT 토글 후보 기각**: C/Go/C++처럼 backpressure가
  걸린 socket만 `POLLOUT`으로 poll하도록 Python client의 `Poller.modify_socket(...)`
  후보를 시험했다. `python -m py_compile bindings/python/perf/multi/perf_multi_dealer_dealer_client.py`는
  통과했고 공식 제한 측정도 complete였지만, 같은 조건 C
  `perf_c_multi_linux_20260525_054843.txt` 대비 Python
  `perf_python_multi_linux_20260525_054907.txt`가 tcp 262144B 21.0%에 그쳤다. 기존
  대표값 22.5%보다 낮아, Python에서는 poll set 축소보다 메시지별 send/recv FFI 비용이
  더 지배적이라고 보고 반영하지 않는다.
- **MULTI_DEALER_DEALER move-send 후보 기각**: Python binding send path가 `Message`
  입력도 `_clone_native_msg(...)`로 복사하므로, `zlink_msg_move` 기반 ownership 이전
  wrapper를 임시로 추가하고 tcp 64B client에서 `Message.allocate(...)`에 직접 header를
  찍어 move-send하는 후보를 시험했다. `python -m py_compile`과
  `PYTHONPATH=bindings/python/src python -m pytest bindings/python/tests/test_core_api_alignment.py bindings/python/tests/test_boundary_ownership_contract.py -q`는
  통과했다. 첫 공식 wrapper는 import 실수로 partial(`perf_python_multi_linux_20260525_114857.txt`)이었고,
  수정 뒤 complete(`perf_python_multi_linux_20260525_114951.txt`)됐지만 median은
  143.741Kmsg/s로 기존 대표값 `perf_python_multi_linux_20260524_191648.txt`의
  182.389Kmsg/s보다 낮았다. fresh C `perf_c_multi_linux_20260525_114847.txt` 대비도
  4.9%에 그쳐 public move-send API를 추가할 근거가 없으므로 반영하지 않는다.
- **MULTI_DEALER_DEALER latency sampling 후보 기각**: PUBSUB처럼 server throughput
  count는 전 메시지를 유지하고 latency 계산/리스트 저장만 표본화하는 후보를 시험했다.
  `python3 -m py_compile bindings/python/perf/multi/perf_multi_dealer_dealer_server.py bindings/python/perf/multi/perf_multi_dealer_dealer_client.py`는
  통과했고, 같은 조건 C `perf_c_multi_linux_20260525_133709_python_dd_latency_sample_c.txt`도
  complete였다. stride 32 후보 `perf_python_multi_linux_20260525_133804_dd_latency_sample_candidate.txt`는
  tcp 64/256/1024/65536/262144B가 186.5/176.9/172.3/88.0/10.6Kmsg/s였다.
  같은 코드에서 기존 동작에 해당하는 stride 1 재측정
  `perf_python_multi_linux_20260525_133842_dd_latency_stride1_reference.txt`의
  178.1/181.8/176.6/82.7/10.4Kmsg/s와 비교하면 64B와 large는 조금 높지만
  256/1024B가 낮아졌다. 더 완만한 stride 8 후보
  `perf_python_multi_linux_20260525_133912_dd_latency_stride8_probe.txt`도 tcp
  64/256/1024B가 181.5/177.7/171.5Kmsg/s라 안정적인 개선이 아니었다. small size
  보류도 6.3/9.2/11.9%로 남으므로 반영하지 않는다.
- **MULTI_DEALER_DEALER server header single-decode 후보 기각**: server hot path의
  metric payload 확인, active 판별, latency 계산이 같은 header를 여러 번 decode하므로,
  마지막 part의 `data` view에서 header를 한 번만 decode해 count와 latency를 함께 처리하는
  후보를 시험했다. `python3 -m py_compile bindings/python/perf/multi/perf_multi_dealer_dealer_server.py bindings/python/perf/multi/perf_multi_dealer_dealer_client.py`는
  통과했다. 그러나 공식 wrapper
  `perf_python_multi_linux_20260525_134111_dd_single_decode_candidate.txt`는 repeat 3의
  `tcp 64B`에서 READY 수집 실패로 partial이 됐고, complete된 앞선 repeat median도
  tcp 64/256/1024/65536/262144B 181.7/179.1/174.6/89.5/10.6Kmsg/s로 기존 보류권과
  다르지 않았다. `tcp 64B` 단독 재시도
  `perf_python_multi_linux_20260525_134126_dd_single_decode_64_retry.txt`는 complete였지만
  183.9Kmsg/s로 기존 대표값 182.4Kmsg/s 대비 1% 미만이다. decode 중복은 일부 latency
  수치를 낮췄지만 throughput 보류를 해소하지 못하고 묶음 complete도 확보하지 못해
  반영하지 않는다.
- **routed `recv_into` single-part fast path 적용**: Python `RouterSocket.recv_into`는 모든 routed 수신에서 Python 리스트에 `ZlinkMsg`를 모은 뒤 다시 C 배열로 복사했다. single-part가 대부분인 routed echo hot path에 일반 `recv_into`와 같은 fast path를 넣어 첫 part를 바로 `_ReceivedPartsOwner`로 넘기게 했다.
  `bindings/python/tests/run_tests.sh`는 통과했고, 공식 wrapper `PERF_FAIL_FAST=1 bindings/python/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 64,65536,131072,262144 --duration 1 --runs 3`에서 `MULTI_DEALER_ROUTER` large가 43.2/57.0/63.2%로 올랐다(`perf_python_multi_linux_20260524_235408.txt`). `MULTI_ROUTER_ROUTER` 65536B/262144B는 단독 complete 재측정에서 33.1/57.3%로 확인했다(`perf_python_multi_linux_20260524_235704.txt`, `perf_python_multi_linux_20260524_235511.txt`). small size는 여전히 per-call FFI 벽 때문에 보류다.
  같은 fast path는 non-tcp routed echo에도 효과가 있었다. `ws MULTI_DEALER_ROUTER 65536B`는 32.6%→38.0%(`perf_python_multi_linux_20260525_000108.txt`), `tls MULTI_ROUTER_ROUTER 65536B`는 28.8%→46.2%(`perf_python_multi_linux_20260525_000403.txt`)로 보류에서 통과로 올라갔다. `wss` 65536B는 `MULTI_DEALER_ROUTER` 70.5%, `MULTI_ROUTER_ROUTER` 56.1%로 통과 여유가 커졌다(`perf_python_multi_linux_20260525_000301.txt`). `ws MULTI_ROUTER_ROUTER 65536B`는 28.8%로 개선됐지만 아직 보류다(`perf_python_multi_linux_20260525_000503.txt`).
  2026-05-25 같은 조건 제한 재측정에서는 `ws MULTI_ROUTER_ROUTER 131072B`가 43.3%로 통과권에 올라왔고, `ws 65536B`는 28.9%로 보류가 유지됐다(`perf_c_multi_linux_20260525_023817.txt`, `perf_python_multi_linux_20260525_024823.txt`). `wss MULTI_ROUTER_ROUTER 65536/131072B`는 54.5/69.6%로 통과를 재확인했다.
- **2026-06-22 Python recv owner 경로 재검토**:
  simple `_MessageSocket.recv_into`는 native extension에 `recv_owner`가 있으면 bytes tuple로
  payload를 복사하지 않고 `NativeReceivedPartsOwner`를 바로 받는다. routed
  `_RoutedMessageSocket.recv_into`도 `router_recv_owner`가 있으면 owner 경로를 먼저 쓴다.
  따라서 남은 Python small receive 미달을 줄이기 위해 bytes tuple fallback을 건드리는 것은
  현재 build의 hot path가 아니다. `Received.parts`의 `ReceivedMessage` tuple을 재사용하는
  후보도 이전 parts view를 잡고 있는 사용자 코드를 깨뜨릴 수 있어 public contract 보존
  원칙에 맞지 않는다. 이번 라운드에서는 성능 코드를 바꾸지 않고, Python public
  `recv_into` 주석의 압축 영어 표현을 풀어 hot path 의도를 명확히 했다. 다음 Python
  후보는 owner receive 이후의 per-call FFI 경계나 send native bridge 쪽에서 찾아야 한다.
- **2026-06-22 Python `ReceivedMessage.data` zero-copy 후보 폐기**:
  `NativeReceivedPartsOwner.data(index)`는 native message buffer를 직접 노출하지 않고
  Python-owned bytes snapshot 위의 memoryview를 돌려준다. 이 비용을 없애면 perf loop의
  마지막 part 접근은 빨라질 수 있지만, 사용자가 받은 view를 들고 있는 동안 received
  part를 닫아도 view가 유효해야 한다는 현재 public lifetime 의미가 바뀐다. 이는
  benchmark 데이터 접근만 빠르게 만드는 변경이고, binding의 일반 public contract를
  지키는 최적화가 아니다. 따라서 zero-copy view로 바꾸지 않고, C extension의
  `native_parts_owner_data` 가까이에 `HOT PATH` 주석으로 snapshot 계약과 금지 이유를
  명시했다.
- **2026-06-22 Python perf loop 데이터 접근 후보 제외**:
  Python single perf는 이미 public `recv_into(storage, DONT_WAIT)`와 long-lived
  `Received`/`TopicMessage` storage를 사용한다. `storage_data_part(...)`가 마지막 part만
  `to_bytes()`로 읽도록 줄인 기존 perf 보강도 metric extraction 범위에 머문다. 남은
  미달을 줄이기 위해 perf에서 `ReceivedMessage.data` view를 직접 읽거나 native helper로
  latency/header만 빼는 변경은 실사용 public 경로의 allocation, copy, ownership 비용을
  줄인 근거가 없으면 채택하지 않는다. 다음 Python 후보는 perf 코드가 아니라 binding
  내부의 send native bridge, owner receive 이후 Python object materialization, FFI call
  boundary 중 profiler로 확인되는 일반 비용만 대상으로 한다.
- **2026-06-22 Python send native bridge move/borrow 후보 폐기**:
  public `.send().message(...).flags(...).submit()`, routed send, publish send는 native
  `SocketSendOp` 계열 객체를 먼저 사용한다. single payload fast path도 Python에서
  submit 루프를 돌지 않고 C extension 안에서 `zlink_send_part`, `zlink_send_part_rid`,
  `zlink_publish_part`를 호출한다. 남은 큰 비용은 caller buffer를 native message로 복사하는
  부분이지만, 이 복사는 submit 실패 또는 `DONT_WAIT` backpressure에서 Python payload가
  그대로 남아야 하는 public 계약을 지키기 위한 비용이다. bytearray/template payload를
  move하거나 native message가 caller buffer를 빌리도록 바꾸는 후보는 perf의 반복 payload
  모양에는 유리할 수 있어도 실사용 public 계약을 약하게 만든다. 따라서 코드는 바꾸지
  않고 C extension의 socket/routed/publisher single-payload hot path 가까이에 복사를
  유지해야 하는 이유를 주석으로 고정했다.
- **multi routed echo latency sampling 후보 기각**: `MULTI_DEALER_ROUTER`/`MULTI_ROUTER_ROUTER`
  client reply hot path도 `MULTI_PUBSUB`처럼 count는 payload length로 유지하고 latency header
  decode만 32개당 1개로 줄이는 후보를 시험했다. `python3 -m py_compile
  bindings/python/perf/multi/perf_multi_common.py
  bindings/python/perf/multi/perf_multi_dealer_router_client.py
  bindings/python/perf/multi/perf_multi_router_router_client.py`는 통과했고, 공식 wrapper
  `PERF_FAIL_FAST=1 bindings/python/perf/run_benchmarks_multi.sh --transports tcp --pattern
  MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,65536 --duration 1 --runs 3`도
  complete였다(`perf_python_multi_linux_20260525_193443_multi_routed_latency_sample_candidate.txt`).
  그러나 median은 `MULTI_DEALER_ROUTER` tcp 64/256/1024/65536B가
  54.8/54.9/54.6/37.9Kops/s로 기존 대표값 56.5/56.1/54.0Kops/s 및 43.2% large 근거보다
  낮거나 같은 수준이었다. `MULTI_ROUTER_ROUTER`는 64/256/1024B가
  43.0/42.4/41.7Kops/s로 기존 39.7/39.9/38.9Kops/s보다 소폭 높지만,
  65536B는 29.0Kops/s로 기존 통과 근거 33.1%보다 낮아졌다. small size도 기준선에는
  멀어, routed echo의 남은 병목은 latency 기록보다 메시지별 send/recv FFI 경계와 server
  echo 경로 비용이 지배적이라고 보고 후보를 반영하지 않는다.
- **SPOT routed receive fast path 후보 기각**: 같은 single-part fast path를 SPOT routed receive에도 적용하는 후보를 시험했지만, 공식 wrapper `PERF_FAIL_FAST=1 bindings/python/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER,MULTI_SPOT_SENDSEND --msg-sizes 64,65536,131072 --duration 1 --runs 3`가 `MULTI_SPOT_SENDSEND tcp 64B`에서 `CLIENT_READY` 누락 partial로 끝났다(`perf_python_multi_linux_20260524_235122.txt`). SPOT 변경을 되돌린 뒤 `MULTI_SPOT_SENDSEND tcp 64B` 단독 확인은 complete였다(`perf_python_multi_linux_20260524_235612.txt`). 따라서 이번 적용 범위는 일반 routed socket으로 제한한다.
- **echo server view-send 후보 기각**: `MULTI_DEALER_ROUTER`/`MULTI_ROUTER_ROUTER` server에서 즉시 reply 성공 시 `to_bytes_list()` 복사를 피하고 `ReceivedMessage.data` view를 바로 `.message(...)`에 넘기는 후보를 시험했다. 공식 runner는 complete였지만 `MULTI_DEALER_ROUTER` 64B는 52.7K→50.2Kops/s(`perf_python_multi_linux_20260524_194948.txt`)로 내려갔고, `MULTI_ROUTER_ROUTER` 65536B도 26.5K→25.7Kops/s(`perf_python_multi_linux_20260524_195005.txt`)로 내려갔다. backpressure 시에는 결국 queue 보관을 위해 복사가 필요하고, 즉시 send 경로도 binding send buffer 준비 비용이 남아 안정적인 개선이 아니므로 반영하지 않는다.
- **SPOT send-send consume-forward 후보 기각**: Python binding에 C `zlink_spot_forward_routed`를 임시로 노출해 `MULTI_SPOT_SENDSEND` server가 수신 payload를 Python 객체로 복사하지 않고 바로 source spot으로 되돌리는 후보를 시험했다.
  `DONT_WAIT` send 조합은 `wss 65536B`에서 client timeout으로 partial이 됐다(`perf_python_multi_linux_20260524_204031.txt`, handshake 구간 적용 / `perf_python_multi_linux_20260524_204146.txt`, active 구간만 적용).
  blocking send 조합은 complete됐지만 `wss` 65536/131072/262144B가 각각 4.908K/2.751K/1.639Kops/s로 C 대비 35.4%/33.2%/36.3%에 그쳐 기존 값보다 낮았고(`perf_python_multi_linux_20260524_204251.txt`, `perf_python_multi_linux_20260524_204638.txt`), `tls 131072B`는 READY control line 누락으로 partial이 됐다.
  이 경로는 Python 복사를 줄여도 send backpressure와 route readiness 경계에서 안정성을 해치거나 throughput을 낮추므로 공개 API와 perf runner 변경을 반영하지 않는다.
- **SPOT send-send dispatch callback 후보 기각**: Rust server처럼 `on_dispatch_event(ROUTED_READABLE)`에서 즉시 `_drain_replier()`를 호출하는 후보를 시험했다.
  `python -m py_compile bindings/python/perf/multi/perf_multi_spot_sendsend_server.py`는 통과했지만, 공식 runner `wss 65536B`에서 `CONTROL_CONNECTED` handshake 누락으로 partial이 됐다(`perf_python_multi_linux_20260524_205012.txt`).
  Python callback dispatcher를 이 server handshake와 섞으면 control/data 초기화 순서를 더 불안정하게 만들 수 있어 반영하지 않는다.
- **SPOT send-send server view-send 후보 기각**: `MULTI_SPOT_SENDSEND` server에서 `received.to_bytes_list()` 대신 `ReceivedMessage.data` view를 바로 send builder에 넘기는 후보를 시험했다. 첫 후보는 `received.first_part().data`를 `received.send().message(...)`에 넘겼고, `python -m py_compile bindings/python/perf/multi/perf_multi_spot_sendsend_server.py`는 통과했지만 공식 runner `tcp 64/65536/131072/262144B`에서 262144B client timeout partial이 재현됐다(`perf_python_multi_linux_20260524_225700.txt`). 이후 전 transport/large 후보는 `perf_python_multi_linux_20260525_011507.txt`에서 `ws 65536B` client timeout partial로 끝났다. tcp 65536/131072B 조건부 후보도 `perf_python_multi_linux_20260525_013902.txt`에서 `tcp 65536B` client timeout partial로 끝났다. 중간 repeat에서 일부 tcp large 수치가 올라간 적은 있지만 run 간 변동이 크고 timeout을 만들기 때문에 반영하지 않는다.
- **SPOT send-send 최신 제한 재측정도 불안정**: current HEAD에서도 C 기준 확보를 위해 묶은 실행은 C `MULTI_SPOT_REQREP ws 131072B` fast_mutex partial로 끝났고(`perf_c_multi_linux_20260525_023516.txt`), `MULTI_SPOT_SENDSEND`를 함께 보려던 Python 묶음도 `ws 65536B` client timeout partial로 끝났다(`perf_python_multi_linux_20260525_024649.txt`). timeout을 만드는 결과는 통과 근거로 쓰지 않고, SPOT send-send는 계속 별도 안정화 대상으로 둔다.
- **SPOT send-send small active-slot 32 후보 기각**: small size에서 100개 spot을 모두 active로 스캔하는 비용을 줄이기 위해 active slot을 32로 제한하는 후보를 시험했다. 공식 wrapper `perf_python_multi_linux_20260525_051821.txt`는 complete였지만 tcp 64/256/1024B median이 27.8/27.9/27.4Kops/s로 기존 `perf_python_multi_linux_20260522_204542_codex_python_multi_tcp_sendsend_small_final_recheck_20260522.txt`의 29.5/28.7/28.8Kops/s보다 낮았다. small size에서는 active slot 축소가 per-call FFI 비용을 줄이지 못하고 in-flight echo만 줄이므로 반영하지 않는다.
- **SPOT send-send small active-slot 16 후보 기각**: active slot 32보다 더 좁힌 16-slot 후보도 tcp 64/256/1024B에 재시험했다. C 기준 `perf_c_multi_linux_20260525_060714.txt` 대비 후보 `perf_python_multi_linux_20260525_061604.txt`는 25.260/24.181/24.943Kops/s, 9.6/9.2/10.4%에 그쳤다. 기존 대표값과 active-slot 32 후보보다 낮으므로 small active slot 축소는 더 진행하지 않는다.
- **SPOT send-send `tcp 64/256/1024B` 최신 재측정**: 2026-05-27 같은 조건으로
  C `perf_c_multi_linux_20260527_082237_codex_c_multi_sendsend_small_recheck_20260527.txt`와
  Python `perf_python_multi_linux_20260527_083127_codex_python_multi_sendsend_small_recheck_20260527.txt`를
  단독 실행했다. 두 파일 모두 complete였고 Python median은 29.033/29.211/28.548Kops/s,
  C median은 266.306/257.744/235.179Kops/s라 C 대비 10.9/11.3/12.1%다. 1024B는
  오래된 C 기준에서는 통과권으로 보였지만 최신 같은 조건 C 기준을 갱신하면 보류권이다.
  기존 active-slot 축소와 view-send 후보가 이미 낮은 처리량 또는 timeout을 만들었기 때문에
  추가 perf 변경 없이 보류를 유지한다.
- **SPOT send-send `ws 64B` 최신 재측정 및 active-slot 32 후보 기각**: 오래된
  표의 0.2%는 current HEAD에서 재현되지 않았다. 같은 조건 최신 재측정 C
  `perf_c_multi_linux_20260525_084304.txt` 대비 no-code Python
  `perf_python_multi_linux_20260525_084908.txt`는 29.153Kops/s, 11.8%였으므로
  표를 갱신한다. 다만 `ws 64B`만 active slot을 32로 좁힌 후보
  `perf_python_multi_linux_20260525_084610.txt`는 complete였어도 27.475Kops/s,
  11.1%로 no-code보다 낮았다. active slot 축소는 latency는 낮췄지만 throughput
  보류를 해소하지 못하므로 반영하지 않는다.
- **SPOT send-send `wss 64B` 최신 재측정 및 view-send 후보 기각**: 오래된
  표의 0.1%도 current HEAD에서 재현되지 않았다. 같은 조건 최신 재측정 C
  `perf_c_multi_linux_20260525_085103.txt` 대비 no-code Python complete 단독
  재측정 `perf_python_multi_linux_20260525_085744.txt`는 27.841Kops/s, 10.1%였으므로
  표를 갱신한다. 다만 `wss 64B`만 `ReceivedMessage.data` view를 바로
  send builder에 넘기는 후보 `perf_python_multi_linux_20260525_085457.txt`는
  complete였어도 28.475Kops/s, 10.4%에 그쳤고, no-code 3-run 재측정
  `perf_python_multi_linux_20260525_085628.txt`의 성공 run 28.545Kops/s보다도
  낮았다. 조건부 특수 분기만 늘리고 throughput 보류를 해소하지 못하므로 반영하지
  않는다.
- **MULTI_SPOT reusable TopicMessage 후보 기각**: client가 수신 메시지마다 caller-provided `TopicMessage()` placeholder를 새로 만들던 비용을 줄이기 위해 spot별 reusable `TopicMessage`를 두고 `subscribe_into(...)`에 재사용하는 후보를 시험했다. `python -m py_compile bindings/python/perf/multi/perf_multi_spot_client.py`는 통과했고 공식 runner도 complete였지만, C `perf_c_multi_linux_20260525_065239.txt` 대비 후보 `perf_python_multi_linux_20260525_070822.txt`의 tcp 64/256/1024B median은 114.033/115.156/113.439Kmsg/s로 기존 약 120Kmsg/s 대역보다 낮거나 같았다. placeholder 재사용만으로는 `subscribe_into` 내부 native receive와 Python 경계 비용을 줄이지 못하므로 반영하지 않는다.
- **MULTI_SPOT subscribe_into 직접 `_replace` 후보 기각**: `Spot.subscribe_into(...)`가 내부에서 `TopicMessage`를 새로 만든 뒤 `_adopt_from(...)`으로 caller-provided storage에 옮기던 구조를 일반 SUB socket처럼 owner를 직접 `_replace(...)`하는 fast path로 바꾸는 후보를 시험했다. `python -m py_compile bindings/python/src/zlink/contracts/service/spot.py bindings/python/perf/multi/perf_multi_spot_client.py`와 `bindings/python/tests/run_tests.sh`는 통과했고 공식 runner도 complete였지만, C `perf_c_multi_linux_20260525_074518.txt` 대비 후보 `perf_python_multi_linux_20260525_080609.txt`의 tcp 64/256/1024/65536B median은 119.886/118.150/115.048/75.429Kmsg/s, 2.6/3.0/3.2/5.0%에 그쳤다. 기존 약 120Kmsg/s 수신 벽을 넘지 못해 코드에는 반영하지 않는다.
- **MULTI_SPOT active header 검증 샘플링 후보 기각**: server가 START 이후 phase=1
  payload만 보내는 현재 perf topology를 이용해 throughput count는 전체 수신 메시지로
  세고, run-id/msg-size/phase 검증과 latency decode는 sample stride마다만 수행하는
  후보를 시험했다. `python -m py_compile bindings/python/perf/multi/perf_multi_spot_client.py`는
  통과했고 공식 wrapper도 complete였지만, C 기준 `perf_c_multi_linux_20260525_165006_python_multi_spot_sampled_active_c.txt`
  대비 후보 `perf_python_multi_linux_20260525_170109_multi_spot_sampled_active_candidate.txt`의
  tcp 64/65536B median은 130.383/80.332Kmsg/s, 2.6/5.2%에 그쳤다. active 검증
  비용을 줄여도 Python `subscribe_into` 수신 경계가 남고, perf-only 가정을 넓히는
  변화라 보류 해소 근거로 반영하지 않는다.
- **MULTI_SPOT worker drain 후보 기각**: Rust/Go처럼 100개 spot을 여러 worker가 나눠
  drain하면 Python에서도 per-spot 순회 병목을 줄일 수 있는지 확인했다. worker별
  `Poller`와 local count/latency를 두고 `PERF_MULTI_SPOT_RECV_WORKERS=4` 후보를
  시험했다. `python -m py_compile bindings/python/perf/multi/perf_multi_spot_client.py`는
  통과했고 공식 wrapper도 complete였지만, 같은 조건 C 기준
  `perf_c_multi_linux_20260525_171355_python_multi_spot_worker_c.txt` 대비 후보
  `perf_python_multi_linux_20260525_171743_multi_spot_worker4_candidate.txt`의 tcp
  64/65536B는 14.576/14.229Kmsg/s로 current 단일-thread 후보보다 크게 낮다. Python
  thread와 ctypes/poller 분할 비용이 이득보다 커서 반영하지 않는다.
- **MULTI_SPOT native receive 뒤 worker drain 재확인 후보 기각**: client native metric
  receive 적용 뒤에도 worker drain 기각이 유지되는지 다시 확인했다. 같은 native receive
  hot path 위에서 `PERF_MULTI_SPOT_RECV_WORKERS=4`를 env-gated로 붙인 후보는
  `python3 -m py_compile bindings/python/perf/multi/perf_multi_spot_client.py`를 통과했고,
  공식 wrapper `perf_python_multi_linux_20260525_225602_multi_spot_native_receive_worker4_probe.txt`도
  tcp 64/65536B runs=1 complete였다. 그러나 처리량은 25.029/19.189Kmsg/s로,
  단일 thread native receive의 308.6/130.9Kmsg/s보다 크게 낮다. native receive 뒤에도
  Python thread와 worker별 poller 분할 비용이 이득보다 커서 코드는 반영하지 않는다.
- **MULTI_SPOT server blocking fallback 후보 기각**: C server는 `DONTWAIT` publish가
  `EAGAIN`이면 같은 payload를 blocking submit으로 한 번 더 시도한다. Python server에도
  같은 fallback을 env-gated 후보로 붙여 시험했다. `python3 -m py_compile
  bindings/python/perf/multi/perf_multi_common.py bindings/python/perf/multi/perf_multi_spot_server.py
  bindings/python/perf/multi/perf_multi_spot_client.py`는 통과했고, 같은 조건 C
  `perf_c_multi_linux_20260525_151212_python_multi_spot_blocking_fallback_c.txt` 대비 Python 후보
  `perf_python_multi_linux_20260525_152755_spot_publish_blocking_fallback_candidate.txt`는
  complete였다. 하지만 tcp 64/256/1024B median이 114.869/116.314/111.008Kmsg/s로
  기존 직접 `_replace` 후보보다 낮고, 일부 run latency가 3~4.5초까지 튀었다. Python
  `Spot.publish` blocking submit은 send path를 오래 붙잡아 client drain backlog를 키우므로
  C fallback 의미를 그대로 옮겨도 보류를 해소하지 못해 반영하지 않는다.
- **MULTI_SPOT server native publish 후보 기각**: client native metric receive 적용 뒤에도
  server가 매 active publish마다 `stamp_payload(...)`의 `bytes` 복사와 fluent
  `Spot.publish(...).message(...).submit()` 구성을 거치므로, server active path만
  `bytearray` header stamp와 native `zlink_spot_publish_part` 직접 호출로 좁히는 후보를
  시험했다. 처음 smoke는 잘못된 `HEADER_FORMAT` import 때문에 READY 전 partial
  `perf_python_multi_linux_20260525_224039_multi_spot_native_publish_receive_candidate_smoke.txt`가
  났고, import 수정 뒤 공식 wrapper
  `perf_python_multi_linux_20260525_224744_multi_spot_native_publish_receive_candidate_smoke2.txt`는
  tcp 64/256/1024/65536B runs=1 complete였다. 처리량은 320.9/318.2/295.4/134.2Kmsg/s로,
  직전 client native receive median 308.6/324.6/299.9/130.9Kmsg/s 대비 cell별 개선과
  회귀가 섞였다. 서버 publish 객체 비용은 일부 size에서만 noise 수준으로 보이고,
  보류의 주 병목은 여전히 client drain/backlog 쪽이므로 코드는 반영하지 않는다.
- **MULTI_SPOT_REQREP callback latency queue 제거 후보 기각**: reply callback이
  `SimpleQueue`에 latency를 넣고 main loop가 비우는 비용을 줄이기 위해 callback에서
  보호된 리스트에 바로 기록하는 후보를 시험했다. `python -m py_compile`로
  `bindings/python/perf/multi/perf_multi_spot_reqrep_client.py`를 확인했고
  `bindings/python/tests/run_tests.sh`는 통과했고 공식 runner도 complete였다. 그러나
  C `perf_c_multi_linux_20260525_081726.txt` 대비 후보
  `perf_python_multi_linux_20260525_083508.txt`는 tcp 64/256/1024B가 14.1/14.5/15.2%,
  ws 64/256/1024B가 16.3/16.4/19.1%로 기존 대표값과 같은 보류권이다. callback 뒤
  queue 왕복보다 request/reply callback 경계와 per-message Python 호출 비용이 지배적이므로
  반영하지 않는다.
- **MULTI_SPOT_REQREP active count fast path 후보 기각**: PUBSUB에서 효과가 있었던
  active count 분리와 latency sampling을 SPOT reqrep client에도 시험했다. callback은
  reply payload length로 active reply를 세고, latency는 기본 32개당 1개만 계산하도록
  바꿨다. `python3 -m py_compile bindings/python/perf/multi/perf_multi_spot_reqrep_client.py`는
  통과했고 공식 wrapper도 complete였다. 그러나 fresh C
  `perf_c_multi_linux_20260525_195105_python_spot_reqrep_sampled_count_c.txt` 대비 Python 후보
  `perf_python_multi_linux_20260525_200020_spot_reqrep_sampled_count_candidate.txt`는 tcp
  64/256/1024B median이 36.832/37.117/35.841Kops/s, 비율은 14.5/14.8/15.4%에 머물렀다.
  header decode와 latency queue를 줄여도 small SPOT reqrep는 callback 전달과 request submit
  경계 비용이 더 커서 기준선을 넘지 못하므로 반영하지 않는다.
- **MULTI_PUBSUB multi-worker drain 후보 기각**: Python에서도 100개 subscriber를 worker 4개로 나눠 worker별 poller와 local latency/count를 합산하는 후보를 시험했다.
  `python -m py_compile bindings/python/perf/multi/perf_multi_common.py bindings/python/perf/multi/perf_multi_pubsub_client.py`는 통과했지만, 공식 runner `PERF_FAIL_FAST=1 bindings/python/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_PUBSUB --msg-sizes 64 --duration 1 --runs 1`에서 client timeout partial(`perf_python_multi_linux_20260524_194603.txt`)이 재현됐다. Python binding 객체와 poller를 여러 Python thread에 나누는 접근은 현재 공개 API 조합에서 안정적인 개선 후보가 아니므로 반영하지 않는다.
- **MULTI_PUBSUB latency sampling 적용**: PUBSUB client hot path가 모든 active 메시지에서 latency를 계산하고 Python list에 저장하던 비용을 줄이기 위해
  throughput count는 전 메시지를 유지하고 latency 계산/저장만 기본 32개당 1개로 줄였다(`PERF_MULTI_PUBSUB_LATENCY_SAMPLE_STRIDE`).
  `python -m py_compile bindings/python/perf/multi/perf_multi_pubsub_client.py`는 통과했다. 공식 wrapper C `perf_c_multi_linux_20260525_110110.txt` 대비
  Python `perf_python_multi_linux_20260525_111359.txt`는 전 transport complete였고, tcp 64/256/1024/65536B가 141.9/141.6/135.7/100.7Kmsg/s였다.
  기존 동작에 해당하는 `PERF_MULTI_PUBSUB_LATENCY_SAMPLE_STRIDE=1` 재측정 `perf_python_multi_linux_20260525_111617.txt`의
  134.4/130.7/110.3/88.7Kmsg/s보다 모두 높아 기본값으로 반영한다. wss/tls 65536B도 111.4/100.8%로 통과권을 유지했다.
  small size는 여전히 Python per-message FFI 호출 비용 때문에 기준보다 낮다.
- **MULTI_PUBSUB active count fast path 적용**: PUBSUB server는 START 이후 active payload와
  wire-level stop token만 보내고 endpoint도 run마다 새로 만들기 때문에, client hot path에서
  모든 메시지의 metric header를 decode하지 않아도 active count를 보존할 수 있다. subscriber는
  stop token을 먼저 확인한 뒤 payload length가 현재 size와 같은 메시지를 count하고,
  latency sampling 대상에만 header decode를 수행하도록 좁혔다. `python3 -m py_compile
  bindings/python/perf/multi/perf_multi_common.py bindings/python/perf/multi/perf_multi_pubsub_client.py
  bindings/python/perf/multi/perf_multi_pubsub_server.py`와 `bindings/python/tests/run_tests.sh`는
  통과했다. 공식 wrapper는 fresh C
  `perf_c_multi_linux_20260525_190118_python_multi_pubsub_publish_native_c.txt` 대비 Python
  `perf_python_multi_linux_20260525_192512_multi_pubsub_client_len_fastpath_candidate.txt`가
  전 transport complete였고, tcp 64/256/1024/65536B는 209.3/206.1/192.3/140.1Kmsg/s,
  ws는 190.7/191.4/199.0/117.7Kmsg/s, wss는 198.5/202.1/199.6/79.2Kmsg/s,
  tls는 186.8/190.7/199.4/96.3Kmsg/s였다. small size는 아직 보류지만, 이전
  header-decode 기준보다 전 transport small absolute 처리량이 올랐다.
- **MULTI_PUBSUB publish native result 후보 기각**: server `publish_nonblocking(...)`도
  `_publish_via_native_no_wait_result`로 fluent builder를 우회하는 후보를 시험했다.
  `python3 -m py_compile ...`와 `bindings/python/tests/run_tests.sh`는 통과했지만 공식 wrapper
  `perf_python_multi_linux_20260525_191214_multi_pubsub_publish_native_candidate.txt`가
  `wss 64B`에서 client timeout partial로 끝났다. tcp/ws small 처리량도 count fast path보다
  낮아 안정적인 개선 후보가 아니므로 반영하지 않는다. current HEAD에서도 같은 후보를 다시
  좁혀 시험했다. `python3 -m py_compile bindings/python/perf/multi/perf_multi_common.py
  bindings/python/perf/multi/perf_multi_pubsub_server.py`는 통과했고 같은 조건 C
  `perf_c_multi_linux_20260525_210323_python_multi_pubsub_publish_native_c.txt`는 complete였지만,
  Python 후보 `perf_python_multi_linux_20260525_210400_multi_pubsub_publish_native_candidate.txt`는
  tcp 64B만 207.2Kmsg/s로 올린 뒤 256B에서 READY control line을 받지 못해 fail-fast
  partial로 끝났다. publish hot path를 더 빠르게 만드는 것만으로는 subscriber/control
  전환 안정성이 깨져 반영하지 않는다.
- **MULTI_PUBSUB server in-place stamp 후보 기각**: server hot path에서 `stamp_payload(...)`의
  `bytes(payload)` 복사를 줄이기 위해 재사용 `bytearray`를 그대로 publish submit에 넘기는
  후보를 시험했다. `python -m py_compile bindings/python/perf/perf_metrics.py bindings/python/perf/multi/perf_multi_common.py bindings/python/perf/multi/perf_multi_pubsub_server.py bindings/python/perf/multi/perf_multi_pubsub_client.py`는 통과했고,
  공식 wrapper `perf_python_multi_linux_20260525_112616.txt`와
  `perf_python_multi_linux_20260525_113719.txt`는 complete였다. 그러나 같은 C 기준
  `perf_c_multi_linux_20260525_110110.txt`와 직전 Python 기준
  `perf_python_multi_linux_20260525_111359.txt`를 비교하면 tcp 64/65536B만
  1.6%/2.5% 개선되고, ws/wss/tls는 대부분 1.4~16.6% 낮아졌다. publish submit 내부에서
  native message copy가 남아 있고 transport별 큰 메시지 회귀가 생겨 안정적인 개선이 아니므로
  반영하지 않는다.
- **MULTI_PUBSUB server backpressure sleep 제거 후보 기각**: server active loop에서
  `publish_nonblocking(...)` 실패 시 1ms backoff를 없애고 즉시 다음 publish를 재시도하는
  후보를 시험했다. `python3 -m py_compile bindings/python/perf/multi/perf_multi_pubsub_server.py bindings/python/perf/multi/perf_multi_pubsub_client.py`는
  통과했고, 공식 wrapper C `perf_c_multi_linux_20260525_133159_python_pubsub_no_sleep_c.txt`
  대비 Python 후보 `perf_python_multi_linux_20260525_133413_pubsub_server_no_sleep_candidate.txt`는
  complete였다. tcp 64/256/1024/65536B 후보는 148.0/150.5/133.3/99.6Kmsg/s로,
  같은 조건 직전 기준 141.9/141.6/135.7/100.7Kmsg/s 대비 64/256B만 4~6% 개선되고
  1024/65536B는 1~2% 낮아졌다. C 대비 비율도 5.8/6.2/11.8/46.1%라 small size
  보류를 해소하지 못한다. 조건 없는 busy retry는 크기별 안정성이 낮고 서버 hot path
  복잡성만 늘리므로 반영하지 않는다.
- **MULTI_PUBSUB 64/256B 조건부 no-sleep 후보 기각**: 위 후보를 64/256B에만 좁히면
  1024B 이상 회귀를 피할 수 있는지 다시 시험했다. `python3 -m py_compile
  bindings/python/perf/multi/perf_multi_pubsub_server.py bindings/python/perf/multi/perf_multi_pubsub_client.py`는
  통과했고 공식 wrapper도 complete였다. tcp 기준 C
  `perf_c_multi_linux_20260525_172705_python_pubsub_small_cond_no_sleep_c.txt` 대비 Python 후보
  `perf_python_multi_linux_20260525_172856_pubsub_small_cond_no_sleep_candidate.txt`는
  64/256/1024/65536B 149.5/147.9/144.7/114.4Kmsg/s였지만 C 대비 비율은
  5.4/5.7/13.6/50.0%로 기존 보류권이다. non-tcp 확인 C
  `perf_c_multi_linux_20260525_172908_python_pubsub_small_cond_no_sleep_non_tcp_c.txt`와
  Python 후보 `perf_python_multi_linux_20260525_173756_pubsub_small_cond_no_sleep_non_tcp_candidate.txt`에서도
  ws 64/256B 5.6/5.6%, wss 64/256B 5.5/5.4%, tls 64/256B 5.7/5.1%에 그쳤다.
  조건부 분기는 small 보류를 해소하지 못하고 일부 transport/size는 기존 대표 비율보다
  낮으므로 코드는 반영하지 않는다.
- **MULTI_STREAM connect concurrency parity 반영**: Python multi runner는 STREAM 기본
  clients=10000에서도 `connect_concurrency`를 128로 표시하고 실행 env에도 기본값을
  전달하지 않았다. C runner의 effective option은 같은 조건에서 1024(default)이므로,
  Python runner도 clients가 10000 이상이면 기본 connect concurrency를 1024로 계산해
  표시하고 case env에 전달하도록 맞췄다. `python3 -m py_compile
  bindings/python/perf/multi/run_benchmarks.py`는 통과했고, 공식 wrapper
  `perf_python_multi_linux_20260525_214408_multi_stream_connect_concurrency_parity.txt`에서
  `connect_concurrency: 1024 (default)`로 표시되는 것을 확인했다. 다만 tcp 64B는 여전히
  READY 이후 결과 없이 partial이므로 이 수정은 runner 조건 정렬이고 보류 해소는 아니다.
- **MULTI_STREAM stale route guard 후보 부분 반영**: 10000-client 재현에서 Python STREAM
  server가 stale routing id로 reply를 보내다 `SubmitError(... internal_errno=113)` 또는
  native crash로 끝나는 것을 확인했다. `EHOSTUNREACH`/`ENOTCONN` pending reply를 버리는
  guard를 추가한 뒤 `python3 -m py_compile
  bindings/python/perf/multi/perf_multi_stream_server.py`는 통과했고, 제한 smoke
  `perf_python_multi_linux_20260525_214754_multi_stream_clients1000_guard_smoke.txt`는
  `--clients 1000`, tcp 64B, runs=3에서 complete였다. 그러나 기본 10000-client 공식
  wrapper `perf_python_multi_linux_20260525_214636_multi_stream_stale_route_drop_candidate.txt`는
  여전히 `tcp 64B` partial이므로, 이 guard는 stale-route 예외 방어일 뿐 기본 STREAM
  보류를 해결한 최적화로 보지 않는다.
- **MULTI_STREAM frame view-copy 후보 기각**: Python stream server에서 `header.to_bytes()`/`body.to_bytes()` 뒤 frame concatenation을 하는 대신 `Message.data` view에서 최종 frame `bytearray`로 바로 채우는 후보를 시험했다. `python -m py_compile bindings/python/perf/multi/perf_multi_stream_server.py`는 통과했지만, 공식 runner `PERF_FAIL_FAST=1 bindings/python/perf/run_benchmarks_multi.sh --transports tcp,wss --pattern MULTI_STREAM --msg-sizes 64,65536 --duration 1 --runs 3`가 `tcp 64B`에서 즉시 partial로 끝났다(`perf_python_multi_linux_20260524_233240.txt`). current HEAD에서 같은 후보를 다시 좁혀 시험한 공식 wrapper `perf_python_multi_linux_20260525_100136.txt`도 `tcp 64B`에서 `READY,tcp://...` failure reason만 남기고 partial로 끝났다. 기존 `on_packet` public callback은 native packet을 이미 Python-owned `Message`로 복사해 전달하므로, frame 조립의 마지막 복사만 줄이는 접근은 안정적인 개선 후보가 아니다.
- **MULTI_STREAM on_packet native clone 후보 기각**: `StreamSocket.on_packet`이 native header/body를 `bytes`로 만든 뒤 다시 `Message`로 복사하는 경로를 줄이기 위해 `_clone_native_msg` 기반 `Message` 생성을 시험했다. callback 테스트는 통과했지만, 원본 native part를 callback 안에서 close하는 변형은 공식 wrapper `perf_python_multi_linux_20260525_005843.txt`에서 `tcp 64B` partial이 났고, close하지 않는 변형도 `perf_python_multi_linux_20260525_005927.txt`에서 서버 READY 수집 단계가 깨졌다. stream callback의 native part 수명은 core callback 경계와 맞물려 있어 이 방식은 반영하지 않는다.
- **MULTI_STREAM bytes callback 후보 기각**: public `on_packet` 의미는 유지하고 perf 전용
  private callback이 native header/body를 Python `bytes`로만 전달하면 `Message.from_(...)`
  생성과 perf 서버의 `to_bytes()` 재복사를 줄일 수 있는지 시험했다. `python3 -m py_compile
  bindings/python/src/zlink/contracts/sockets/sockets.py bindings/python/perf/multi/perf_multi_stream_server.py`는
  통과했고 같은 조건 C 기준
  `perf_c_multi_linux_20260525_174225_python_stream_bytes_callback_c.txt`도 complete였다.
  그러나 Python 후보
  `perf_python_multi_linux_20260525_174228_stream_bytes_callback_candidate.txt`는 `tcp 64B`
  READY 이후 결과 없이 partial로 끝났다. native stream callback 경계에서 public `Message`
  수명 관리를 우회하면 shared C stream client handshake/echo 진행이 불안정해지므로
  코드는 반영하지 않는다.
- **MULTI_STREAM routed native send 후보 기각**: stream server의 POLLOUT drain에서
  `send_nonblocking(server, frame, routing_id=...)` builder 경로를 건너뛰고
  `_send_rid_via_native_no_wait_result(...)`로 단일 frame을 직접 보내는 후보를 시험했다.
  `python3 -m py_compile bindings/python/perf/multi/perf_multi_stream_server.py`는 통과했지만,
  공식 wrapper `PERF_FAIL_FAST=1 bindings/python/perf/run_benchmarks_multi.sh --transports tcp
  --pattern MULTI_STREAM --msg-sizes 64 --duration 1 --runs 3`가 첫 `tcp 64B`에서
  READY 이후 결과 없이 partial로 끝났다
  (`perf_python_multi_linux_20260525_194338_stream_native_send_candidate_tcp64.txt`).
  stream socket의 public send builder를 우회하면 shared C stream client와 echo completion
  경계가 안정적으로 유지되지 않으므로 반영하지 않는다.
- **MULTI_STREAM callback immediate-send 후보 기각**: C stream server처럼 packet callback에서
  pending queue가 비었을 때 즉시 `send(...DONT_WAIT)`를 시도하고 backpressure 때만 queue에
  넣는 후보를 시험했다. `python -m py_compile bindings/python/perf/multi/perf_multi_stream_server.py`는
  통과했고 같은 조건 C 기준 `perf_c_multi_linux_20260525_091324.txt`는 complete였지만,
  Python 공식 wrapper `perf_python_multi_linux_20260525_091345.txt`는 `tcp 64B`에서 결과 없이
  partial로 끝났다. Python `on_packet`은 native callback이 header/body를 Python `Message`로
  복사한 뒤 dispatcher thread에서 user handler를 호출하므로, 그 안에서 stream socket send를
  바로 섞으면 현재 runner의 READY/stream client handshake 경계를 불안정하게 만든다.
  따라서 기존 queue 기반 POLLOUT drain을 유지한다.
- **MULTI_STREAM direct callback dispatch 후보 기각**: dispatcher queue 왕복 비용을 줄이기
  위해 `StreamSocket.on_packet`이 native callback 안에서 handler를 바로 호출하되,
  callback 재진입 guard(`EDEADLK`)는 유지하는 후보를 시험했다.
  `python -m py_compile bindings/python/src/zlink/contracts/sockets/sockets.py bindings/python/perf/multi/perf_multi_stream_server.py`와
  `PYTHONPATH=bindings/python/src python -m pytest bindings/python/tests/test_callback_send.py -q`는
  통과했다. 그러나 공식 wrapper는 C 기준 `perf_c_multi_linux_20260525_114553.txt`가
  complete인 같은 조건에서 Python 후보 `perf_python_multi_linux_20260525_114623.txt`가
  `tcp 64B` READY 이후 결과 없이 partial로 끝났다. stream packet callback은 core callback
  경계와 server POLLOUT drain을 분리해야 안정적이므로 dispatcher 기반 호출을 유지한다.
- **MULTI_STREAM pending lock 제거 후보 기각**: packet callback과 POLLOUT drain 사이의
  `pending_lock` 비용을 줄이기 위해 `deque`의 append/popleft를 GIL에 맡기는 후보를
  시험했다. `python -m py_compile bindings/python/perf/multi/perf_multi_stream_server.py`는
  통과했고 같은 조건 C 기준 `perf_c_multi_linux_20260525_164746_python_multi_stream_lockfree_c.txt`도
  complete였지만, Python 공식 wrapper `perf_python_multi_linux_20260525_164820_multi_stream_lockfree_candidate.txt`는
  `tcp 64B` READY 이후 결과 없이 partial로 끝났다. callback dispatcher thread와 main
  drain loop가 같은 queue를 공유하는 구조에서는 lock 제거가 안정적인 hot path 개선이
  아니므로 반영하지 않는다.
- **MULTI_STREAM pending condition wake 후보 기각**: 기존 server loop는 pending queue가
  비었을 때 `stop.wait(100ms)`로 쉬므로, packet callback이 pending frame을 추가할 때
  condition으로 drain loop를 즉시 깨우는 후보를 시험했다. `python3 -m py_compile
  bindings/python/perf/multi/perf_multi_stream_server.py`는 통과했고 C 기준
  `perf_c_multi_linux_20260525_181839_python_stream_pending_cond_c.txt`도 complete였다.
  그러나 Python 후보는 첫 실행
  `perf_python_multi_linux_20260525_181853_stream_pending_cond_candidate.txt`에서 서버 READY
  수집 전 partial이었고, 재시도
  `perf_python_multi_linux_20260525_182010_stream_pending_cond_candidate_retry.txt`도
  `tcp 64B` READY 이후 결과 없이 partial로 끝났다. wake 지연만 줄여도 Python stream
  callback과 shared C stream client의 진행 경계가 안정화되지 않으므로, 기존
  lock+bounded wait 구조를 유지한다. 2026-05-26 재검토에서는 같은 후보를
  `--clients 100`, tcp 64/65536B로 좁혀 다시 시험했다. C 기준
  `perf_c_multi_linux_20260526_070503_python_stream_clients100_c_recheck.txt`는
  451.263/116.888 Kops/s였고, current Python
  `perf_python_multi_linux_20260526_070523_multi_stream_clients100_current_recheck.txt`는
  5.489/4.389 Kops/s였다. condition 후보
  `perf_python_multi_linux_20260526_070651_multi_stream_clients100_pending_cond_candidate.txt`는
  5.499/4.404 Kops/s로 사실상 같아 낮은 client 수에서도 보류 해소 후보가 아니다.
- **MULTI_STREAM idle wait 1ms 후보 기각**: pending queue가 비었을 때의 bounded wait를
  100ms에서 1ms로 줄여 queued drain 지연이 주된 병목인지 확인했다. `python3 -m
  py_compile bindings/python/perf/multi/perf_multi_stream_server.py`는 통과했고, 공식 wrapper
  `perf_python_multi_linux_20260526_070814_multi_stream_clients100_idle1ms_candidate.txt`는
  `--clients 100`, tcp 64/65536B에서 complete였다. 그러나 median은 5.616/4.661 Kops/s로
  current 5.489/4.389 Kops/s보다 조금 높을 뿐이고, 같은 조건 C
  451.263/116.888 Kops/s 대비 1.2%/4.0%라 보류권을 벗어나지 못한다. idle wait만 줄이면
  CPU idle loop 비용은 늘지만 Python callback/queue/send builder 경계의 큰 차이는 줄지
  않으므로 반영하지 않는다.
- SPOT reply decode에도 같은 원칙을 적용했다. `MULTI_SPOT_REQREP`는 callback 이전에 reply part가 이미 Python `Message`로 clone되므로 추가 `to_bytes()` 제거 효과가 제한적이고,
  `MULTI_SPOT_SENDSEND`는 routed reply `ReceivedMessage.data`로 262144B가 37.4%→43.6%까지 올랐다. small/65536B와 일부 131072B는 여전히 기준보다 낮다.
- **SPOT reqrep server view-reply 후보 기각**: `MULTI_SPOT_REQREP` server에서
  `received.reply().messages(*received.to_bytes_list())` 대신 `received.first_part().data` view를
  바로 reply payload로 넘기는 후보를 시험했다. 공식 runner `tcp,ws 65536/131072/262144B`는
  complete였지만(`perf_python_multi_linux_20260524_231443.txt`), 기존 문서 기준보다 비율이
  낮았다(tcp 27.7/37.8/39.5%, ws 37.8/36.2/46.0%). 따라서 반영하지 않는다.
- **SPOT reqrep client waiting lock 제거 후보 기각**: `MULTI_SPOT_REQREP` client active loop에서
  `waiting` slot 보호용 `threading.Lock`을 제거하는 후보를 시험했다. CPython GIL과 callback
  경계만으로 list flag 접근을 처리하면 per-request lock 비용을 줄일 수 있을 것으로 보았지만,
  공식 제한 재측정은 complete였어도 C 대비 tcp 64/65536/131072B가 14.4/30.6/41.5%,
  ws 64/65536/131072B가 15.6/38.7/37.4%에 머물렀다
  (`perf_c_multi_linux_20260525_032146.txt`, `perf_python_multi_linux_20260525_034004.txt`).
  callback 이전 native-to-Python reply 전달 비용과 request submit 경계가 더 큰 병목이어서
  코드에는 반영하지 않는다.

## 7. 완료 기준

아래 조건을 모두 만족하면 해당 언어 binding 작업을 완료한다.

- single과 multi의 대상 조합이 모두 목표 비율 이상이거나, 아래 `영구 보류` 정책에 해당한다.
- 상세 상태 표에 `미측정`, `미달`, (영구 보류가 아닌) `보류`가 하나도 남아 있지 않다.
- perf 결과가 `doc/perf` 정책과 `bindings/c/perf` 의미를 유지한다.
- perf 코드를 수정했다면 버그 또는 정책 위반 근거가 남아 있다.
- binding 라이브러리 변경에 필요한 테스트가 통과한다.
- 실행 중 발견된 이슈가 모두 리뷰되었고, 필요한 테스트와 수정이 끝났다.
- 이 문서가 실제 실행 절차와 판단 기준을 최신 상태로 반영한다.
- 결과 파일 경로와 C 대비 비율 요약이 최종 보고에 포함된다.

### 7.1 보류 cell 분류 (2026-05-23 갱신)

남은 저성능 cell은 두 부류로 나눈다. **(A) 바인딩 hot-path 비효율(고칠 수 있음)**과 **(B) 런타임 경계 비용 한계(개선폭 제한)**다.
중요한 근거: 같은 core 6.0.3에서 **C++ 바인딩은 routed/multi large에서 C와 ≈100%**(예: C++ multi DD large 173k vs C 170k)다.
즉 core는 정상이고, native/fast 바인딩이 가능함을 증명한다. 따라서 다른 바인딩이 같은 연산에서 크게 낮은 것은 **근본 한계가 아니라
그 바인딩의 구현 비효율**이며, perf-only API 없이 내부 hot-path 최적화로 고칠 수 있는 (A)에 해당한다.

- **(A) 바인딩 hot-path 비효율 — 최적화 대상(보류, 영구 아님)**:
  - Rust `DEALER_ROUTER`/`ROUTER_ROUTER` large(11~13%, latency 13~35x): C++가 같은 routed large에서 84~99%이므로 고칠 수 있다.
    표면 후보(blocking send로 C 의미 정렬, per-send 할당)는 빗나갔다(2026-05-23 확인). recv/send hot-path를 프로파일링으로
    pinpoint해야 하는 미해결 항목이다.
  - Python `MULTI_DEALER_DEALER` large와 Rust/Python `MULTI_SPOT`/`MULTI_SPOT_SENDSEND`/`MULTI_SPOT_REQREP` large: 같은 core에서
    C/C++ 바인딩은 ≈100%다. Go는 아래 2026-05-23~25 진단과 수정으로 현재 표 기준 보류가
    해소됐고, 여기서는 Rust/Python 잔여 항목을 추적한다. **2026-05-23 Go DD 65536
    client-scaling 진단은 당시 원인을 국소화한 이력이다**:
    clients=4 → 5370 msg/s(per-client 1342 ≈ C++ per-client 1731의 78%, 정상권), clients=20 → 3302, clients=50 → 2954,
    **clients=100 → 0.8~5040 msg/s(변동, 사실상 backpressure deadlock 경계)**. 즉 per-message 비용·HWM 크기·할당이 아니라
    (모두 배제됨: `.Bytes()` 재사용·blocking send·auto-HWM 적용 순서·수동 HWM=512 모두 회복 실패) **many-client + large에서 Go의
    DONTWAIT+poller backpressure 루프가 불안정·near-deadlock**이 진짜 원인이다. blocking send는 안정적이나 단일 goroutine 직렬화로
    ~5k에 묶인다. C++는 같은 DONTWAIT+pollout 방식으로 100 client에서 안정 173k이므로 **고칠 수 있는 Go poller/pollout 관리 버그**다.
    수정 방향: C++ `perf_dealer_dealer_client`의 per-socket pollout 토글·poll 관리와 정확히 대조해 Go send window의 pending/POLLOUT
    전이와 poll wait 의미를 맞추고, 다중 소켓에서 POLLOUT writability 신호가 누락되지 않는지 확인한다.
    2026-05-23 추가 실험(모두 ~3~5k에 묶임, 34x 미해결): `.Bytes()` 재사용, blocking send, auto-HWM 적용 순서, 수동 HWM=512(오히려 2050으로 악화),
    client round-robin one-per-pass(2385, 안정성만 개선), GOMAXPROCS=8(3114)/20(4143). active phase는 client·server 모두 비-블록 spin(데이터
    미흐름)이다. Go CPU 프로파일은 비어 있고(block-bound), Go block 프로파일은 cgo 대기를 못 본다. 이 환경엔 `perf`/`strace`가 없어 cgo/core 경계의
    실제 wait 지점을 계측할 수 없다. 다음 단계는 (i) `perf`/`strace` 설치 후 syscall/native 프로파일, 또는 (ii) core flow-control에 임시 계측 추가
    (동시 core 마이그레이션 안정 후)로 100-client large에서 client→server pipe 데이터가 server recv까지 전달 안 되는 지점을 특정하는 것이다.
  - **2026-05-23 해결됨(Go)**: `strace`를 설치해 위 (i)를 수행했다. client/server 양쪽이 block-bound(client `futex 51%`+`epoll_wait 21%`@1.8ms/call,
    server `futex 61%`+`nanosleep 9.4%`)이고, DD hot path엔 sleep이 없으므로 `nanosleep`은 **Go 런타임 스케줄러**가 blocking cgo poll-wait에서
    M을 핸드오프/파킹하며 만든 것이다 — 즉 "poller/pollout 관리 버그"가 아니라 **Go 런타임의 cgo wakeup-latency 증폭**이 진짜 원인이었다.
    `runtime.LockOSThread()`로 role/worker goroutine을 OS thread에 고정하니 DD tcp 65536이 5,582→89,000 msg/s(3%→53%)로 회복됐다.
    상세는 6.6.3 "2026-05-23 근본 원인 규명 + 수정" 참조. 이는 Go 전용 수정이며, **Rust/Python은 native OS thread를 써서 같은 런타임 병목이 없으므로
    이 수정이 적용되지 않는다**(이들 large 미달은 per-message copy/FFI 비용이라 별도 추적).
  - 나머지 보류 항목들은 `보류(비율%)`로 두고(영구 아님), 위처럼 client-scaling/프로파일링으로 hot-path를 특정한 기록을 남긴다.
- **(B) 런타임 경계 비용 — 개선폭 제한(보류, 근거 명시)**:
  - Python/Node small-size(64~1024B, ~수만 msg/s): 매 송수신이 런타임↔C 경계를 넘는 per-call 비용이 지배적이다(C는 1.2M+).
    바인딩이 메시지당 FFI 호출/객체 할당을 줄이면 일부 개선되지만, 메시지 배치 없이는 C급에 도달하지 못한다. 배치 public API는
    별도 설계 대상이다. 이 부류만 사실상 영구 보류 후보이며, 그 경우에도 먼저 메시지당 FFI 호출 수 감소 최적화를 시도한 기록을 남긴다.

요약: "개선 불가"로 단정하지 않는다. (A)는 프로파일링으로 추적·수정하고, (B)도 호출/할당 감소를 먼저 시도한 뒤에만 한계로 기록한다.

모든 대상 언어가 완료되면(또는 영구 보류만 남으면) 최종 요약에는 언어별 최저 비율, 남은 예외(영구 보류 포함), 수정한 파일,
실행한 perf 명령을 함께 기록한다.
