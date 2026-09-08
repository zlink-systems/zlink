# ALL gate progress

- 2026-09-08 KST: `main..wip/0.17.3-all2` patch를 `git apply --3way --index`로 충돌 없이 적용했다. `core`, `bindings`, `scripts`에 적용 전 다른 변경은 없었다.
- 2026-09-08 KST: Linux dev build를 `JOBS=4`로 시작했다. 시작 전 `ninja=0`, available 10,642 MiB를 확인했고, 3분 경과 시점에도 빌드가 진행 중이다.
- 2026-09-08 KST: 6분 경과 시점에도 dev build는 진행 중이며 compiler 프로세스가 동작한다.
- 2026-09-08 KST: dev build 프로세스가 종료했고 필요한 test binary와 CTest 설정이 생성됐다. 전체 ctest를 시작한다.
- 2026-09-08 KST: 전체 ctest는 78/211까지 진행했다. `hotpath_gate`만 dev tree에서 instruction ratio +10.75~+35.33%로 실패했으며, 지시된 대로 정상 전체-test 판정에서 제외하고 release 단계에서 별도 확인한다.
- 2026-09-08 KST: 전체 ctest는 211개 중 210개 통과, `hotpath_gate` 1개만 dev instruction 측정 특성으로 실패했다(실시간 337.78 s). 변경 suite 3회와 until-fail 검증을 진행한다.
- 2026-09-08 KST: 변경 suite 1차는 53/121까지 모두 통과했다.
- 2026-09-08 KST: 변경 suite 1차는 121/121 통과했다. 2·3차(`--repeat until-fail:2`)는 39/121까지 각 반복이 통과했다.
- 2026-09-08 KST: 변경 suite 121개 × 3회, `test_stream_concurrent_pull_send` 10회, `unittest_phase3_request_reply_owners` 20회가 모두 통과했다. ABI diff 0 및 12개 mirror 일치를 확인했다. TSan build는 다른 ninja 종료를 대기한 뒤 시작했다.
- 2026-09-08 KST: TSan build가 진행 중이다. GCC 13은 `atomic_thread_fence`에 대해 `-Wtsan` 컴파일 경고를 내지만, runtime sanitizer suppression은 사용하지 않는다.
- 2026-09-08 KST: TSan build는 성공했다. `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`, suppression 없이 `setarch x86_64 -R` 전체 ctest를 실행 중이며 34/211까지 통과했다.
- 2026-09-08 KST: TSan 전체 ctest 65/211 시점에 `test_stream_packet_progress`의 `test_shutdown_during_drain`이 fragment queue assertion에서 1회 실패했다. 전체 결과 뒤 동일 target 반복으로 간헐/고정을 분류한다.
- 2026-09-08 KST: TSan 전체는 209개 통과, `hotpath_gate`(계측 instruction overhead)와 `test_stream_packet_progress`(같은 `test_shutdown_during_drain` assertion이 단독 재실행에서도 즉시 실패) 2개 실패로 끝났다. runtime TSan race/deadlock report는 없었다.
- 2026-09-08 KST: Windows `D:\project\zlink`에 사용자 변경 `core/src/runtime/core/ctx_termination.cpp`, `core/src/runtime/utils/random.cpp`가 있어 지정된 detach checkout이 거부됐다. stash/정리/강제 checkout 권한이 없으므로 Windows 및 그 뒤 Linux release/측정을 시작하지 않았다.
- 다음: Windows 변경 보존/정리 방법에 대한 감독관 지시 후 Windows 전체 build·ctest, 이어 Linux release·측정.
