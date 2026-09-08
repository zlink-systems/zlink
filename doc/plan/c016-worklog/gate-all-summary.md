# ALL gate summary (중단 상태)

## 적용

- `main..wip/0.17.3-all2`를 scratch `all2.patch`로 만든 뒤 `git apply --3way --index`로 적용했다. 충돌은 없었다.
- 적용 전 `core`, `bindings`, `scripts`에는 다른 변경이 없었다. 누적 patch는 staged 상태로 유지했다.

## Linux dev 검증

| 항목 | 결과 |
|---|---|
| dev build | `JOBS=4 scripts/build-core.sh dev` 성공. 시작 전 ninja 0, available 10,642 MiB |
| 전체 ctest | 211개 중 210 통과, 실시간 337.78 s. `hotpath_gate`만 dev instruction ratio 1.1075~1.3533으로 실패(지시상 release 측정으로 분리) |
| 변경 suite | 정규식 121개 × 3회 모두 통과 (1차 150.57 s, 2·3차 275.77 s) |
| ALL-1 신규 `test_stream_concurrent_pull_send` | until-fail:10, 10/10 통과 (6.74 s) |
| `unittest_phase3_request_reply_owners` | until-fail:20, 20/20 통과 (13.12 s) |
| 공개 인터페이스 | `core/include`, `core/src/libzlink.vers` staged diff 0; C/C++/Go/Rust 3개 header mirror 12/12 일치 |

## TSan

- GCC, `-fsanitize=thread -fno-omit-frame-pointer`, LTO off, `setarch x86_64 -R`, `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`; suppression 없음.
- build 성공. GCC 13은 `atomic_thread_fence`에 `-Wtsan` 컴파일 경고를 출력한다. runtime TSan race/deadlock report는 0개였다.
- 전체 ctest: 209/211 통과, 578.88 s. `hotpath_gate`는 TSan instruction overhead(41.69~79.40× reference)로 실패했다. `test_stream_packet_progress`의 `test_shutdown_during_drain`은 “transport did not queue all fragments”로 실패했고, 동일 TSan target `until-fail:3`은 첫 회에 같은 assertion으로 재현되어 고정 후보로 분류했다.

## Windows 및 후속 단계 차단

`D:\project\zlink`에서 `git fetch` 후 지정 detach checkout을 시도했으나 다음 기존 변경 때문에 거부됐다.

- `core/src/runtime/core/ctx_termination.cpp`
- `core/src/runtime/utils/random.cpp`

변경 보호 및 stash 금지 규칙상 이를 정리하거나 강제 checkout하지 않았다. 따라서 Windows Release build/ctest, Linux release hotpath, with_stream, perf/c, WS/WSS 비율 gate 및 ASan은 실행하지 않았다. 지시한 순서(Windows 뒤 Linux 측정)를 지키기 위해 측정을 시작하지 않았다.
