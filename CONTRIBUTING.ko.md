# zlink 기여·운영 핸드북

이 문서는 zlink 저장소에서 일하는 사람과 에이전트가 처음 읽는 진입점이다. 규칙의 본문은 각
소유 문서에 있고, 여기서는 **어디에 무엇이 있고 어떤 순서로 하는지**만 적는다. 규칙을 이 문서에
복제하지 않는다. 한 사실은 한 문서만 소유한다.

에이전트(Claude, Codex)는 [`AGENTS.md`](AGENTS.md)를 먼저 읽고, 이 문서는 절차와 위치를 찾을
때 참고한다.

## 1. 5분 시작

```bash
git clone git@github.com:zlink-systems/zlink.git && cd zlink

# Core (테스트 포함, Release)
cmake -S core -B core/build -DZLINK_BUILD_TESTS=ON -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build core/build -j
ctest --test-dir core/build -j2

# 바인딩 스모크 (Core는 위 core/build를 사용)
bash bindings/cpp/tests/run_tests.sh
ZLINK_CORE_SOURCE=local bash bindings/python/tests/run_tests.sh
```

- 상세 빌드·플랫폼·CMake 옵션: [`doc/building/build-guide.ko.md`](doc/building/build-guide.ko.md),
  [`doc/building/cmake-options.ko.md`](doc/building/cmake-options.ko.md).
- 빌드 트리는 `scripts/build-core.sh`로 고정한다: `dev`(`core/build-dev`, no-LTO, 테스트 ON) —
  평소 개발·ctest; `release`(`core/build`, LTO, 테스트 OFF) — 출하 라이브러리·perf 측정(라이브러리
  LTO 링크 1회, 2분대); `release-gate`(`core/build`, LTO, 테스트 ON) — 릴리스 직전 `hotpath_gate`와
  LTO 전체 ctest 전용. `<mode> --lib-only`는 그 트리의 `libzlink` 런타임만 다시 빌드하며, perf
  runner가 stale한 `core/build`를 재빌드할 때 이 옵션으로 호출한다(테스트 재링크 없음). Core 테스트
  실행파일은 정적 아카이브에 링크하므로 LTO 트리에서 테스트를 켜면 Core 소스 하나를 고쳐도
  테스트마다 전체 최적화를 다시 한다(그래서 `release`는 테스트 OFF).
- Python 바인딩 테스트는 시스템 Python에 pytest가 없으면
  `PYTHON_EXECUTABLE=<venv>/bin/python`을 절대 경로로 준다.

## 2. 저장소 지도

| 경로 | 소유 | 진입 문서 |
|---|---|---|
| `core/` | C++ Core 런타임과 공개 C API(`core/include`) | [`core/doc/spec/core/`](core/doc/spec/core/README.ko.md) (정식 계약) |
| `core/tests/` | Core 테스트(unittest / integration / perf gate) | [`core/tests/README.md`](core/tests/README.md) |
| `bindings/<lang>/` | 언어 바인딩. `bindings/*/include`는 `core/include`의 **raw header mirror** | 각 바인딩 README |
| `bindings/c/perf/` | C 성능 벤치와 release 비교 gate | [`bindings/c/perf/README.md`](bindings/c/perf/README.md) |
| `framework/` | 언어별 Framework(actor, DI, codec) | [`framework/AGENTS.md`](framework/AGENTS.md) |
| `doc/` | 사용자 문서, 설계 원칙, 빌드, 계획 | [`doc/README.ko.md`](doc/README.ko.md) |
| `doc/plan/` | 캠페인 계획과 판정 기록(공개 계약 아님) | §7 |
| `scripts/local-package/` | Core·바인딩 로컬 패키징, 버전 동기화 | `scripts/local-package/README.ko.md` |

## 3. 코드 규칙

- 설계 원칙: [`doc/principal/dev/zlink-system-design-principles.ko.md`](doc/principal/dev/zlink-system-design-principles.ko.md),
  POSDDD [`doc/principal/dev/posddd.ko.md`](doc/principal/dev/posddd.ko.md),
  도메인 맵 [`doc/principal/dev/zlink-core-domain-map.ko.md`](doc/principal/dev/zlink-core-domain-map.ko.md).
- Core 핫패스(메시지마다 실행되는 경로)는 정식 스펙
  [`core/doc/spec/core/systems/10-hot-path.ko.md`](core/doc/spec/core/systems/10-hot-path.ko.md)가
  규범이다. §3 금지 7항(heap 할당, 문자열 identity, socket 단위 table 조회, 조건 없는 부가 작업,
  reader를 재우는 미리보기, 고정 sleep, 임시 owner 신호 누락)을 어기는 변경은 §4 캐시/후퇴
  형태로 다시 쓴다.
- 주석: [`doc/principal/source-comment-principles.ko.md`](doc/principal/source-comment-principles.ko.md).
  "왜"만 적고 구현을 반복 설명하지 않는다.
- 공개 API·ABI·enum 변경은 스펙 문서 변경과 함께 별도 커밋으로 한다. 계약 없는 API는 구현 전에
  설계 변경으로 분리해 보고한다(`AGENTS.md` §3).

## 4. 테스트 규칙

- 분류와 라벨(`unittest` / `integration` / `e2e` / `regression`, `parallel-safe` / `serial`)은
  [`core/tests/README.md`](core/tests/README.md)가 소유한다.
- 통합 테스트는 **공개 C API만** 사용한다. 내부 심볼·failpoint·synthetic harness에 기대는
  테스트는 리팩토링마다 깨지고 사용자가 보는 동작과 어긋난다. 경쟁 조건은 monitor 이벤트,
  poller의 `ZLINK_CONFIG_BUSY`, flow state처럼 공개 API로 관측 가능한 동기화로 결정적으로
  재현한다(`core/tests/integration/test_wake_invariants.cpp`가 기준 예). 공개 API로 재현할 수
  없는 경쟁은 테스트를 억지로 맞추지 말고 스펙 gap으로 보고한다.
- 단위 테스트는 검사 대상 컴포넌트의 소스를 직접 컴파일한다(라이브러리 링크 없음). 라이브러리
  동작을 보는 테스트는 통합 테스트다. (현재는 두 부류 모두 `libzlink-static`에 링크한다.
  전환 계획은 §9.)
- `sleep`으로 타이밍을 추정하는 테스트를 추가하지 않는다. 새 테스트는 5회 반복 green을 확인한다.
- 알려진 load flake: `test_single_lane_flow_snapshot_accounting`(병렬 suite에서 드물게 즉시 실패 →
  단독 재실행 1회로 판정), C++ 바인딩 테스트의 exit 86/134(재링크 직후 → 1회 재실행).

## 5. 커밋 전 gate

모든 Core 커밋은 아래를 green으로 만든 뒤 커밋 메시지에 결과를 적는다.

```bash
ulimit -v 16777216
cmake --build core/build -j
ctest --test-dir core/build -j2                          # 전체 (hotpath_gate, wake-invariant 포함)
ctest --test-dir core/build -R '^test_single_lane_' -j2  # ×2
for l in c cpp go rust; do for h in zlink_enum.h zlink/socket/api.h zlink/eventing/api.h; do
  cmp -s core/include/$h bindings/$l/include/$h || echo "MIRROR DIFF $l/$h"; done; done
git diff --check
bash bindings/cpp/tests/run_tests.sh
ZLINK_CORE_SOURCE=local bash bindings/python/tests/run_tests.sh
```

- `hotpath_gate`(callgrind, 명령어 수/msg ±5%)는 valgrind가 있을 때만 등록된다. 없으면 "gate
  미실행"을 보고에 남기고 green으로 세지 않는다. 기준값 `core/tests/perf/hotpath_reference.json`은
  감독자만 `--update-reference`로 갱신하며, 의도한 비용 증가는 근거와 함께 판정 기록에 남긴다.
- 성능을 건드린 변경은 §6의 release 비교까지 실행한다.

## 6. 성능 판정

- 판정 기준은 스펙 [`10-hot-path.ko.md` §5](core/doc/spec/core/systems/10-hot-path.ko.md)가
  소유한다: cell(pattern·transport·size·metric) 5%는 측정 오차 허용치, (pattern, transport)별
  size 64·256·1024·65536 기하평균이 baseline보다 낮으면 개선 대상.
- 도구: `bindings/c/perf/run_benchmarks.sh`(single) / `run_benchmarks_multi.sh`(multi),
  `bindings/c/perf/perf_regression_gate.py`. baseline은 직전 release 태그를 같은 머신에서 자체
  빌드한 worktree를 쓴다(`--core-version` 금지).
- 측정 중에는 같은 머신에서 빌드·테스트를 돌리지 않는다. 판정은 cell 단위로 하고, 개선 대상이
  나오면 전체를 기다리지 말고 바로 고친 뒤 재측정한다. 단일 run 편차가 크면 runner의
  `--runs 3`(size별 median)으로 확인한다.
- 벤치가 잘못 재고 있으면(포화 구간의 queue 깊이를 latency로 보고, 반올림이 gate보다 큰 경우)
  gate가 아니라 벤치를 고치고 baseline worktree에 같은 소스를 복사한다.

## 7. 계획과 판정 기록

- 캠페인은 `doc/plan/<campaign>.ko.md`(계획)와 `doc/plan/<campaign>-worklog/`(브리프, 요약,
  드라이버, `decisions.ko.md`)로 남긴다. `doc/plan/**`은 임시 문서이며 공개 문서에서 링크하지
  않는다([`doc/AGENTS.md`](doc/AGENTS.md)).
- 판정은 `decisions.ko.md`에 `## D-NNN (일시, 누가) 제목` 형식으로 append한다. 두 머신이 같은
  캠페인을 병렬로 진행하면 한쪽은 접두를 붙인다(예: `D-B54`). 병합할 때 번호를 다시 매기지 않는다.
- 에이전트 job의 브리프(`briefs/*.prompt`)와 요약(`*-summary.md`)은 그대로 보관한다. 요약에는
  변경 파일, 근거, gate 결과, BLOCKERS를 적는다.

## 8. 브랜치·커밋·PR·릴리스

- 브랜치·commit·push·merge는 사용자가 명시적으로 요청할 때만 한다(`AGENTS.md` §1). 요청이
  없으면 `main`에서 작업한다.
- 커밋 메시지: `<모듈>: <한 줄 요약>` + 본문에 원인·수정·근거 수치·gate 결과. 리팩토링은 항목
  (불필요 코드 제거 / 책임 분리 / 명명)이 diff에서 구분되게 한다.
- 버전 범프 체크리스트(한 커밋에 모두):
  1. `VERSION`, `core/CMakeLists.txt`, `core/include/zlink/common.h`, `core/include/zlink.h`.
  2. raw header mirror: `bindings/{c,cpp,go,rust}/include/zlink.h`, `zlink/common.h`를 `core/include`에서
     그대로 복사(`contract_c_header_mirror`가 검사한다).
  3. 버전을 하드코딩한 계약 테스트 갱신: `bindings/cpp/tests/contract/test_cpp_contract_common_header_version.cpp`.
  4. 바인딩 매니페스트와 `scripts/local-package/build-wsl.sh --sync-versions`.
- 릴리스 태그 조건: §5 gate green, `hotpath_gate` PASS, §6 release 비교 판정 PASS(또는 판정
  기록에 사용자 결정으로 예외 명시), 패키징 검증 `scripts/local-package/core/verify-package.sh`.
- 릴리스 뒤 baseline worktree를 새 태그로 갱신한다.

## 9. 알려진 부채와 예정 작업

- 테스트 링크 구조: 통합 테스트를 릴리스 `libzlink.so`에 동적 링크(공개 API 강제), 단위 테스트는
  소스 직접 컴파일로 전환. 내부 훅을 쓰는 통합 테스트 17개는 공개 API로 재작성, 라이브러리
  동작을 보는 단위 테스트 12개는 통합 테스트로 이동. 완료 전까지는 §1의 `scripts/build-core.sh dev`
  트리로 개발 빌드를 한다.
- 0.16.0 캠페인의 이월 항목(전체 70 cell 4-size sweep, POSDDD 리팩토링 BLOCKERS)은
  [`doc/plan/c016-worklog/decisions.ko.md`](doc/plan/c016-worklog/decisions.ko.md)의 마지막
  판정을 본다.

## 10. 에이전트 운영 관례

- 규칙 본문: [`AGENTS.md`](AGENTS.md)(전역), 디렉터리별 `AGENTS.md`(세부). 문서 작성은
  [`doc/AGENTS.md`](doc/AGENTS.md).
- 큰 작업은 모듈 경계로 나눠 병렬 투입한다. 하나의 job은 **원인 하나, 1.5시간 상한**으로 두고,
  상한 안에 근본 수정이 없으면 실증한 사실과 후보를 요약에 적고 종료한다. 한 job에 여러 원인과
  리팩토링을 묶지 않는다.
- job은 gate·perf 측정을 스스로 반복하지 않는다. 감독자가 job 종료 뒤 gate를 한 번 돌리고,
  diff를 직접 읽은 뒤 파일을 명시해 커밋한다(`git add -A` 금지).
- job은 `doc/**`, `core/doc/**`, `hotpath_reference.json`, `scripts/local-package/**`를 수정·실행하지
  않는다. 스펙 변경이 필요하면 BLOCKERS로 보고하고 감독자가 별도 커밋한다.
