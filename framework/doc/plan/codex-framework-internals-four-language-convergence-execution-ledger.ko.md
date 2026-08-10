# Framework internals 4개 runtime 통합 실행 ledger

이 문서는 Framework internals 통합 계획의 실행 증거를 같은 target SHA에 연결한다.
임시 작업 기록이며 공개 spec·internals·guide에서 참조하지 않는다.

## 단계

| 단계 | 상태 | 기준 |
|---|---|---|
| 공통 relocation protocol과 conformance fixture | 완료 | schema·golden·fixture validator와 네 runtime consumer가 같은 의미를 검증했다. |
| C++/.NET/JVM/Node runtime·unit 수렴 | 완료 | IC-01~IC-14의 production 경로와 white-box test를 네 runtime checkpoint로 닫았다. |
| POSDDD refactoring | 진행 중 | public API·wire·lifecycle 의미를 바꾸지 않고 중복 policy, pass-through wrapper, test-only 우회를 제거한다. |
| package·전체 regression | 대기 | 같은 target SHA에서 filter·skip 없이 통과해야 한다. |
| 7종 sample × 5개 공개 표면 | 대기 | 35개 process cell의 업무 self-check, 역할 log, 정상 종료와 cleanup을 확인해야 한다. |
| release | 대기 | `RUNTIME-UNIT-CLEAN`, package provenance와 local/upstream SHA 일치 뒤 진행한다. |
| 359개 E2E | release 뒤 대기 | 구현 완전성을 먼저 검사한 뒤 실제 process를 실행하고 기능 결함만 수정한다. |

## Pushed checkpoint

| SHA | 변경 | 실행 증거 | 남은 조건 |
|---|---|---|---|
| `59caca327f` | Session relocation command 42~45 golden | schema/golden consumer 검증 | 네 runtime restart·ordering review |
| `39122d7f04` | .NET Session relocation barrier | focused relocation, full .NET unit | IC-01~IC-05와 독립 최종 review |
| `dbffdd3164` | JVM Session relocation barrier | focused/core/Kotlin unit | direct Join restart recovery 후속 checkpoint |
| `841bbec4fd` | C++ Session relocation barrier | focused M6B/M6C/wire | 독립 최종 review |
| `0cc661c372`, `046f93c976` | Node Session relocation barrier와 terminal refusal | typecheck/build/focused runtime | ordering 후속 checkpoint와 독립 review |
| `cb3ec737ea` | .NET Completed 뒤 route 수렴 | focused relocation 177/177, relocation 11/11 | 전체 공통 review |
| `211a367404` | serial·observation conformance fixture | fixture validator PASS | 네 runtime direct consumer |
| `eb2a12cbb0` | Node Completed 뒤 route 수렴 | typecheck, M6C 91/91 | 전체 공통 review |
| `785633bbe9` | C++ IC-01~IC-04 | execution, M6B, M6C 3/3 PASS | IC-05, IC-07~IC-14, full gate |
| `683b3d43f1` | Node IC-01~IC-04 | typecheck, M6B 101/101 PASS; agent full focused matrix PASS | IC-05, IC-07~IC-14, full gate |
| `b42a04bf6a` | Node IC-05 | build, typecheck, observation focused 52/52, M6B 101/101 PASS | IC-07~IC-14, full gate |
| `94487f77ff` | IC-07~IC-13 공통 conformance fixture 5종 | fixture validator PASS, Node/C++/.NET/JVM direct consumer 연결 | 네 runtime package·전체 regression |
| `562f19494b` | Node IC-07~IC-14·IC-12 public codec | typecheck, build, lint, focused 146/146, M6A 39/39, M6B 101/101, M6C 91/91 PASS | filter-free package·전체 regression |
| `f08f4d76a7` | Node completion terminal ownership 정리 | copy 실패 terminal 소비 test, completion focused 4/4, channel contract 99/99 PASS | 병렬 build가 끝난 뒤 filter-free gate 재실행 |
| `4c237ffc84` | Node typed decode terminal·Message Follow volume·STREAM dispatch reservation 수렴 | typecheck, build, lint, M6B 101/101, STREAM 55/55, filter-free `npm test` PASS | package provenance와 cross-language 최종 review |
| `a5f08f2f72` | Node ZoneWorld one-way tick과 관측 조건 정렬 | ZoneWorld 실제 process 3/3, focused 14/14, Node sample regression 48/48 PASS | 나머지 네 공개 표면과 합쳐 35개 cell 검증 |
| `cb247017e8` | C++ IC-07~IC-14·CT-03·relocation hold·completion 수렴 | focused 5종, full build, framework-unit 32/32(47.47초), diff-check PASS | package provenance와 cross-language 최종 review |
| `e3daeee813` | Message Follow의 16 MiB 상한을 control envelope 하나에만 적용하고 `u32` queue 값은 포화 진단으로 고정했다. Retained payload admission에는 건수·byte 상한을 두지 않는다. | protocol validator·generator, C++ M6B focused PASS | 네 runtime consumer와 package provenance 최종 review |
| `ac67839a54` | .NET IC-07~IC-14, completion terminal, payload ownership, Message Follow와 lifecycle 책임 수렴 | Unit 1,704/1,704, Contract 76/76, HTTP 64/64, STREAM 147/147, Redis 41/41, package 9종과 standalone HTTP PASS | POSDDD audit의 HTTP codec·startup ingress finding과 최종 cross-language review |
| `607d8bfcd0` | JVM IC-07~IC-14, declared-type codec, exact relocation barrier와 crash-safe retained ingress 수렴 | relocation focused 128, core unit 1,016, Kotlin check, Java/Kotlin package consumer와 frozen API snapshot PASS | POSDDD audit와 최종 cross-language review |

## Benchmark

같은 workload의 변경 전·후 수치를 함께 보관한다. 수치는 공개 SLA가 아니라 canonical
admission bound 변경의 자원·latency 영향을 확인하는 gate다.

| runtime | 기준 | accepted / rejected | 첫 거부 | throughput | p95 / p99 | peak count / bytes | 재수락 / drain |
|---|---|---:|---:|---:|---:|---:|---:|
| Node | 변경 전 4,096 | 4,096 / 256 | 4,097 | 1,651,620/s | 4.475 / 4.483 ms | 4,096 / 5,242,880 | 0.002 / 2.324 ms |
| Node | canonical 1,024 | 1,024 / 3,328 | 1,025 | 1,097,032/s | 14.741 / 14.749 ms | 1,024 / 1,310,720 | 0.002 / 0.607 ms |
| .NET | 변경 전 4,096 | 4,096 / 256 | 4,097 | 7,761,726/s | 0.077 / 0.622 us | 4,096 / 1,048,576 | 1.479 / 2.787 ms |
| .NET | canonical 1,024 | 1,024 / 3,328 | 1,025 | 9,895,407/s | 0.079 / 0.586 us | 1,024 / 262,144 | 0.994 / 1.425 ms |

## 아직 유효하지 않은 증거

- production, generated asset, package 또는 test source가 바뀌면 해당 runtime의 package와
  전체 regression 증거를 다시 만든다.
- 전체 E2E는 release 뒤 실행한다. release 전 E2E 변경은 inventory와 unit/package gate를
  일치시키는 범위에만 사용하며 E2E PASS로 세지 않는다.
- Windows에서 WSL build binary를 직접 실행해 발생한 `BAD_COMMAND`와 UNC Node 실행 실패는
  runtime 결과가 아니다. 같은 명령을 WSL에서 다시 실행한 PASS만 checkpoint에 기록한다.

## 실행 환경 cleanup

- 2026-08-10 환경 점검에서 2026-08-08
  `framework/languages/cpp/e2e/SpotActorTransfer/logs/20260808-153413-562318` 설정을 사용하던
  parent 없는 session process 두 개(PID 562484, 562510)를 확인했다. 현재 candidate 검증과
  무관한 이전 run의 orphan임을 command line으로 확인한 뒤 두 PID만 종료했고, `ps`에서 더
  이상 조회되지 않음을 확인했다.

## 문서 검증

- `python3 doc/site/scripts/check_doc_links.py framework`: 922개 문서와 12,657개 link PASS.
- `python3 doc/site/scripts/check_doc_tabs.py framework`: 922개 문서, 192개 snippet,
  258개 sample tab group PASS.
- `python3 doc/site/scripts/check_prose_neutrality.py`: 공통 정본 27개와 산문 7,388줄 PASS.
- `bash scripts/verify-framework-doc-contracts.sh`: 5개 언어 INSTANCE SPOT·submit API contract와
  public implementation scan PASS.
- Common modified 문서 68개의 `HEAD` anchor를 변경본이 모두 유지한다. Common 문서와 두 ledger
  70개를 대상으로 fence 균형, 명시 anchor 중복, 한국어·영어 heading 수, tab, trailing space와
  conflict marker를 검사했고 오류가 없었다.
- Public 문서의 plan link와 common internals의 raw line citation은 없다.
- `90-implementation-gap.ko.md`와 영어판의 Java/Kotlin 행을 실제 ingress barrier,
  exact high-water와 restart fail-closed 동작에 맞춰 종결했다.
- Runtime conformance fixture, service wire schema와 decoder fixture validator가 모두 PASS했다.
- 문서 범위의 `git diff --check`도 PASS했다.
- 현재 변경과 `HEAD` archive에서 각각 `python3 -m mkdocs build --strict`를 실행했다. 두
  실행 모두 site 전체의 기존 미등록 nav·site 외부 link 때문에 정확히 130개 warning으로
  실패했다. 변경 문서의 link/anchor 회귀는 위 전용 checker에서 발견되지 않았으므로 이
  결과는 새 PASS가 아니라 동일한 baseline-red로 기록한다.

## Release 뒤 E2E 구현 기준선

- Common E2E 한국어·영어 heading에서 직접 산출한 inventory는 14개 config, 359개
  scenario다. `bash framework/languages/cpp/e2e/verify_common_inventory.sh`가 같은 수를
  출력해 사용자 확정 수치와 일치했다.
- 현재 C++ E2E 구현 완전성 기준선은 feature-map 누락 97, source/runner 참조 누락 125,
  incomplete 상태 57로 총 279개 조건이 열려 있다. 따라서 현재 E2E source를 실행 가능한
  것으로 간주하지 않으며, release 뒤 E2E1에서 fixture·runner·evidence 구현부터 닫는다.
