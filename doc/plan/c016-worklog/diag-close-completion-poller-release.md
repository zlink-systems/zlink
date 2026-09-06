# 진단: `test_close_completion_poller_release` 간헐 실패 (gate s2-s9 finding)

- 담당: 진단 job (커밋 없음, main 편집 없음). 상한 1.5 h 내 종료.
- 대상: `core/tests/integration/test_close_completion_poller_release.cpp`
- 비교 트리: **main**(S-2 non-recursive mutex + S-9 패치 적용, uncommitted) / **ab-old**(`~/project/zlink-work/ab-old`, `6f64e76b51`, S-2/S-4/S-9/S-10 **없음**)
- 모든 측정은 `flock <PERF_LOCK>` 하에 포그라운드 실행.

## 1. "Expected 1 Was 0"이 세는 것

테스트 69행:

```
TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, &event, 1, 1000, &error));
```

모니터 완료 **개수**가 아니라 **`zlink_poller_wait()`가 1000 ms 안에 반환한 이벤트 개수**다.
즉 `Expected 1 Was 0` = "close가 수락되어 등록이 pin한 소켓에 대해 나와야 할 1회성 `ZLINK_POLLERR`가
1000 ms 안에 poller에게 전달되지 않았다". 모니터 유무는 부수 조건이고(양쪽 변형 모두 실패 관측),
`with_monitor` 쪽이 더 자주 걸리는 것은 앞 구간이 조금 더 길어 타이밍 창이 넓어지기 때문으로 보인다.

CTest 10 s 타임아웃은 같은 원인의 누적이다: 각 변형은 20 iteration을 돌고, 걸린 iteration마다
정확히 +1 s가 붙는다(아래 2절 참고). 여러 iteration이 걸리면 테스트 총 시간이 0.03 s → 3~7 s로,
동시 실행이 심하면 10 s CTest 한도를 넘긴다.

## 2. 재현 결과

### 2.1 suite 10회 (`ctest -R 'wake|poll|stream|pipe|mailbox' -j2`, 34 tests)

| 트리 | 실패 run | 정상 아닌 소요(초) |
|---|---:|---|
| main (S-2+S-9) | **0 / 10** | 3.06, 5.05, 7.06 (정상 0.02~0.03) |
| ab-old (pre-campaign) | **1 / 10** | 4.04 — 실패 1건은 `..._without_monitor` 69행 `Expected 1 Was 0` |

### 2.2 스트레스 변형 (단일 테스트 50회 루프 + 동시에 `ctest -j4 -R 'stream|pipe'` 반복)

| 트리 | 실패 | 500 ms 초과 iteration | 관측 소요값 |
|---|---:|---:|---|
| main (S-2+S-9) | **2 / 50** | 18 | 1024·1031·2029·3037·4037·5518·6049·7043 ms |
| ab-old | **4 / 50** | 14 | 1018·1030·2034·3033·4043·5050·7040 ms |

→ **ab-old에서 더 자주 실패한다. S-2/S-9 유래가 아니다 — pre-existing.**
(S-10 job이 `-j4`에서 한 번 본 것과 같은 현상이다.)

소요 시간이 모두 **정확히 N × 1000 ms + α** 라는 점이 핵심 단서다. close가 느린 것이라면
지연이 임의 값이어야 한다. 1000 ms 배수는 곧 "poller가 자기 timeout을 **끝까지** 자고,
timeout 만료 뒤의 재확인에서 비로소 POLLERR를 본다"는 뜻이다(창을 아주 살짝 넘기면 0 반환 = 실패).

## 3. 근본 원인

### 3.1 정지 순간의 스레드 상태 (직접 증거)

gdb가 없어 `/proc/<pid>/task/*/{comm,stat,wchan}` 를 정지 중에 3회 샘플링했다. 3회 모두 동일:

```
tid  test_close_comp   state=S  wchan=do_sys_poll      <- 메인(=poller_wait)
tid  ZLINKbg/Reaper    state=S  wchan=futex_wait_queue
tid  ZLINKbg/core-ct   state=S  wchan=futex_wait_queue
tid  ZLINKbg/IO/0..3   state=S  wchan=futex_wait_queue
```

**closer 스레드(`std::thread`)가 이미 존재하지 않는다.** 즉 `zlink_close()`는 이미 성공적으로
끝났고 closing 비트도 오래 전에 보였는데, poller만 `poll()` 안에서 1000 ms를 마저 잔다.
→ close가 늦은 것이 아니라 **wake edge가 유실된 것**(lost wake-up)이다.

### 3.2 경로

- 종료 상태 게시: `core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp:277`
  `begin_close_or_fail_busy()` 가 `public_api_closing_bit` 를 세운다.
- 바로 뒤 wake edge 게시: `core/src/runtime/sockets/common/socket_base_control.cpp:30-41`
  `begin_close_handoff()` → `notify_submit_progress()` + `mailbox_t::signal()`.
  `signal()`(`core/src/runtime/core/mailbox.cpp:91-107`)은 등록된 poller signaler와
  primary `_signaler` 양쪽에 edge를 보낸다. 여기까지는 정상.
- poller가 종료 상태를 읽는 유일한 지점: `core/src/runtime/sockets/common/socket_base_api.cpp:890-899`
  `get_events_for_poller()` 진입부의 `socket_public_api_scope_t admission` — 실패하면 `ZLINK_POLLERR`.
  이 샘플은 **level이 아니라 edge에 의존**한다: poller는 `socket_poller.cpp:833`의 `poll()`에서
  깨어나야만 다시 이 샘플을 돌린다.
- 유실 지점(잔여 후보, 창이 매우 좁아 계측 없이는 한 줄로 못 박음):
  같은 primary signaler fd의 edge를 **poller가 아닌 쪽이 소비하고 재무장(rearm)하지 않는 경로**.
  - `core/src/runtime/core/mailbox.cpp:151-158` — `recv(cmd, timeout_=0)` 의
    "Avoid poll syscall on non-blocking checks" 분기가 `_signaler.recv_failable()` 로
    primary edge를 소비한다. 소켓의 async command owner(ZLINKbg 스레드)가 이 경로를 탄다.
  - `core/src/runtime/sockets/common/socket_base_api.cpp:911` `drain_primary_signaler()`.
  - 대칭 재무장은 이미 여러 drain 경로에 존재한다
    (`socket_base_lifecycle.cpp:1240, 1497, 1511, 1526` 의 `rearm_primary_signaler()`),
    **close edge 경로에는 그 짝이 없다.** 계약상 POLLERR는 level(“1회성이지만 소멸하지 않는”)
    상태인데 통지만 edge라서, edge 하나를 제3자가 먹으면 다음 자발적 wake까지 잠든다.

### 3.3 분류

**B — Core 기존 결함(pre-existing).** 계약 변경(D) 아님: 
"close가 수락되면 등록은 1회성 POLLERR를 받는다"는 규칙 자체는 그대로이고,
그 규칙이 부하 아래 최대 timeout만큼 늦어질 뿐이다. 테스트 결함(test defect)도 아니다 —
1000 ms 예산은 충분히 관대하고, 기대값은 계약 그대로다.

## 4. 제안하는 최소 수정

우선순위 순:

1. **(권장) close edge를 재무장 대상으로 만든다.** `begin_close_handoff()`
   (`socket_base_control.cpp:41`)에서 `signal()` 과 함께
   `static_cast<mailbox_t *> (_mailbox)->rearm_primary_signaler ()` 를 호출하고,
   primary edge를 소비하는 drain 경로(`mailbox.cpp:151-158` / `socket_base_api.cpp:911`)가
   **closing 상태에서는 소비 후 즉시 재무장**하도록 한다. 새 플래그·옵션 없이 기존
   `rearm_primary_signaler()` 규칙을 close 경로로 확장하는 것이라 POSDDD상 제어점이 늘지 않는다.
2. (대안, 비권장) `socket_poller_t::wait()` 에 상한 있는 재확인 주기를 넣는다 —
   폴링 규칙을 새로 추가하는 방향이라 1안보다 나쁘다.

두 안 모두 `core/include/**` · `libzlink.vers` 를 건드리지 않는다.

## 5. 결론 / 조치

- gate s2-s9의 이 finding은 **S-2/S-9 채택을 막는 사유가 아니다**(ab-old에서 더 자주 재현).
- 별도의 Core 결함 job으로 분리 권장. 그 job은 3.2의 유실 지점을 계측(예: poller notification이
  등록된 동안의 `_signaler.recv_failable()` 소비 카운터)으로 한 줄까지 확정한 뒤 4-1안을 적용하고,
  검증은 본 문서 2.2의 스트레스 변형(50회 + 동시 `-j4`)으로 0/50 을 확인하면 된다.
