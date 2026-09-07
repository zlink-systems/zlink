# g11b3 gate summary

## 적용과 인터페이스

- `~/project/zlink-work/g11b2`의 G-11b-3 patch를 `git apply --3way`로 충돌 없이 적용했다. 변경은 `core/src/runtime/core/pipe.{cpp,hpp}`, `core/src/runtime/sockets/common/socket_base_monitor.cpp` 3개이며 staged 상태로 남겼다.
- main은 `main...origin/main` (ahead/behind 없음)이었다. `core/doc/spec/**`의 미커밋 변경은 지시대로 검사에서 제외하고 손대지 않았다.
- `core/include/**`, `core/src/libzlink.vers`의 staged diff는 비어 있고 `git diff --cached --check`도 통과했다. core/c/cpp/go/rust의 `zlink.h`, `zlink_enum.h`, `zlink_errno.h` 12개 mirror cmp가 통과했다.

## 빌드와 기능 검증

- `JOBS=4 scripts/build-core.sh release --lib-only`: 성공.
- `JOBS=4 scripts/build-core.sh dev`: 성공. 기존 `unittest_monitor_ready_drain`의 GCC `-Wstringop-overflow` 경고와 TSan의 `atomic_thread_fence` 미지원 컴파일러 경고가 있었다.
- 전체 ctest (`-j2`): 208개 중 207 통과. 유일한 실패 `hotpath_gate`는 모든 셀이 reference보다 6.13~27.33% 빨라 guard가 FAIL로 표기한 것이다(기준 파일 미변경).
- 관련 105개 패턴(`wake|poll|stream|pipe|mailbox|hwm|flow|credit|monitor|send|recv|router|dealer|pair|inproc`) 5회: 매회 105/105 통과(124.67, 124.36, 126.56, 123.18, 123.10 s).
- lost-wake 세트 `until-fail:20` 3/3 통과(616.92 s), 같은 3개 추가 20회 3/3 통과(625.15 s). `test_close_completion_poller_release` 50/50 통과(21.29 s), `stream|pipe` `-j4` 23/23 통과(9.85 s), `monitor|flow_state|auto_hwm|hwm` 10회 26/26 통과(182.56 s).

## TSan

- `core/build-tsan` 기존 설정(`ENABLE_TSAN=OFF`, `-fsanitize=thread`, LTO off)을 재사용했다. 3개 patch 파일을 일시 reverse-apply해 pristine main을 재빌드·개별 실행한 뒤, patch를 재적용해 동일하게 재빌드·개별 실행했다.
- pristine/after 모두 `mailbox_t::activate_if_command_pending(bool)`, `mailbox_t::reschedule_if_needed()`, `receive_once_guarded<...recv_routed...>` race를 검출했다. 정규화한 3개 signature 집합의 `comm -3` 결과는 비어 있어 차이는 0이다. 기존 TSan 경고는 남지만 patch가 추가한 warning은 없다. 로그: scratchpad `g11b3-tsan-base-*`, `g11b3-tsan-after2-*`.

## 측정

- idle 조건은 ninja 없음·load 1.16→0.70→0.42→0.26의 2분으로 충족했다. `hotpath_gate`는 build 뒤 측정 직전 load 4.27에서 실행되어 다음 5셀 모두 PASS였다: dealer/dealer 3270.922 (1.0124), dealer/router 18325.358 (0.9819), pair 2331.538 (0.9928), router/router 2913.132 (0.9800), stream 13929.655 (0.9526).
- with_stream(`--runs 3`, zlink/asio/zmq) 및 C perf 3셀은 남은 게이트 시간 안에 실행하지 못했다. pristine with_stream 비교 디렉터리는 미사용이다.
- 변경 분류: B 기존 결함 후보. runtime 동작 수정은 하지 않았고, 소유 계층/스펙 조항/교차언어 대조는 gate job 범위 밖의 포팅 검증이다.
