# core-rf-SD-1 결과 보고서

## 1. 결론

계측 app의 TCP suffix 오판은 수정했고, B1·B2 및 죽은 D-f knob 제거를 요청된 하나의 증거 patch로 만들었다. 기능·동시성 검증은 통과했다. 다만 릴리스 판정용 64 KiB A/B에서 zlink뿐 아니라 asio·cppserver·zmq 대조군까지 함께 크게 하락해 측정 환경의 시간축 변동이 확인됐고, zlink 3-run 중앙값 자체도 처리량과 RSS 기준을 만족하지 못했다. 따라서 0.17.4에는 계측 app 수정과 D-f만 채택하고 B1·B2는 기각하는 것이 안전하다. 제출한 `SD-1.patch`는 네 변경을 합친 재현·검토용 patch라 그대로 착지하면 안 되며, 감독자가 B1·B2 hunk를 제외해 계측 app과 D-f만 분리해야 한다.

- 본 patch: `/home/hep7hep7/project/zlink-work/all-artifacts/SD-1.patch`
  - SHA-256: `d5a0a4a179c25bf84fb8383c274f45b5163dd95b32bc27931544bad27ae61c14`
- D-S1 실험 patch: `/home/hep7hep7/project/zlink-work/all-artifacts/SD-1-DS1-experiment.patch`
  - SHA-256: `82dd92b6abb3c081a80086d34a4dae621e3e3c46db9101bb861a8ad9d0c7cd6b`
  - 본 patch 위에 적용하는 delta이며 `git apply --check`를 통과했다. 본 worktree에는 남기지 않았다.
- 작업 시작 시 기준 commit: `84d25131a6424031149ab7321e0678b65409bd75` (당시 `wip/0.17.4` HEAD; 측정 중 ref 이동과 무관하게 이 commit을 base로 보관)
- base Release library SHA-256: `1ed8fd9c005dd7f1f25be1887df5280b858bb1d306bb9aa4431173decc218c1c`
- `core/include/**`, `core/src/libzlink.vers`, ABI, enum, errno mapping 및 스펙 문서는 변경하지 않았다.

## 2. 변경 내용과 근거

### 2.1 변경 파일:행

| 파일:행 | 변경 |
|---|---|
| `bindings/c/bench/with_stream/stacks/zlink/test_scenario_stream_zlink.cpp:208-290` | exact 1-frame RAW chunk만 zero-copy로 보내고, 그 외 byte는 RID별 `frame_buffer_t`에 전부 append한 뒤 완성 frame만 반복 소비한다. 한 frame 뒤의 정상 partial suffix를 malformed로 버리던 chunk-local parser를 제거했다. |
| `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp:129-144` | RAW STREAM에는 protocol gather header가 없다는 단일 capability 판정만 남기고 죽은 STREAM gather env 접근자 3개를 제거했다. ZMP의 `ZLINK_ASIO_GATHER_WRITE`는 유지했다. |
| `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp:332-399` | decoder와 encoder의 `full_hits`/2-hit 상태를 제거했다. 요청한 크기를 한 번 채우면 현 target의 2배로 성장하고 기존 max로 clamp한다. encoder의 기존 message-boundary short-batch 축소는 유지했다. |
| `core/src/runtime/engine/asio/asio_engine_pipeline.hpp:25-77` | decoder/encoder hit counter와 partial-prefix 상태를 삭제하고 async/speculative가 공유하는 `last_read_bytes` 하나만 남겼다. |
| `core/src/runtime/engine/asio/asio_engine.cpp:804-815,985-1025,1673-1681` | decoder 성장에 동일 full-read evidence를 사용한다. normal completion과 `restart_input()` 모두 `should_speculatively_read_stream()` 한 조건과 기존 64회/1 MiB bounded drain을 거친 뒤 async read를 rearm한다. |
| `core/src/runtime/engine/asio/asio_engine.hpp:225-232` | 위 단일 speculative-read predicate를 선언했다. |
| `core/tests/unittest/unittest_asio_write_turn_policy.cpp:169-198,280-326,351-368` | 1-hit 성장·short read 비성장·encoder 축소와 RAW/ZMP gather capability 경계를 검증한다. |

### 2.2 규칙 전후

규칙 수는 결과 경로를 독립적으로 선택하거나 상태 전이를 결정해 유지 설명이 필요했던 source-level policy clause를 한 개로 세었다. 호출되지 않던 threshold accessor 두 개는 실행 규칙에 넣지 않고 죽은 knob로 별도 집계했다.

| 영역 | 수정 전 | 개수 | 수정 후 | 개수 |
|---|---|---:|---|---:|
| 계측 frame 조립 | exact frame, chunk-local 다중 frame parser, RID buffer의 세 경로. 첫 frame 뒤 partial suffix는 malformed | 3 | exact 1-frame zero-copy 또는 RID buffer 조립의 두 경로. suffix는 다음 chunk까지 보존 | 2 |
| decoder 성장 | hit 누적/초기화와 2-hit 성장 | 2 | 한 번의 full read면 2배, max clamp; short read는 비성장 | 1 |
| encoder 성장 | hit 누적/초기화, 2-hit 성장, short-batch 축소 | 3 | 한 번의 full batch면 2배, max clamp; 기존 short-batch 축소 | 2 |
| speculative read | normal drain과 restart가 진입 조건을 각자 구성 | 2 | full-read evidence를 포함한 predicate 하나를 두 경로가 공유 | 1 |
| STREAM gather | protocol·transport eligibility, ZMP global enable, STREAM 전용 enable | 3 | protocol·transport eligibility와 ZMP global enable | 2 |

수정 전/후 규칙 수: 13개 → 8개다. 여기에 실행 규칙이 아니던 dead threshold accessor 2개도 0개로 줄었으며, 새 option·flag·env·상태는 0개다.

### 2.3 소유 계층·스펙·교차언어·분류

- 소유 계층: read target, speculative read와 bounded drain은 Core Asio I/O thread가 소유한다. 계측 frame 조립은 benchmark app의 RID별 state가 소유한다. 상위 Framework에 보상 상태를 추가하지 않았다.
- 스펙 조항: `08-stream.ko.md` §5의 RAW part ownership/DONTWAIT, §6.3의 bounded queue/HWM·malformed framing과 §1~§9의 공개 동작을 유지했다. §10의 구현 설명 중 405~407행은 D-f 적용 뒤 구현과 어긋나므로 승인된 문장 삭제와 함께 착지해야 한다. `01-zmp.ko.md` §9의 ZMP 내부 engine/encoder 경계를 침범하지 않았고, `11-synchronization-model.ko.md` §1·§3의 I/O-thread ownership과 socket thread-safety도 완화하지 않았다.
- 교차언어 대조: C/C++/.NET/Java binding은 모두 이 native Core engine을 사용하므로 언어별 runtime 분기는 없다. 계측 app 수정은 요청대로 C benchmark zlink stack에만 한정했으며 asio/cppserver가 이미 쓰는 RID별 누적 조립 규칙과 같게 했다.
- 변경 분류: 계측 app=B(기존 결함), B1=B(기존 결함 가설이나 성능 목표 미달), B2=B(기존 결함 가설이나 릴리스 측정 불충분), D-f=A(무효 compatibility 접근 제거), D-S1=D(spec gap 실험, 미착지).

## 3. 검증

유효 판정에 사용한 빌드는 시작 전 `ninja` 0개와 available memory 6,000 MiB 이상을 확인했고 `JOBS=4`를 사용했다. 자원 조건을 어긴 뒤 결과를 폐기한 1회는 §7에 별도로 기록했다.

| 검증 | 결과 |
|---|---|
| dev build, RelWithDebInfo/LTO off | 성공 |
| `ctest --test-dir core/build-dev -E hotpath_gate --output-on-failure` | 211/211 성공, 233.44 s |
| stream/asio/decoder/ws 관련 27-test suite, 3회 | 27/27 × 3 성공 |
| GCC TSan, LTO off, `setarch x86_64 -R`, 같은 27-test suite | 27/27 성공, suppression 없음, TSan 보고 0건 |
| Release+LTO clean build | 성공 |
| 최종 focused dev (`test_stream_socket`, `unittest_asio_write_turn_policy`) | 2/2 성공 |
| `git diff --check` | 성공 |

### 3.1 Release+LTO hotpath 5셀

계획 §4에 따라 Ir는 참고값으로만 사용했다.

| 셀 | Ir reference | 측정 | 비율 | gate |
|---|---:|---:|---:|---|
| dealer_dealer | 3230.922 | 3105.066 | 0.9610 | PASS |
| dealer_router_reqrep | 16455.383 | 15619.807 | 0.9492 | FAIL |
| pair | 2348.457 | 2456.727 | 1.0461 | PASS |
| router_router_tcp | 2972.532 | 3041.537 | 1.0232 | PASS |
| stream | 13969.806 | 14007.265 | 1.0027 | PASS |

dealer-router 단일 재측정도 15627.306, 비율 0.9497로 FAIL이었다. 변경 대상 STREAM 셀은 통과했지만 전체 결과는 4 PASS/1 FAIL이다.

## 4. base/patch A/B

조건은 `bindings/c/bench/with_stream`, tcp, CCU 1000, I/O thread 4, warmup 3 s, duration 5 s, `rcvbuf=sndbuf=1 MiB`, 각 3-run 중앙값이다. 네 stack과 모든 run에서 mismatch는 0이었다. base/patch 묶음의 lock 획득 시점은 각각 `ninja=0`, load1 0.74/0.51이었다. latency 비율은 patch/base라 낮을수록 좋다. 원시 결과는 `all-artifacts/SD-1-base-bench`와 `all-artifacts/SD-1-patch-bench`에 있다.

| stack | size | base kops | patch kops | kops 비 | base p50 µs | patch p50 µs | p50 비 | base p99 µs | patch p99 µs | p99 비 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| zlink | 64 | 296.830 | 281.199 | 0.947 | 1683.440 | 1776.950 | 1.056 | 2481.250 | 2645.810 | 1.066 |
| zlink | 1024 | 266.538 | 254.672 | 0.955 | 1874.800 | 1962.070 | 1.047 | 2756.230 | 2825.160 | 1.025 |
| zlink | 65536 | 32.378 | 19.925 | 0.615 | 15369.070 | 24910.620 | 1.621 | 21550.370 | 459445.480 | 21.320 |
| asio | 64 | 354.640 | 348.594 | 0.983 | 1409.090 | 1433.520 | 1.017 | 1992.660 | 1931.150 | 0.969 |
| asio | 1024 | 326.880 | 312.749 | 0.957 | 1528.690 | 1597.760 | 1.045 | 2058.570 | 2229.040 | 1.083 |
| asio | 65536 | 39.891 | 23.414 | 0.587 | 12479.750 | 21193.420 | 1.698 | 15394.730 | 24549.660 | 1.595 |
| cppserver | 64 | 351.695 | 345.765 | 0.983 | 1421.000 | 1445.270 | 1.017 | 2025.820 | 2109.710 | 1.041 |
| cppserver | 1024 | 331.423 | 315.992 | 0.953 | 1507.610 | 1581.260 | 1.049 | 2085.370 | 2099.540 | 1.007 |
| cppserver | 65536 | 32.230 | 27.378 | 0.849 | 15424.710 | 18126.740 | 1.175 | 19616.660 | 21995.890 | 1.121 |
| zmq | 64 | 308.628 | 310.671 | 1.007 | 1619.180 | 1608.530 | 0.993 | 2521.980 | 2519.960 | 0.999 |
| zmq | 1024 | 306.223 | 290.213 | 0.948 | 1631.740 | 1721.690 | 1.055 | 2533.770 | 2620.340 | 1.034 |
| zmq | 65536 | 20.462 | 16.727 | 0.817 | 24201.810 | 29663.390 | 1.226 | 48977.740 | 113593.480 | 2.319 |

64 KiB 각 run의 처리량은 다음과 같다. 실행 순서는 stack별 묶음이 아니라 runner가 남긴 시간 순서이므로 아래 run 번호만으로 완전한 열 이력은 복원할 수 없지만, 여러 대조 stack의 동반 하락은 확인할 수 있다.

| 구분 | zlink run 1/2/3 | asio run 1/2/3 | cppserver run 1/2/3 | zmq run 1/2/3 |
|---|---|---|---|---|
| base kops | 32.424 / 32.378 / 20.279 | 41.327 / 39.891 / 20.606 | 41.929 / 32.230 / 25.028 | 28.073 / 20.462 / 15.374 |
| patch kops | 41.300 / 19.925 / 17.881 | 38.680 / 23.414 / 21.371 | 32.406 / 27.026 / 27.378 | 16.800 / 13.204 / 16.727 |

따라서 combined patch의 변화만 zlink 코드 인과로 읽을 수는 없다. 그렇더라도 판정 기준인 zlink 3-run 중앙값은 처리량 0.615배, p99 21.320배이고 릴리스 채택 근거가 되지 못한다.

### 4.1 `recvfrom`/message

조건은 S-D와 같은 tcp, 64 KiB, CCU 20, I/O 1, warmup 2 s, duration 5 s, buffer 1 MiB다.

| 구분 | `recvfrom` calls | EAGAIN calls | server messages | calls/msg | 성공 calls/msg | EAGAIN/msg |
|---|---:|---:|---:|---:|---:|---:|
| base | 24,272 | 8,044 | 8,044 | 3.017 | 2.017 | 1.000 |
| patch | 23,408 | 11,644 | 11,644 | 2.010 | 1.010 | 1.000 |
| patch/base | - | - | - | 0.666 | 0.501 | 1.000 |

combined patch에서 성공 read는 약 절반이 됐지만 불필요한 마지막 DONTWAIT는 줄지 않았다. 계측 app+B1+B2+D-f를 분리한 ablation이 없으므로 감소를 B2 단독 효과로 귀속하지 않는다. 코드상 B2가 read target을 바꾸는 유일한 변경이라는 정황은 있지만 개별 채택 증거로는 부족하다. `strace` 계측 중 처리량은 1604.8→2324.8 ops/s였으나 진단 실행이므로 위 3-run A/B를 대체하지 않는다. 원시는 `all-artifacts/SD-1-base-strace`와 `all-artifacts/SD-1-patch-strace`에 있다.

### 4.2 zlink 서버 RSS

| size | base peak RSS KiB | patch peak RSS KiB | 비율 |
|---|---:|---:|---:|
| 64 | 46,464 | 46,456 | 1.000 |
| 1024 | 46,464 | 46,336 | 0.997 |
| 65536 | 279,384 | 423,220 | 1.515 |

64 KiB RSS는 같은 시간축 불안정과 backlog의 영향을 받았다. combined patch 관측값이 51.5% 증가했으나 B1/B2별 ablation이 없으므로 B2에 단독 귀속하지 않고, 합친 변경을 그대로 착지하지 않는 근거로만 반영했다.

### 4.3 WS 64 KiB 1회

`bindings/c/perf/ws_roundtrip_gate.py`가 STREAM-only report를 판정하지 않아 도구가 요구하는 DD+DR, tcp/ws/wss, 1024/65536 전체 12셀을 base/patch 각각 1회 실행했다. `Q(size)`는 `(DR transport/tcp)/(DD transport/tcp)`이고, `Q64/Q1`은 `Q(65536)/Q(1024)`다.

| 구분 | 패턴 | tcp 64 KiB kops | ws 64 KiB kops | wss 64 KiB kops | ws Q64/Q1 | wss Q64/Q1 | gate |
|---|---|---:|---:|---:|---:|---:|---|
| base | DD | 70.841 | 53.360 | 27.855 | 0.430835 | 1.781459 | FAIL |
| base | DR | 31.534 | 5.541 | 4.181 | 0.430835 | 1.781459 | FAIL |
| patch | DD | 73.234 | 51.369 | 27.372 | 0.919083 | 0.799415 | FAIL |
| patch | DR | 37.497 | 18.715 | 5.563 | 0.919083 | 0.799415 | FAIL |

본 변경은 RAW STREAM 경로이고 이 표는 1회 측정이라 WS 변화의 인과를 주장하지 않는다. 원시는 `all-artifacts/SD-1-base-ws-gate`와 `all-artifacts/SD-1-patch-ws-gate`에 있다.

## 5. D-f 죽은 knob 확인

제거 전 production connection policy는 STREAM 전용 env accessor를 호출했지만, RAW STREAM 생성 시 `protocol_builds_gather_header=false`가 먼저 결과를 false로 만들었다. 따라서 transport가 gather를 지원하고 env를 바꿔도 wire 동작에는 영향이 없었다. 제거 후 세 문자열은 구현과 test에서 0건이며, ZMP 대상인 `ZLINK_ASIO_GATHER_WRITE`와 protocol/transport capability 판정은 남아 있다.

스펙 `08-stream.ko.md` 런타임 기본값의 승인 후 삭제 문장 초안:

> `ZLINK_ASIO_STREAM_DISABLE_GATHER`, `ZLINK_ASIO_STREAM_GATHER_THRESHOLD`, `ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD`는 호환성 때문에 읽지만 현재 raw STREAM 동작에는 영향을 주지 않는다.

삭제 위치는 `core/doc/spec/core/socket/08-stream.ko.md:405-407`이다. 위 문장만 삭제하고 ZMP의 `ZLINK_ASIO_GATHER_WRITE` 설명은 유지한다. 이번 patch에서는 스펙을 수정하지 않았으므로 D-f는 승인된 스펙 변경과 함께 착지해야 한다.

## 6. D-S1 실험

실험 patch는 `rcvbuf=-1`인 STREAM decoder max만 `getsockopt(SO_RCVBUF)`의 OS 기본값까지 올리고 기존 `maxmsgsize` clamp를 적용한다. 새 option/flag/env는 없다. 본 patch에는 포함하지 않았다.

조건은 MULTI_STREAM/tcp, `rcvbuf=-1`, CCU 1000, duration 5 s, 3-run 중앙값이다. 이 runner는 p50을 출력하지 않아 mean, p95와 p99를 기록했다. 아래 latency 비율은 실험/main이다.

| size | main kops | 실험 kops | 비율 | main mean ms | 실험 mean ms | 비율 | main p95 ms | 실험 p95 ms | 비율 | main p99 ms | 실험 p99 ms | 비율 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 291.794 | 310.217 | 1.063 | 1.712 | 1.611 | 0.941 | 2.386 | 2.200 | 0.922 | 2.771 | 2.555 | 0.922 |
| 1024 | 232.600 | 271.443 | 1.167 | 2.148 | 1.840 | 0.857 | 3.021 | 2.478 | 0.820 | 3.479 | 2.877 | 0.827 |
| 65536 | 21.535 | 24.877 | 1.155 | 23.099 | 20.008 | 0.866 | 26.296 | 22.748 | 0.865 | 32.329 | 27.268 | 0.843 |

64 KiB, CCU1000, duration 30 s의 서버 peak RSS는 main 170,796 KiB, 실험 169,692 KiB(0.994배)였다. peak RSS를 연결 수로 단순히 나누면 170.8/169.7 KiB지만 고정 process 비용을 포함하므로 증분 메모리로 해석하지 않는다. 같은 실행 처리량은 23.546→26.593 kops였다. 원시는 `all-artifacts/SD-1-main-rcvbuf-default-bench`, `SD-1-DS1-bench`, `SD-1-main-rss1000`, `SD-1-DS1-rss1000-long`에 있다.

연결 4,000개 조건은 main과 실험 모두 기존 10 s `CLIENT_READY` 경계에서 `non_zero_exit_2_CLIENT_READY,65536`으로 실패했다. 금지된 timeout 증가로 우회하지 않았으며, 준비 완료 전 RSS snapshot은 유효한 측정에서 제외했다. 처리량 이득은 보였지만 연결 4,000개의 메모리 판정이 없으므로 D-S1은 별도 실험 patch로만 보관하고 착지하지 않는다.

## 7. 미실행·제외 항목

- D-S1 연결 4,000개 RSS: 양쪽 모두 기존 readiness timeout에서 실패했다. timeout/budget 증가는 금지 규칙이므로 재시도 조건을 완화하지 않았다.
- base WS 준비 중 `--reuse-build`가 적용되지 않은 1회 실행은 runner가 JOBS=16으로 자동으로 빌드해 자원 규칙을 벗어났다. 결과 전체를 폐기했고, 이후 clean Release+LTO를 JOBS=4로 다시 빌드한 뒤 유효 gate만 보고했다.
- cppserver는 이 worktree에 submodule checkout이 없어 canonical worktree의 기존 binary를 읽어 이 worktree의 무시 대상 build directory로 복사했다. 정확한 upstream source commit은 산출물에 기록되지 않았으며, binary SHA-256은 `4173dc9af6e5a4150f90659750e6bc3a630581629d0cc5c1397bd210992e2ba4`다. base와 patch에 같은 binary를 사용했고 다른 worktree의 source는 변경하지 않았다.
- 추가 6-stack 측정은 하지 않았다. 요청된 최소 4-stack(zlink/asio/cppserver/zmq)을 모두 3-run 측정했다.
- 사용자 소유 untracked `SUPERVISOR-NOTE.md`는 변경하거나 patch에 포함하지 않았다.
- 진행 파일은 단계 완료 때마다 갱신했지만 일부 간격이 13~16분이어서 요청된 10분 주기를 충족하지 못했다.

## 8. 독립 2축 검토 반영

- 코드 부합 축: patch와 원시 결과를 다시 열어 D-S1의 p50 오표기를 p95로 바로잡고 mean을 추가했다. dead threshold accessor와 실제 gather 판정을 분리해 규칙 수를 다시 셌고, combined patch 측정의 B2 단독 귀속을 제거했다. 작업 시작 뒤 움직인 base ref, 유효 build의 JOBS 범위와 진행 갱신 지연도 명시했다.
- 원칙 준수 축: 기각 항목이 든 combined patch를 그대로 착지할 수 없다는 점, D-f와 스펙 405~407행의 동시 반영 조건, 원시 산출물 경로와 64 KiB per-run 값, WS의 Q 정의를 추가했다. cppserver source commit을 특정하라는 제안은 기존 binary에 그 provenance가 없어 추정하지 않고 미기록 사실과 binary hash를 보고하는 방식으로 반영했다.

채택 권고: B1 기각 / B2 기각 / D-f 채택
