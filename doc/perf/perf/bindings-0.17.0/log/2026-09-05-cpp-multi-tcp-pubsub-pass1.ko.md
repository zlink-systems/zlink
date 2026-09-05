# C++ Multi `tcp` `MULTI_PUBSUB` 자체 hot-path pass 1 — no-go, 3-run 재짝지음

before(`p1cpp`, 1 run) aggregate 93.3%(목표 95%)로 `미달`이던 `MULTI_PUBSUB` `tcp`의 자체 개선 pass(계획서 §7.4-9~11).
codex sol high job(브리프 `doc/plan/c016-worklog/briefs/cpp-perf-pubsub-pass1.b.prompt`, 요약
`doc/plan/c016-worklog/cpp-perf-pubsub-pass1-summary.md`, 05:40~05:52 KST, worktree detached @ `b774608b60`).

## 비용 위치 (job 결과)

- callgrind(10 clients, 1초, 64B·4096B, C vs C++ publisher·subscriber)와 C/C++ 교차 실행(3초):
  publisher는 C와 동률(C++ pub + C sub 99.7%/101.0%), **subscriber 쪽 공개 wrapper 비용이 64B·4096B 각각 약 5%**(C pub + C++ sub 94.7%/94.4%).
- subscriber 잔여 비용은 `subscribe_part()`가 계약대로 topic·source RID·`message_t` ownership을 호출자에게 넘기는 비용(약 215 Ir/call, topic SSO 복사 약 75 Ir).
  메시지당 binding allocation·`std::function`·lock·payload 복사는 관측되지 않았다(동기 PUBSUB 수신은 completion owner·scheduler를 타지 않음).
- 후보 표는 job 요약 §"§2.1 / §2.4 판정과 변경": 즉시 publish state 제거·output wrapper pool(기각 목록)·topic 생략·empty-output 판정 조회 제거 모두 계약 변경이거나 5% 미만 → **no-go, 코드 변경 없음**.
- job의 after 1 run(같은 코드)은 C 대비 120.7%로 before 93.3%와 27%p 차이 — PUBSUB(lossy) 셀의 run-to-run 편차가 매우 크다는 뜻이며 library 효과로 귀속하지 않는다.

## 3-run 재짝지음 (판정용)

C 직후 C++, 100 clients, 5초, `--runs 3` 평균, tcp, Core 0.17.0 local(`core/build`, `core_revision 3480ee5d78`, dirty 0), 05:53~05:57 KST,
C 시작 load 0.18. tag `p1cpp-pubsub-r3`:
C `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_055350_p1cpp-pubsub-r3.txt`,
C++ `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_055517_p1cpp-pubsub-r3.txt`.

| size | C Kmsg/s | C++ Kmsg/s | ratio | C ms | C++ ms | lat ratio |
|---|---|---|---|---|---|---|
| 64 | 821.3 | 659.3 | 80.3% | 1680.5 | 1590.3 | 0.95x |
| 256 | 986.3 | 610.6 | 61.9% | 1738.6 | 1977.7 | 1.14x |
| 1024 | 956.9 | 769.4 | 80.4% | 816.1 | 1050.3 | 1.29x |
| 4096 | 678.4 | 620.2 | 91.4% | 356.2 | 367.2 | 1.03x |
| 65536 | 68.4 | 63.9 | 93.4% | 156.6 | 157.9 | 1.01x |

aggregate throughput(size 비율 평균) **81.5%**, latency 평균 **1.08x** → `미달(81.5%)`(목표 95%).

## 읽는 법·주의

- 세 측정(before 1 run 93.3%, job after 1 run 120.7%, 3-run 81.5%)이 모두 같은 C++ 코드다. lossy PUBSUB은 publisher가 HWM에서
  drop하므로 완료 수신 수가 subscriber의 keep-up과 스케줄링에 민감하다. 3-run 평균을 판정값으로 쓴다.
- 두 러너의 result 시점 Auto-HWM detail이 다르다: C pub 서버 SNDHWM이 64/1024/65536B에서 1,048,576, 256/4096B에서 4,096,000으로
  size마다 번갈아 찍히고 C++는 항상 4,096,000이다(둘 다 auto-hwm balanced, 같은 Core). 스냅숏 시점 문제로 보이며 C++ 쪽 HWM이
  더 크므로 C++ 미달의 원인은 아니다. C 러너 report의 HWM 스냅숏 시점은 별도 확인 항목으로 남긴다.
- callgrind에서 확인된 subscriber wrapper 5%로는 3-run의 18.5%p 부족을 설명하지 못한다. 나머지는 lossy 경로의 동적 특성(구독자 처리
  속도·drop)이며 library 코드에서 계약을 유지한 채 줄일 후보가 없어 pass 2(read-only 리뷰)는 열지 않고 `미달`로 둔다(§7.4-11 no-go 기록).

## pass 2 (Sol read-only 리뷰, 06:10~06:24 KST) — no-go, 판정 `보류(81.5%)`

브리프 `doc/plan/c016-worklog/briefs/cpp-perf-pubsub-pass2.b.prompt`, 요약 `doc/plan/c016-worklog/cpp-perf-pubsub-pass2-summary.md`(파일 수정·빌드·perf 없음, 기존 callgrind에 `callgrind_annotate`만).

- 수신 경로 Core 경계 대조: `zlink_subscribe_part` 2.00/message, `zlink_msg_close` 2.00/message로 C와 동일; C++ 순증은 wrapper의 empty-output preflight `zlink_msg_size` 약 1회/message. completion·`std::function`·lock 없음. poller wrapper 전체 0.13%.
- 공개 contract 유지 후보 4개 모두 5% 미만(preflight 상태화 ≤1%, topic SSO fast path <0.6%, poller bookkeeping 0.13%, rollback 경로 0%) → **no-go**. §7.5에 따라 두 pass 뒤 `보류(81.5%)`로 확정.
- 18.5%p 전부를 binding에 귀속하지 않는다: profile·교차 실행이 뒷받침하는 subscriber wrapper 비용은 5~6%. 나머지는 lossy 경로에서 **runner parity 차이**가 키운 것으로 본다: (1) C multi PUBSUB server는 client START 뒤 size마다 auto-HWM을 재계산·적용하고 C++ server는 bind/connect 전에 1회만(report의 server SNDHWM 1 MiB vs 4 MiB 불일치의 원인), (2) SUB filter C `""` vs C++ `"bench"`, (3) C++ client의 topic 문자열·routing-id size 추가 검사, (4) deadline/100 ms poll 규칙. → **runner parity 수정 트랙(가이드 §5: library 효과와 분리)**: C++ multi PUBSUB server의 HWM 재계산 시점을 C에 맞춘 뒤 별도 report로 재판정. 사용자 확인 뒤 진행.

## 러너 parity 수정(D-B90, `53d599aa00`) 뒤 3-run 재짝지음 (11:03~11:06 KST, `p1cpp-pubsub-r3b`)

C `perf_c_multi_linux_20260905_110309_p1cpp-pubsub-r3b.txt`, C++ `perf_cpp_multi_linux_20260905_110443_p1cpp-pubsub-r3b.txt`. 100 clients, 5초, 3-run 평균, Core `0c39ed2e52` Release lib(10:48 재빌드), C++ library 코드 변경 없음.

| size | C Kmsg/s | C++ Kmsg/s | ratio | C ms | C++ ms | lat |
|---|---|---|---|---|---|---|
| 64 | 783.2 | 663.3 | 84.7% | 1604.6 | 1566.7 | 0.98x |
| 256 | 784.6 | 662.3 | 84.4% | 1906.7 | 1951.4 | 1.02x |
| 1024 | 902.4 | 792.9 | 87.9% | 947.1 | 1051.9 | 1.11x |
| 4096 | 688.2 | 625.9 | 90.9% | 333.7 | 365.0 | 1.09x |
| 65536 | 68.7 | 81.2 | 118.2% | 180.5 | 123.9 | 0.69x |

aggregate throughput **93.2%**(앞선 3-run 81.5%; 러너 효과라 library 개선으로 합산하지 않음), latency 0.98x. 목표 95%에 1.8%p 미달 → `보류(93.2%)`로 판정값 갱신(두 pass 완료). server Auto-HWM detail은 64/256/65536B 일치, 1024/4096B는 C 4 MiB vs C++ 1 MiB로 여전히 번갈아 다름 — 같은 per-size lifecycle(D-B105)에서 C 자체도 size별로 1/4 MiB가 오가므로 auto-HWM balanced 스냅숏의 비결정성으로 보이며 Core auto-HWM 관찰 항목으로 남긴다.
