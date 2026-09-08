# WIN-2 — Windows `test_writable_resubmit_*` 기동 불가 원인 확정 + MAC-3 적용 + hotpath reference 갱신

- 일자: 2026-09-08. worktree `~/project/zlink-work/rel174`, 브랜치 `wip/0.17.4`(push 완료).
- 시작 21:30 / 종료 23:05. main 커밋·tag 없음, 스펙·public 헤더·ABI 변경 없음.
- Windows 검증 clone: `D:\project\zlink-rel174`(`84d25131a6` = MAC-3 적용 전 상태).

## 결론 요약

| 항목 | 판정 |
|---|---|
| `test_writable_resubmit_from_other_thread_while_sequence_open`(+`_send`) 기동 불가 | **Core 결함 아님. Windows 호스트의 Smart App Control(SAC) 차단.** 코드 수정 없음 |
| MERGE-1 전체 실행의 `BAD_COMMAND` 6건 | 같은 SAC 차단이 1회씩 발생한 것으로 이벤트 로그에서 확정 |
| test_wake_invariants / unittest_flow_state_socket / unittest_mutex | host MSVC에서 재현(수정하지 않음). Actions 대조 필요 |
| MAC-3 | 적용·검증·커밋 `bdedf4300d` |
| hotpath `dealer_router_reqrep_inproc` | idle 재측정으로 개선 확인, reference 갱신 커밋 `b27f25dea1` |

## 1. WIN-2 — 원인 확정

### 1.1 대조

| 대조 항목 | zlink-rel174 | zlink-all3 | 차이 |
|---|---|---|---|
| exe 크기 | 79,872 B | 79,872 B | 없음 |
| `dumpbin /DEPENDENTS` | zlink.dll, KERNEL32, MSVCP140, VCRUNTIME140, VCRUNTIME140_1, api-ms-win-crt-{runtime,stdio,environment,string,heap,math,locale} | **동일 12개** | 없음 |
| 테스트 소스 | `core/tests/integration/test_writable_resubmit_from_other_thread_while_sequence_open.cpp` | 동일 | `cmp` 결과 **바이트 동일**(마지막 변경 `29f4d8b45c`, ALL-2b/ALL-3/RR-1/MAC-2/MAC-3 모두 미변경) |
| 실행 | 기동 실패 | PASS (2 Tests 0 Failures) | — |

즉 의존 DLL 부재(0xC0000135)도, 빌드 디렉터리 캐시도, PATH도, 테스트 등록 인자도 아니다. 해당 target만 다시 링크해도 결과가 같았다.

### 1.2 확정 근거 — 이벤트 로그

`cmd.exe`로 직접 실행하면 PowerShell의 모호한 `ApplicationFailedException`(“애플리케이션 구성이 잘못되었습니다”) 대신 원문이 나온다:

```
'...\test_writable_resubmit_from_other_thread_while_sequence_open.exe'
이(가) Device Guard 정책에 의해 차단되었습니다. 자세한 정보는 관리자에게 문의하십시오.
```

`Microsoft-Windows-CodeIntegrity/Operational` 로그:

```
Id=3033  Code Integrity determined that a process (…\cmd.exe) attempted to load
         \Device\HarddiskVolume7\project\zlink-rel174\build-win-x64-test-config\bin\Release\
         test_writable_resubmit_from_other_thread_while_sequence_open.exe
         that did not meet the Enterprise signing level requirements.
Id=3077  … or violated code integrity policy (Policy ID:{0283ac0f-fff1-49ae-ada1-8a933130cad6})
Id=3118  Smart App Control Block Details
```

정책 상태:

| 키 / 클래스 | 값 | 의미 |
|---|---|---|
| `HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy` `VerifiedAndReputablePolicyState` | **1** | Smart App Control **켜짐(적용)** |
| `Win32_DeviceGuard.UsermodeCodeIntegrityPolicyEnforcementStatus` | **2** | 사용자 모드 CI 정책 **강제** |
| `Win32_DeviceGuard.CodeIntegrityPolicyEnforcementStatus` | 2 | 강제 |

즉 **서명되지 않고 평판이 없는 실행 파일을 SAC가 차단**한다. zlink 코드·링크와 무관하며, 최근 400개 이벤트 중 차단 대상은 zlink 테스트뿐 아니라 `cc1.exe`, `objdump.exe`, `ranlib.exe`, `perf_multi.exe` 등 툴체인·벤치 바이너리도 포함한다.

### 1.3 MERGE-1의 `BAD_COMMAND` 6건도 같은 원인

차단 이벤트(3033/3077, 이벤트 2개 = 차단 1회) 집계:

| exe | 차단 횟수(이벤트/2) |
|---|---:|
| test_writable_resubmit_from_other_thread_while_sequence_open | 12 |
| perf_multi | 7 |
| perf_pair | 6 |
| perf_stream_client | 5 |
| cc1 | 4 |
| cpp_perf_pubsub | 4 |
| cpp_perf_pair / raw_pair_probe | 3 |
| **test_zmp_request_reply / test_close_completion_poller_release / test_zmp_ws_wss / test_asio_ws / test_public_inproc_multipart_send / unittest_receive_transaction** | **각 1** |

마지막 줄 6개는 MERGE-1 전체 실행에서 `BAD_COMMAND`(“Process not started … [unknown error]”)로 실패했다가 재실행에서 PASS한 바로 그 6개다. **각 1회만 차단된 뒤 SAC 평판 판정이 끝나 통과**했다. `writable_resubmit`만 12회 연속 차단으로 남았다.

### 1.4 수정

**Core 수정 없음**(코드·public 헤더·ABI·테스트 모두 무변경). 빌드/환경 문제이며 재현·해소 절차는 다음과 같다.

재현 절차
1. Smart App Control이 켜진(평가 아님) Windows 11 호스트.
2. `cmake --build … --config Release`로 서명 없는 테스트 exe 생성.
3. `ctest -C Release -R writable_resubmit` 또는 exe 직접 실행 → 기동 실패.
4. `Get-WinEvent -LogName Microsoft-Windows-CodeIntegrity/Operational`에 3033/3077/3118.

해소(택1)
- **권장: Windows 게이트는 GitHub Actions `Build Windows x64`에서 판정한다.** Actions 러너에는 SAC가 없다(0.17.3 Actions Windows x64 green).
- 호스트에서 계속 검증하려면 설정 → 개인 정보 및 보안 → Windows 보안 → 앱 및 브라우저 컨트롤 → **스마트 앱 컨트롤을 끈다**(끄면 재설치 없이 되돌릴 수 없음).
- 또는 테스트 바이너리에 코드 서명을 붙인다.

**보류 사항:** SAC 차단 때문에 이 두 target은 host MSVC에서 **실행 자체가 되지 않았다**. 소스는 ALL-3 clone과 바이트 동일하고 그쪽에서 통과했으므로 로직 커버리지 공백은 없다고 보지만, `wip/0.17.4`의 zlink.dll로 실제 통과했다는 확인은 Actions 실행으로 대체해야 한다.

## 2. 나머지 3개 (수정하지 않음, 각 1줄)

한산한 창에서 `ctest -C Release -R '^(test_wake_invariants|unittest_flow_state_socket|unittest_mutex)$'`를 2회 돌렸다.

- **test_wake_invariants**: `:1336` large-HWM drain이 2/3회 60 s 만료로 실패(1회는 통과) — host MSVC 한정 여부는 미확정이며, 0.17.3 Actions Windows x64가 green이었으므로 **호스트 환경/부하 의존 가능성이 높다**.
- **unittest_flow_state_socket**: 20 case 통과 후 CTest 10 s aggregate 제한에 걸리는 재현 100% — 실행시간 문제이며 개별 case는 ALL-3에서 5/5 통과, Actions가 green인 점과 정합한다.
- **unittest_mutex**: `:66` Windows `CRITICAL_SECTION`의 재귀 동작으로 owner `try_lock`이 true, 재현 100% — 백엔드 차이이며 spec11:224-228이 다른 backend의 재진입 검출 미지원을 명시한다(assertion 완화 금지 규칙에 따라 무변경).

세 건 모두 `wip/0.17.4`를 Actions Windows x64에 한 번 돌려 host 한정인지 최종 확인하는 것이 남았다.

## 3. MAC-3 적용

- 원본: `~/project/zlink-work/mac1` `77a2f81db6..b89a3a1396`의 `core/tests` 한정 diff(`.github/workflows/core-macos-test.yml` 진단 커밋 제외, cherry-pick된 `550f0e3f6e` 제외).
- `git apply --3way` 충돌 0. `unittest_flow_state_monitor.cpp`는 main의 LIN-1 수정과 결과가 같아 실제 변경 0으로 흡수됐다.

| 파일 | 변경 |
|---|---:|
| core/tests/integration/test_stream_packet_progress.cpp | +27/−4 |
| core/tests/integration/test_wake_invariants.cpp | +56/−5 |
| core/tests/unittest/unittest_asio_transport_writev_lifetime.cpp | +11/−0 |
| core/tests/unittest/unittest_flow_state_monitor.cpp | (main과 동일 결과, 변경 0) |

| 검증 | 결과 |
|---|---|
| `JOBS=4 scripts/build-core.sh dev` | PASS |
| `ctest -R 'wake\|writev_lifetime\|stream_packet_progress\|flow_state_monitor\|xpub_nodrop' --repeat until-fail:5` | **9/9 PASS**, 172.75 s |
| `git diff --stat HEAD -- core/src core/include` | **비어 있음** |
| `git diff --stat origin/main -- core/include core/src/libzlink.vers` | **비어 있음** |

커밋 `bdedf4300d test(core): MAC-3 macOS test_wake_invariants/writev/packet_progress fixes (0.17.4)`, push 완료.

## 4. hotpath reference 갱신

Release+LTO 트리(`core/build`, release-gate)에서 idle(`ninja 0`, load1 확인, `flock PERF_LOCK`)로 해당 셀만 재측정했다.

| 회차 | 조건 | Ir/msg | 기존 reference 대비 |
|---|---|---:|---:|
| MERGE-1 (load1 0.44) | 5셀 gate | 15623.436 | 0.9494 |
| WIN-2 (load1 0.49) | 단일 셀 | **15609.787** | 0.9486 |

두 측정의 편차는 0.09 %로 값이 유지된다. 기준을 **15609.7872**로 갱신했다(`core/tests/perf/hotpath_reference.json`, 다른 4셀 무변경). 갱신 후 idle(load1 0.99)에서 5셀 `hotpath_gate` **PASS**(10.29 s).

커밋 `b27f25dea1 perf(core): hotpath reference dealer_router_reqrep_inproc`, push 완료.

## 5. 브랜치 상태

| 커밋 | 내용 |
|---|---|
| `b27f25dea1` | hotpath reference 갱신 |
| `bdedf4300d` | MAC-3 |
| `84d25131a6` | MAC-1+MAC-2 |
| `c414d95d52` | RR-1 |
| `fd1a055e31` | ALL-3 |
| `45a389ff7c` | ALL-2b |
| `50ae7ffb42` | ALL-2 |

`origin/wip/0.17.4` = `b27f25dea1`. `core/include`·`core/src/libzlink.vers`는 `origin/main` 대비 무변경.

## 6. 남은 항목

1. **`wip/0.17.4`를 GitHub Actions에 한 번 돌려 Windows x64를 판정**한다(host MSVC의 SAC 차단·부하 의존 실패를 걷어낸 유일한 방법). Windows clone은 MAC-3 적용 전(`84d25131a6`)이라 MAC-3의 `test_wake_invariants` 변경이 Windows에 반영되지 않은 상태다.
2. MERGE-1에서 미실행으로 남은 **latency·RSS gate**(`--rss-sidecar` 샘플러 필요)와 **with_stream cppserver/cppserver_pull/zmq 3 스택**(vendored upstream·libzmq 부재).
3. **WS 비율 gate FAIL**(ws Q64/Q1 = 0.534 < 0.80) — 이번 job 범위 밖, ALL-3와 같은 미해결 항목.
