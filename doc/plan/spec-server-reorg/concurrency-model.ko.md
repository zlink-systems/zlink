# 동기화 모델이 flake를 만드는 구조인가 — 실측

작성: 2026-08-26. 질문은 "샘플 flake 잡는 데 시간이 너무 많이 든다. lock이 흩어져 있어서
아닌가"였다. 세어 본 결과 그 방향이 맞고, 더 구체적으로 말할 수 있다.

## 1. 실측

| 계층 | mutex 선언 | lock 취득 지점 |
|---|---:|---:|
| `core/src` (ZeroMQ 계열 transport) | 31 | 262 |
| `framework/languages/cpp/framework/src` (우리 계층) | 133 | **1303** |

**이미 직렬화된 transport 위에 얹은 계층이 그 아래보다 lock 밀도가 5배다.**

lock 취득 파일 상위: `spot_runtime.cpp` 186, `public_host_runtime.cpp` 104,
`mesh_node_runtime.cpp` 83, `actor_gateway_runtime.cpp` 81, `stream_host_service.cpp` 71.

## 2. core는 원래 모델을 갖고 있다

`runtime/core/`에 `io_thread`, `mailbox`, `poller`, `command`가 그대로 있다. socket 하나를
io_thread 하나가 소유하고, 스레드 간 작업은 공유 상태를 잠그는 대신 **mailbox로 command를
보낸다.** 질문에서 말한 "poller로 순차적으로 퍼올리고 거기서부터 직렬화" 모델이다.

## 3. framework는 두 규율을 겹쳐 놨다

framework에도 `runtime/execution/serial_execution_queue.hpp`가 있다. 즉 직렬 실행 primitive는
**있다.** 문제는 그것이 **handler turn만** 직렬화한다는 것이다. 그 주변의 runtime 장부 —
route, binding, sink, registry, location — 는 별도의 mutex 133개로 지켜진다.

**그래서 handler가 만지는 상태가 handler의 lane 소유가 아니다.** 로직 한 줄을 추가하면 그 줄이
어떤 lock을 잡고, 그 인터리빙은 lane이 정해 주지 않는다. 이게 "한 줄 추가했는데 동작이 바뀐다"의
기계적 원인이다.

## 4. 반복되는 실패 모양은 하나다 — lock→스냅샷→해제→사용

`unique_lock` 73개, 명시적 `.unlock()` 61곳. 전형적인 코드는 이렇다.

```
{ lock; find(map); sink = shared_ptr 복사; }   // 여기서 해제
(*sink)(...)                                   // lock 밖, 다른 executor에서 실행
```

`shared_ptr`이라 객체는 살아 있지만 **그 스냅샷이 가리키는 사실은 이미 낡을 수 있다.** 구조상
TOCTOU다.

오늘 쫓은 실패가 전부 이 계열이었다.

| 증상 | 위치 | 같은 모양인가 |
|---|---|---|
| cpp ZoneWorld `bound_session_push` 유실 | `actor_gateway_runtime.cpp` `admit_bound_session_delivery` | 그렇다 — resolve는 `match=true`인데 lock 밖 write가 `unavailable` |
| java `queuedRelocationIntentCannotRacePastYieldRegistration` | `ZLinkAsyncSerialQueue` | 그렇다 — 큐 등록과 intent의 경합 |
| `fast_mutex.hpp:76` abort (`EINVAL` on unlock) | core `pipe`/`stream` | 이 패턴의 최악 형태 — 스냅샷을 쥔 사이 소유 객체가 파괴됨 |

`EINVAL`은 "비소유 unlock"(`EPERM`)이 아니라 **그 저장소가 더 이상 유효한 mutex가 아니라는
뜻**이므로 수명 문제다.

## 5. 방향

core가 이미 직렬화 지점(io_thread + mailbox)을 주는데 framework가 그것을 두 방식으로 버린다.
(1) detached executor로 일을 넘기고(`submit_blocking_call`), (2) 공유 registry를 유지한다.

제안하는 규율은 하나다. **runtime 장부를 lane이 소유하게 하고, 변경은 그 lane에 command로만
넣는다.** lane 안에서 읽을 때는 lock이 필요 없고, 스냅샷이 낡을 창 자체가 없어진다.

전면 교체가 아니라 registry 단위로 옮길 수 있고, 하나 옮길 때마다 flake 한 부류가 사라진다.
착수 순서는 lock 밀도와 오늘 실패 빈도가 겹치는 곳부터다.

1. `actor_gateway_runtime` 의 `bound_session_sinks` / `actors_by_id` (오늘 실패가 여기)
2. `stream_host_service` 의 session route 장부 (tombstone이 write 실패보다 늦는 문제)
3. `spot_runtime` (lock 취득 186개로 최다)

**착수 전에 정할 것:** 이건 스펙 변경이 아니라 구현 구조 변경이다. 다만 §2.2의 "관측 가능한
disconnect" 같은 조항은 지금 구현이 지키지 못하고 있으므로, 옮기면서 스펙 준수 여부를 같이
확인해야 한다.
