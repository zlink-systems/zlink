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
