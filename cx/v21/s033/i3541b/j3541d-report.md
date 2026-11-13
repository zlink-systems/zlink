# #3541 Core TCP connect session 배치 진단

## 판정

수정 뒤 connect session 자체의 transport 선택은 TCP와 TLS 모두 **1회**입니다. TCP connecter 내부에만 존재하는 추가 선택은 없습니다. 추가 소비자는 같은 ctx의 publisher listener가 만드는 **별개의 accept session**입니다. TCP와 TLS listener 모두 같은 `_next_transport_io_thread`를 증가시킵니다.

따라서 전체 transport session 선택이 정확한 round-robin이어도, 그중 connect session만 집계하면 1·3번에 집중될 수 있습니다. TCP의 실패는 이 공유 카운터와 부분집합 집계로 설명할 수 있습니다. 실제 50회 실행의 호출 순서 기록은 제공되지 않았으므로, 각 회차의 실행 순서와 TLS가 항상 균등했던 구체적인 시간 관계는 후보로 구분합니다.

기존 수정은 session과 connecter의 중복 선택을 제거합니다. 잔여 분포만으로 이 수정이 잘못되었다고 판정하거나, TCP에 추가 선택이 남아 있다고 판정할 근거는 없습니다.

## 조사 범위와 근거

- 읽기 전용 worktree: `D:/worktree/zlink-3541d`.
- 확인한 HEAD: `f6b11cafd5461325c026765bc85e4739e4689d17`.
- `76a4d78506`과의 해당 diff에서 `session_base_t::start_connecting()`의 선택 호출이 `_io_thread` 재사용으로 바뀐 것을 확인하였습니다.
- 코드·스펙 변경, 빌드, 시험 실행은 하지 않았습니다. 실행 분포와 반복 횟수는 브리프의 관찰값입니다.
- 아래 `core/...` 경로는 모두 지정 worktree를 기준으로 합니다.

## 선택기와 전체 소비자

선택 경로는 `object_t::choose_io_thread_transport()` → `ctx_t::choose_io_thread_transport()` → `ctx_runtime_resources_t::choose_io_thread_transport()` → `ctx_io_thread_registry_t::choose_transport()`입니다. 각각의 위치는 다음과 같습니다.

| 계층 | 위치 | 동작 |
|---|---|---|
| object | `core/src/runtime/core/object.cpp:276` | ctx로 위임합니다. |
| ctx | `core/src/runtime/core/ctx.cpp:339` | runtime resources로 위임합니다. |
| runtime resources | `core/src/runtime/core/ctx_runtime_resources.cpp:88` | ctx에 속한 registry로 위임합니다. |
| registry | `core/src/runtime/core/ctx_io_thread_registry.cpp:91` | transport 배치를 결정합니다. |
| 카운터 증가 | `core/src/runtime/core/ctx_io_thread_registry.cpp:99` | `_next_transport_io_thread.add(1) % size`를 계산합니다. |

`_next_transport_io_thread`의 정상 선택 중 증가는 위 `:99` 한 곳입니다. 초기값은 `ctx_io_thread_registry.cpp:32`의 0이며, `clear()`의 `:41`에서 0으로 초기화합니다. `atomic_counter_t::add()`는 증가 전 값을 반환합니다(`core/src/runtime/utils/atomic_counter.hpp:70`, Windows 구현 `:76`). affinity 기본값은 0입니다(`core/src/runtime/core/options.cpp:81`). 이 시험에서는 affinity를 바꾸지 않으므로 선택 번호는 증가 전 카운터의 modulo 4입니다.

production 소스의 transport 선택 호출자를 전수 검색한 결과는 다음과 같습니다.

| 호출자 | 위치 | 이 시험에서의 실행 여부 |
|---|---|---|
| 기본 connect session | `core/src/runtime/sockets/common/socket_base_endpoint.cpp:533` | TCP·TLS의 SUB connect마다 1회입니다. |
| transport pair completion session | 같은 파일 `:910` | 실행하지 않습니다. `:504`의 pair 대상은 DEALER·ROUTER이며 시험은 PUB·SUB입니다. |
| TCP accepted session | `core/src/runtime/transports/tcp/asio_tcp_listener.cpp:248` | TCP accept 성공마다 1회입니다. |
| TLS accepted session | `core/src/runtime/transports/tls/asio_tls_listener.cpp:301` | TLS accept 성공마다 1회입니다. |
| IPC accepted session | `core/src/runtime/transports/ipc/asio_ipc_listener.cpp:302` | 이 시험에서는 실행하지 않습니다. |
| WS/WSS accepted session | `core/src/runtime/transports/ws/asio_ws_listener.cpp:326` | 이 시험에서는 실행하지 않습니다. |

listener의 STREAM 분기는 별도 `choose_io_thread_stream()`을 사용합니다. 이 시험의 publisher는 PUB이므로 transport 분기를 사용합니다. IPC도 동일한 session 시작 경로에서 이미 선택한 thread를 connecter에 전달합니다(`core/src/runtime/core/session_base.cpp:871`). IPC/TCP 공통 경로에 숨은 추가 transport 증가 함수는 없습니다.

## TCP·TLS 호출 경로와 횟수

| 단계 | TCP 경로 | TLS 경로 | transport 선택 횟수 |
|---|---|---|---|
| connect 요청 | `socket_base_endpoint.cpp:218` → `connect_internal`, `:234`, `:246` → 선택 `:533` | 동일합니다. | connect session당 1회입니다. |
| 주소 준비·session 생성 | `socket_base_endpoint.cpp:632`, `:642`, `:647` → `:664` | 동일합니다. | 추가 0회입니다. |
| 최초 주소 검증 | `socket_base_endpoint_factory.cpp:161`, TCP/TLS 공통 분기 `:166` → `:193` | 동일합니다. | 추가 0회입니다. |
| session 등록·비동기 시작 | `socket_base_endpoint.cpp:739` → `add_endpoint`, `:974`, `:982` → `own.cpp:61`, `:67` → `session_base.cpp:469`, `:472` | 동일합니다. | 추가 0회입니다. |
| connecter 생성 | `session_base.cpp:853`의 `_io_thread` → `:861` TCP connecter | 같은 `:853` → `:867` TLS connecter | 추가 0회입니다. |
| connecter의 I/O context | `asio_tcp_connecter.cpp:76`에서 전달된 thread의 context를 사용합니다. | `asio_tls_connecter.cpp:106`에서 동일하게 사용합니다. | 추가 0회입니다. |
| connect·주소 해석 | `asio_tcp_connecter.cpp:158`, resolve `:172`, async connect `:238` | `asio_tls_connecter.cpp:183`, SSL context 준비 `:194`, resolve `:208`, async connect `:263` 이후 | 추가 0회입니다. |
| outgoing engine 생성·attach | `asio_tcp_connecter.cpp:288` → `:315`, engine `:324`, attach `:332` | `asio_tls_connecter.cpp:320` → `:367`, engine `:398`, attach `:408` | 추가 0회입니다. |
| session의 engine plug | `session_base.cpp:475`, `:485`에서 `_engine->plug(_io_thread, this)` | 동일합니다. | 추가 0회입니다. |
| bind 측 TCP accept | `asio_tcp_listener.cpp:152` → engine 생성 `:223`, `:228` → 선택 `:248` → passive session 생성 `:252` → attach `:257` | 해당하지 않습니다. | accepted session당 1회입니다. |
| bind 측 TLS accept | 해당하지 않습니다. | `asio_tls_listener.cpp:188` 이후 TCP accept 처리 → SSL context 생성 `:232` → engine 생성 `:246`, `:269` → 선택 `:301` → passive session 생성 `:305` → attach `:310` | accepted session당 1회입니다. |
| 재연결 | `asio_tcp_connecter.cpp:300`의 timer → `:141`의 `start_connecting()` 또는 session의 `start_connecting()` 재진입 | `asio_tls_connecter.cpp:166`의 timer → `start_connecting()` 또는 session 재진입 | 같은 session의 재시도에는 추가 0회입니다. 새 성공 accept가 생기면 그 별개 passive session에 1회입니다. |

위 표의 짧은 파일명은 각각 `core/src/runtime/sockets/common/`, `core/src/runtime/core/`, `core/src/runtime/transports/tcp/`, `core/src/runtime/transports/tls/` 아래의 파일입니다.

성공한 loopback 연결을 양쪽 session 생성까지 계산하면 TCP와 TLS 모두 **connect 1회 + accept 1회 = 2회**입니다. connect 호출이 반환될 때 accept까지 완료된다는 보장은 없습니다. 집계 시점에는 connect 선택 100회와 그때까지 진행한 accept 선택 A회가 공유 카운터에 반영됩니다. 정상적으로 각 연결을 한 번씩 수락한 경우 `0 ≤ A ≤ 100`입니다. 재연결로 새 accept가 추가되는 상황은 별도입니다.

## 시험이 함께 만드는 객체와 집계 범위

시험은 `core/tests/unittest/unittest_connect_io_thread_choice.cpp:60`에서 ctx 하나를 생성합니다. 같은 ctx에 publisher PUB 하나를 만들고(`:68`) bind합니다(`:84`). 이후 SUB 100개를 생성하고(`:100`–`:119`) 같은 endpoint에 connect합니다. PUB listener의 accepted session과 SUB의 connect session은 같은 registry를 사용합니다.

| 객체·동작 | 카운터 영향과 근거 |
|---|---|
| publisher listener 배치 | `socket_base_endpoint.cpp:209`의 일반 `choose_io_thread()`입니다. `ctx_io_thread_registry.cpp:72`의 `_next_io_thread`를 사용하므로 transport 카운터를 소비하지 않습니다. |
| thread 주소 조회 4회 | 시험 `:89`–`:92`의 affinity 지정 `choose_io_thread()`입니다. 일반 카운터만 증가합니다. transport 선택의 시작 번호를 4회 이동시키지 않습니다. |
| SUB socket 생성·command owner 배치 | socket당 객체 배치의 일반 선택과 transport session 선택은 구분됩니다. transport 호출자 전수 검색에서 socket 생성 자체의 추가 소비자는 없습니다. |
| PUB accepted session | TCP listener `:248`, TLS listener `:301`이 transport 카운터를 소비합니다. **시험의 counts에는 포함하지 않습니다.** |
| monitor | 시험은 monitor를 설치하지 않습니다. event 발행 자체는 transport 선택 호출자가 아닙니다. |
| reaper | `ctx_runtime_resources.cpp:107`에서 고정 `ctx_t::reaper_tid`로 생성하며 `:116`에서 시작합니다. transport 선택을 하지 않습니다. |
| completion lane | PUB·SUB는 `socket_base_endpoint.cpp:504`의 paired transport 대상이 아니므로 `:910` 선택을 추가하지 않습니다. |
| close·terminate | 시험 `:123`–`:128`에서 counts를 확정한 뒤 `:129`부터 실행합니다. 이미 얻은 분포의 원인이 아닙니다. |

집계 helper는 전달된 subscribers의 endpoint만 순회합니다(`unittest_connect_io_thread_choice.cpp:27`–`:35`). 그 session의 `_io_thread`를 셉니다(`:38`–`:40`). `_io_thread`는 session 생성자에서 전달값으로 초기화합니다(`core/src/runtime/core/session_base.cpp:94`). attach는 이 thread를 그대로 사용하므로 handshake 완료를 기다리지 않은 것이 connect session 배치 집계의 누락 원인은 아닙니다.

100개 합계와 최대·최소 차이 검사는 시험 `:135`–`:136`입니다. 첫 검사는 생성한 connect session 수를 확인하지만, 둘째 검사는 전체 선택열에서 **connect 부분집합만** 균등할 것을 요구합니다.

## 짝 패턴의 설명

connect 선택을 C, accept 선택을 A라고 합니다. i번째 connect를 0부터 세고, 그 전에 완료된 accept 선택 수를 `a_i`라고 하면 이 시험의 connect 배치 번호는 다음과 같습니다.

```text
thread(C_i) = (i + a_i) mod 4
thread(C_(i+1)) - thread(C_i) = 1 + (a_(i+1) - a_i)   (mod 4)
```

이 식은 affinity 0, 다른 transport 소비자가 없는 현재 fixture에 적용됩니다. 연속한 connect 사이에 accept 선택이 1회씩 발생하면 connect 번호는 2칸씩 진행합니다. 초기 진행이 `C0, C1, A0, C2, A1, C3, ...`이면 connect 번호는 `0, 1, 3, 1, 3, ...`입니다. 초기 C0 이후에는 1·3번이 반복되므로 0·2번의 connect 수가 적어집니다. accept가 나머지 번호를 선택하므로 전체 선택열의 round-robin은 유지됩니다.

중요한 점은 1·3번에 고정된 선호 조건이 코드에 없다는 사실입니다. 초기 위상이나 burst의 길이가 달라지면 0·2번 집중도 가능합니다. 제공된 TCP 회차가 계속 홀수 번호에 집중된 구체적인 startup 순서는 trace가 있어야 확정할 수 있습니다. 코드에서 확정할 수 있는 것은 교차 선택이 같은 홀짝 부분집합을 반복시키는 구조입니다.

예를 들어 다음은 **13/37/14/36을 정확히 만드는 가능한 선택열**입니다. 실제 실행을 복원한 것이 아니라 공유 카운터만으로 해당 분포가 가능함을 보이는 예입니다.

```text
C 52회 → A 2회 → C 1회 → A 2회 → (C → A) 46회 → C 1회
```

처음 52개의 C는 각 thread에 13개씩 배치됩니다. 다음 C는 카운터 54에서 thread 2를 선택합니다. 나머지 47개의 C는 카운터 57부터 2칸씩 진행하여 thread 1에 24개, thread 3에 23개를 추가합니다. 최종 connect 분포는 `13/37/14/36`입니다. 이때 accept 선택은 50회이며, 나머지 accept 50회까지 진행하면 전체 200개 session은 각 thread에 50개씩 배치됩니다.

회차마다 `a_i`가 달라지므로 분포도 달라집니다. connect를 연속 생성하는 애플리케이션 thread와 listener의 비동기 accept callback 사이에는 매 연결의 선택 순서를 고정하는 동기화가 없습니다. burst 구간에서는 C가 1칸씩 진행하고, C/A 교대 구간에서는 2칸씩 진행합니다. 이 구간의 시작과 길이가 `2/48/7/43`, `5/45/6/44` 같은 비율 차이를 만들 수 있습니다. 무작위로 네 thread를 독립 추출하는 시험이 아니므로 connect 100개가 대략 25개씩이어야 한다는 통계 전제도 적용되지 않습니다.

## 수정 전후와 TLS 차이

수정 전에는 outgoing session 선택 C 외에 `session_base_t::start_connecting()`의 connecter 선택 K가 같은 카운터를 소비합니다. C/K/A가 규칙적으로 교차하는 구간에서는 C가 3칸씩 진행할 수 있습니다. 3과 4는 서로소이므로 네 thread를 모두 방문합니다. 중복 선택이 존재하는 코드가 해당 실행 순서에서는 더 균등해 보일 수 있습니다. 수정 전 TCP `27/23/27/23`은 결함이 없다는 증거가 아닙니다. 다만 해당 수치의 실제 C/K/A 순서는 기록이 없으므로 확정하지 않습니다.

TLS에도 accept 선택이 존재합니다. TLS가 통과했다고 해서 transport 선택 횟수가 TCP보다 적다고 결론 내릴 수 없습니다. TLS는 outgoing connecter가 async TCP connect를 제출하기 전에 SSL context를 준비합니다(`asio_tls_connecter.cpp:194`, 이후 `:263`). listener도 선택 전에 SSL context를 생성합니다(`asio_tls_listener.cpp:232`, 선택 `:301`). TCP에는 이 준비 단계가 없습니다.

이 차이로 TLS의 accept 선택이 connect session 생성 100회보다 늦게 진행되었을 가능성이 있습니다. 모든 C가 먼저 진행하면 정확히 `25/25/25/25`가 됩니다. accept가 중간에 진행했더라도 균형 잡힌 위치에서 교차하면 같은 분포가 가능합니다. 따라서 50회 전부 통과했다는 결과만으로 accept가 전부 나중이었다고 단정할 수 없습니다. SSL 준비에 걸린 시간과 실제 선택 순서는 측정이 필요한 후보입니다. TLS handshake 완료 대기 때문에 선택이 늦어진다고 단정해서도 안 됩니다. listener 선택은 engine의 session attach 전에 수행됩니다.

## 원인 판정표

| 판정 | 항목 | 근거 |
|---|---|---|
| 확정 | TCP·TLS outgoing connect session은 수정 뒤 각각 1회만 선택합니다. | `socket_base_endpoint.cpp:533`, `session_base.cpp:853`. |
| 확정 | 같은 ctx의 accepted session이 추가로 같은 카운터를 소비합니다. | TCP listener `:248`, TLS listener `:301`, registry `:99`. |
| 확정 | 시험은 accept 소비자를 제외한 connect 부분집합만 균등하다고 단언합니다. | 시험 `:27`–`:40`, `:124`, `:136`. |
| 확정 | connect/accept 교대는 connect의 modulo 4 진행을 2칸으로 만듭니다. | registry `:99`와 위 계산식. |
| 후보 | 실제 TCP 실패 회차에서는 초기 C burst 뒤 홀수 위상의 C/A 교대 구간이 길었습니다. | 관찰값과 부합하지만 실행 순서 trace가 없습니다. |
| 후보 | TLS SSL context 준비가 accept 선택을 충분히 지연하여 connect 분포가 균등했습니다. | TLS connecter `:194`, listener `:232`; 지연 시간은 측정하지 않았습니다. |
| 근거 없음 | TCP resolve·engine 생성·attach·connecter retry 안에 TCP 전용 transport 선택이 남았습니다. | production transport 선택 호출자 전수 검색에 해당 호출자가 없습니다. |
| 배제 | monitor·reaper·thread 주소 조회·completion lane이 이 fixture의 추가 transport 소비자입니다. | 위 객체·집계 범위 표의 근거. |

## 결정 소유와 수정 방향

| 결정 | 현재 결정 위치 | 제안하는 소유 |
|---|---|---|
| session의 배치 정책 | ctx registry의 `choose_transport()` | **ctx의 `ctx_io_thread_registry_t`**가 계속 소유합니다. transport별 분산 정책을 추가하지 않습니다. |
| session 생애 중 최초 배치 요청 | connect의 endpoint `:533`, 각 listener의 accepted session 생성 직전, pair completion `:910` | 필요 시 `session_base_t::create()`의 session 생성 경계로 통합하여 session당 한 번만 registry에 요청합니다. |
| connecter·engine·reconnect의 실행 thread | session이 가진 `_io_thread` | session이 소유한 기존 값을 모든 하위 객체가 사용합니다. |

정책 결정자는 registry이며, session은 결정된 thread의 생애를 소유합니다. 생성 경계를 통합한다면 호출자가 미리 선택하고 factory도 다시 선택하는 구조가 되지 않도록 기존 선택 호출을 옮겨야 합니다. connect/accept/completion/STREAM의 기존 계약을 모두 유지해야 합니다. 이는 선택 요청 위치를 모으는 구조적 방향이며, connect 부분집합의 엄격한 균등성을 보장하는 수정은 아닙니다.

현재 브랜치의 `_io_thread` 재사용은 위 소유 구조에 부합합니다. 원래 outgoing session과 connecter의 선택 요청 2회는 1회가 됩니다. loopback의 active/passive session은 서로 다른 객체이므로 합계 2회가 정당합니다. 둘을 같은 객체의 중복 결정으로 취급해서는 안 됩니다.

공유된 단일 round-robin 선택열에서는 다른 소비자가 임의로 교차할 때 특정 부분집합까지 항상 균등하게 만드는 보장이 성립하지 않습니다. 따라서 이 진단만으로 제안할 수 있는 잔여 TCP 전용 구현 수정은 없습니다. 먼저 아래 BLOCKERS의 공정성 범위를 확정해야 합니다. 카운터 추가, accept를 listener thread에 고정, TCP만 별도 처리, timeout·retry 증가, 기존 assertion 완화는 제안하지 않습니다. 호출 위치 통합만으로 제공된 실패가 사라진다고 주장하지 않습니다.

## 검증할 시험 형태

기존 same-ctx PUB/SUB 재현과 `100`, `max-min ≤ 1` 기대값은 유지한 상태로 조사해야 합니다. 아래 검사는 기존 실패를 통과시키기 위한 대체 fixture가 아니라 원인과 선택 소유권을 확인하는 추가 검증입니다.

1. **동일 fixture의 선택 순서 기록**: transport 증가 지점의 반환 전 카운터 값, 선택 thread, 해당 session의 active/passive 구분과 identity를 한 선택 사건으로 연결합니다. 출력 순서가 다른 thread의 기록에 따라 바뀔 수 있으므로 로그 줄 순서 대신 카운터 순서로 분석합니다. C 사이에 A가 몇 번 있었는지로 counts를 재계산하고 기존 counts와 일치하는지 확인합니다. TLS도 같은 방식으로 C/A 교차를 측정합니다.
2. **session당 1회 선택 검사**: TCP·TLS·IPC에서 최초 생성, connecter 시작, engine attach, 같은 session의 reconnect를 구분합니다. 최초 session 생성만 1회이며 하위 단계는 선택을 추가하지 않는지 확인합니다. reconnect 이후 서버에 새 passive session이 생기는 사건은 별도로 셉니다. 제거한 connecter 선택을 다시 넣으면 이 검사가 실패해야 합니다.
3. **전체 선택열 검사**: 기존 same-ctx fixture에서 100개 C와 실제 생성된 A를 모두 기록합니다. affinity 0에서 선택 사건 번호와 thread가 modulo 4로 일치하는지 확인합니다. C 집계와 C+A 집계를 함께 표시하되, 기존 connect-only 실패를 성공으로 바꾸는 근거로 사용하지 않습니다.
4. **제어된 교차 순서 검사**: 선택기를 호출하는 작은 단위시험에서 `C,C,A,C,A,...` 순서를 만들어 전체 round-robin과 connect 부분집합의 편중이 동시에 존재함을 확인합니다. 위 `13/37/14/36` 예도 고정 순서로 검증할 수 있습니다. 이 검사는 실제 callback 순서를 입증하는 시험과 구분합니다.

TLS의 인증서 준비 비용을 줄이거나 sleep을 넣어 호출 순서를 바꾸는 방법은 이 원인의 검증이나 수정으로 사용하지 않습니다. 반복 실행의 우연한 균등성보다 선택 사건별 소유와 순서가 우선 검증 대상입니다.

## BLOCKERS

1. **공정성 범위의 스펙 빈칸**: `core/doc/spec/core/systems/03-io-thread.ko.md:147`–`:153`, 영문 `core/doc/spec/core/systems/03-io-thread.en.md:151`–`:158`은 connected/accepted session을 round-robin으로 배치하고 할당 단위를 connection으로 정합니다. 그러나 ctx 전체 합산인지, connect/accept별 부분집합인지, socket별인지와 같은 공정성 범위는 명시하지 않습니다. 기존 시험 `core/tests/unittest/unittest_connect_io_thread_choice.cpp:136`은 connect 부분집합의 엄격한 균등성을 요구합니다. 감독자가 이 차이를 계약으로 판정하기 전에는 새 분산 정책이나 시험 기대값 변경을 제안할 근거가 부족합니다. 스펙은 수정하지 않았습니다.
2. **실행 trace 부재**: 제공된 각 TCP 분포를 만든 정확한 C/A 순서와 TLS의 50회 균등성 원인은 코드만으로 확정할 수 없습니다. 추가 transport 소비자와 편중 가능성은 코드로 확정하였으며, 실제 회차의 시간 관계만 미확정입니다. 빌드·시험은 읽기 전용 진단 범위에 따라 수행하지 않았습니다.

점검 결과: 별도 TCP 전용 추가 선택은 발견하지 않았습니다. 공유 accept 소비자와 connect-only 집계의 차이를 확인하였습니다. 코드 수정은 0건이며, 공정성 계약의 빈칸과 실행 순서 검증을 감독자 판단 사항으로 보고합니다.
