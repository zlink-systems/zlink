# Node routed echo relay C 구조 정합 — 2026-09-08

> 상태: **relay server 구현과 tcp smoke는 완료했다. tls 전수와 Node 전체 단위 테스트는
> 이번 범위 밖의 별도 실패 때문에 완료 판정을 내리지 못했다.**
> 수정 범위는 Node routed SENDSEND relay runner와 그 생성 JavaScript다. Core·Framework·다른
> 언어·Node binding 라이브러리·정책/스펙/계획서는 수정하지 않았고 commit·push도 하지 않았다.

## 1. 실행 조건과 공유 경로

- 고정 Core:
  - `ZLINK_CORE_SOURCE=release`
  - `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.2`
- 모든 perf 실행 전에 `bash scripts/perf/wait-for-idle-perf.sh`를 실행했다. lock을 얻은 뒤
  load가 5를 넘으면 5 이하가 될 때까지 기다렸고, 측정은 한 번에 하나만 실행했다.
- DR과 RR SENDSEND는 relay 구현을 공유한다. 두 server entry가 각각 family만 넘겨
  `perf_multi_routed_sendsend.ts`의 같은 `runRoutedSendSendServer`를 호출한다.

## 2. 변경과 C·Python 대응

기존 server는 수신마다 `sendServerReply` task를 시작해 `pendingTasks`에 넣고 종료 시
`Promise.all`로 기다렸다. 수신과 reply admission 사이의 결합이 없어 socket의 미완료
completion reservation이 계속 증가할 수 있었다.

변경 후 구조는 다음과 같다.

1. 수신 turn에서 routing id와 multipart application payload를 불변 snapshot으로 복사해
   head/tail FIFO에 넣는다.
2. active sender Promise는 하나만 존재한다.
3. sender는 FIFO head를 제출하고 그 admission Promise가 끝난 뒤에만 head를 제거하고 다음
   reply를 제출한다.
4. `NotConnected`와 `NotFound`만 기존 stale-route 의미대로 제거하고, 그 밖의 오류는 server
   loop 또는 종료 drain에서 그대로 전파한다.

이는 C `drain_recv_and_relay`의 receive snapshot과 `flush_pending_replies`의 FIFO head 제출,
backpressure 시 단일 exact-WRITABLE 대기에 대응한다. Node에서는 binding의 public admission
Promise가 exact-WRITABLE retry를 소유한다. Python `RoutedReplySender`의 `deque`와 단일
`_send_pending` task에도 직접 대응한다.

pending FIFO에는 **상한 숫자, 고정 application window, 새 timeout 또는 새 sleep이 없다.**
기존 `ASYNC_PROGRESS_BATCH`는 event-loop scheduler fairness용이며 pending 개수를 제한하거나
다음 reply의 조기 제출을 허용하지 않는다. 수정 전의 “수신마다 독립 submit”과 “종료 때 task
전체 gather” 두 규칙은 수정 후 **“FIFO head admission 완료 후 다음 head submit” 한 규칙**으로
줄었다(2개 → 1개).

## 3. 검증 결과

| 검증 | report/명령 | 결과 |
|---|---|---|
| tcp SENDSEND 2종, 64 B, 1 s, runs 1 | `perf_node_multi_linux_20260908_104124_d_bp24_tcp_final.txt` | **complete**, success 2 / fail 0, result 10/10 |
| tls SENDSEND 2종, 64·256·1024·4096·65536 B, 5 s, runs 5 | `perf_node_multi_linux_20260908_095606_d_bp24_tls.txt` | **partial**, success 9 / fail 1, result 45/50 |
| 최종 FIFO의 RR/tls/65536 B 집중 재검증, 5 s, runs 5 | `perf_node_multi_linux_20260908_103333_d_bp24_rr65536_linked.txt` | **partial**, 개별 run 2/5 성공, 셀 result 0/5 |
| relay 계약 테스트 | `node --test dist-tools/tests/perf_multi_routed_sendsend_contract.test.js` | **통과**, 10/10 |
| Node 전체 테스트 | `bash bindings/node/tests/run_tests.sh` | **실패**, relay 밖 `multipart.test`에서 중단 |

TLS 전수에서는 DR 25/25와 RR 64~4096 B 20/20가 모두 성공했다. RR 65,536 B 실패는 server
제출 오류나 `ENOMEM`이 아니라 client의
`runRoutedSendSendRounds`가 낸 `multi routed send admission drain timed out`이다. 같은 셀을
최종 FIFO로 runs 5 재실행해도 반복됐다. server scheduler 진행 방식을 바꾼 집중 실험에서도
같은 client 오류가 반복되어 해당 가설은 기각하고 실험 변경을 최종 diff에서 제거했다.
client 종료/admission drain 수정이나 timeout 변경은 이번 server 전용 범위를 벗어나므로
우회하지 않았다. 따라서 요구된 tls `status: complete`는 달성하지 못했다.

Node 전체 테스트의 첫 실패는 `tests/multipart.test.ts`의 native send/close stress가
`counts.rejected_einval > 0n`을 만족하지 못한 것이다. 같은 test file 단독 재실행에서도
동일하게 실패했다. relay 대상 계약 테스트는 최종 구현에서 10/10 통과했으며, 범위 밖
assertion이나 fixture는 변경하지 않았다.

## 4. 판정

- D-BP24의 무결합 server task fan-out과 종료 시 `Promise.all`은 공유 DR/RR relay 경로에서
  제거됐다.
- tcp smoke와 relay 계약 테스트는 통과했다.
- tls 전수의 RR 65,536 B client drain 실패와 Node 전체 테스트의 native stress 실패가 남아
  요청된 검증 전체를 `complete`로 판정할 수 없다.
