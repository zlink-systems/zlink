# gate-g10 요약

## 포팅·빌드·테스트

- main/origin/main은 `0af640b0a82f850b9fafe8102890174dc99e3ce9`로 일치했다. 기존 worklog 변경 때문에 `git pull --rebase`는 unstaged 변경으로 실행 불가였으며, stash는 사용하지 않았다.
- G-10 worktree patch(`core/src/runtime/sockets/common/socket_send_submit.cpp`, 7+/5-)를 3-way clean 적용했다. 충돌은 없었다.
- `JOBS=4 scripts/build-core.sh dev`: 성공. 전체 `ctest --test-dir core/build-dev -j2 --output-on-failure`: 208개, 실패 목록 없음.
- 지정 패턴 58개를 `--repeat until-fail:5`로 완료했고, `test_close_completion_poller_release`는 20/20 통과했다. 알려진 간헐(`test_single_lane_flow_snapshot_accounting`, `test_stream_socket_recv_multiclient_ready_regression`, `unittest_request_timeout_scheduler`)은 관측되지 않았다.
- `JOBS=4 scripts/build-core.sh release --lib-only`: 성공. 모든 빌드 전 `pgrep -c -x ninja`는 0이었고, hotpath valgrind 직전 `pgrep -x ninja`도 비어 있었다.

## 공개 표면·hotpath

- `git diff --stat -- core/include core/src/libzlink.vers`는 포팅 전후 모두 비어 있고 `git diff --check`도 통과했다. `scripts/gate/README.md`에는 8-header 절차가 없어 fallback으로 발견한 3개 헤더(`zlink.h`, `zlink_enum.h`, `zlink_errno.h`)를 C/C++/Go/Rust mirror와 비교해 12/12 일치했다.
- 첫 hotpath 실행의 도구 출력이 보존되지 않아, 허용된 두 번째 단일 재측정으로 표를 확보했다(load avg 1.66/3.38/4.10; `flock` 적용).

| 셀 | reference | 측정 | ratio | 판정 |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3230.922 | 0.9437 | FAIL-by-improvement (−5.63%) |
| dealer_router_reqrep_inproc | 18663.506 | 18596.405 | 0.9964 | PASS |
| pair_inproc | 2348.457 | 2288.378 | 0.9744 | PASS |
| router_router_tcp | 2972.532 | 2873.901 | 0.9668 | PASS |
| stream_tcp | 14623.471 | 14139.131 | 0.9669 | PASS |

reference는 수정하지 않았다. FAIL은 개선 방향의 ±5% gate 규칙에 따른 보고 전용 항목이다.

## 성능 (모두 local Release runtime, `flock` 적용)

with_stream은 `ZLINK_CORE_SOURCE=local --stack zlink,asio --size all --ccu 1000 --runs 1 --reuse-build`로 실행했다(load avg 1.56/3.25/4.04). 아래 비율은 §7.1 Phase 0 절대 기준 대비다.

| 크기 | zlink kops/s (기준 대비) | asio kops/s (기준 대비) | zlink/asio |
|---|---:|---:|---:|
| 64 B | 278.909 (1.037) | 369.297 (1.147) | 0.755 |
| 1024 B | 270.749 (1.114) | 337.414 (1.066) | 0.803 |
| 65536 B | 34.214 (1.126) | 32.640 (0.833) | 1.048 |

perf/c 1024 B tcp 경량 3셀: single RR은 439.457 Kmsg/s (Phase 0 744.4 대비 0.590, load 3.51/3.22/3.91), multi RR_SENDSEND은 141.665 Kops/s (111.5 대비 1.270), multi RR_REQREP은 111.880 Kops/s (73.0 대비 1.533; multi load 3.81/3.29/3.93)였다. 세 runner 모두 `PERF_NO_AUTOBUILD=1`과 local `core/build/lib/libzlink.so.0.17.1`을 확인했다.

메인 working tree에는 포팅된 소스 변경과 worklog만 남겼다. 커밋·stash·스펙 문서 변경은 하지 않았다.
