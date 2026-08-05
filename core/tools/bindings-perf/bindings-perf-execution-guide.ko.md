# Bindings Perf Ralph Execution Guide

이 문서는 `core/tools/bindings-perf/` 랄프 루프의 유일한 authority 문서다.
별도 main/master/gap/residual 문서는 만들지 않는다.

## 1. 목적

`/home/hep7/project/kairos/zlink/core/perf/baseline/` 의 core C API baseline
측정값을 기준으로, 선택된 바인딩 언어의 perf surface를 먼저 정책 준수 상태로
전체 패턴/전체 사이즈에서 정상 동작하게 만든 뒤, 그 다음 perf hot path 비효율을
줄여 언어별 목표 비율 이상으로 끌어올린다.

핵심 원칙:

- `perf` benchmark 의미와 core C API baseline 의미가 다르면 그 정합성은 수정한다.
- `STREAM` client shared path는 구조 취향이나 binding-local 구현 선호보다
  우선하는 상위 계약이다.
  `doc/perf`, `core/perf`, 이 guide가 shared stream client 사용을 규정한 경우,
  개별 binding perf는 그 계약을 임의로 binding-local client로 치환하면 안 된다.
- `perf` 수정은 `/home/hep7/project/kairos/zlink/doc/perf/PERF_POLICY.md`,
  `/home/hep7/project/kairos/zlink/doc/perf/PERF_SINGLE_TEST_POLICY.md`,
  `/home/hep7/project/kairos/zlink/doc/perf/PERF_MULTI_TEST_POLICY.md`
  와 다르거나, `core/perf` 구현 의미와 다르거나, benchmark 자체 버그가 있는 경우만 허용한다.
- 항상 `core/perf` 와 동일한 측정 방식을 준수해야 한다.
  측정 방식이 다르면 성능 비교나 원인 판단은 무효로 보며, 그 경우 먼저 perf를
  정책에 맞고 `core/perf` 와 동일한 측정 방식이 되도록 수정해야 한다.
- 위 사유가 아니면 perf 를 수정하면 안 된다.
  그 외 문제는 binding/library/core 를 고쳐야 한다.
- `doc/perf` 가 금지하는 handshake/start gate/ready 판정 방식이 구현, 로그,
  `00_run_state.md`, `00_handoff.md`, `00_notes.md` 에 남아 있으면 그것도
  정책 위반으로 보고 먼저 바로잡아야 한다.
- 우선순위는 항상
  1. perf가 정책을 만족하면서 전체 패턴/전체 사이즈에서 정상 동작하는지 확보
  2. 그 다음 성능 개선
  순서를 따른다.
- benchmark 숫자만 올리기 위해 `perf` 에서만 의미가 있는 코드나 측정 전용 shortcut을 넣으면 안 된다.
- 이 랄프루프의 목표는 `bindings/<lang>/` 라이브러리의 실제 성능 향상이지, perf 숫자만 좋아 보이게 만드는 것이 아니다.
- 성능 격차는 우선 `bindings/<lang>/` 라이브러리 내부 비효율 제거로 해결한다.
- bug가 확인되면 우회 코드를 작성하면 안 된다.
- `bindings/<lang>/` 라이브러리 버그면 해당 바인딩 라이브러리를 직접 수정한 뒤 개선 작업을 계속한다.
- `core` 계약 실패나 core 버그면 우회하지 말고 재현 근거와 함께
  `/home/hep7/project/kairos/zlink/core/doc/bug/` 아래에 `.md` bug report를 작성하고 대기한다.
- 효과 없는 실험, 의미 왜곡, 정책 위반 수정은 남기지 않는다.
- POSD, 언어 스타일, 공통화 축소 같은 구조 원칙은 상위 policy authority를
  덮어쓸 수 없다.
  즉 "더 언어답다", "더 깊은 모듈이다", "더 자연스러운 구조다" 같은 이유만으로
  이미 고정된 perf policy contract를 바꾸면 안 된다.

policy authority 해석 순서:

1. `/home/hep7/project/kairos/zlink/doc/perf/*.md`
2. `/home/hep7/project/kairos/zlink/core/perf/` 의 현재 canonical 구현 의미
3. 이 execution guide
4. 각 binding의 `perf/README.md`, porting plan, implementation plan
5. POSD / 언어 스타일 / 리팩토링 선호

상위 authority와 하위 문서가 충돌하면 상위 authority를 따른다.
하위 문서나 구현이 더 자연스럽게 보이더라도, 상위 authority에 없는 policy 변경은
사용자 승인 없이 수행하면 안 된다.

## 2. 대상 범위

선택된 언어 목록은 환경 변수 `BINDINGS_PERF_LANGUAGES` 를 따른다.
값이 있으면 그 나열 순서 자체가 이번 실행의 고정 작업 순서다.
예를 들어 `cpp,dotnet,java` 이면 반드시 `cpp` 를 먼저 끝내고,
그 다음 `dotnet`, 그 다음 `java` 로 진행한다.
값이 없으면 아래 전체 언어를 대상으로 본다.

- `cpp`
- `dotnet`
- `java`
- `rust`
- `go`
- `node`
- `python`

baseline 디렉터리는 `BINDINGS_PERF_BASELINE_DIR` 를 따른다.
값이 없으면 `/home/hep7/project/kairos/zlink/core/perf/baseline` 를 사용한다.

baseline report 파일은 recv/callback을 분리해서 선택한다.

- recv baseline file: `BINDINGS_PERF_BASELINE_RECV_FILE`
- callback baseline file: `BINDINGS_PERF_BASELINE_CALLBACK_FILE`
- legacy compatibility alias: `BINDINGS_PERF_BASELINE_FILE`
  - 명시적으로 주면 callback baseline file과 같은 의미로 취급한다.

값이 없으면 baseline 디렉터리 안에서 아래 우선순위로 고른다.

1. recv baseline:
   - 가장 최신 `perf_*recv*.txt`
   - 없으면 이름에 `callback` 이 없는 가장 최신 `perf_*.txt`
2. callback baseline:
   - 가장 최신 `perf_*callback*.txt`

선택된 recv/callback baseline file 경로는 매 실행 시작 시 로그에 반드시 남긴다.
같은 비교 기준으로 재현이 필요하면 wrapper의
`--baseline-recv-file <path>` / `--baseline-callback-file <path>` 로
각 report 파일을 명시적으로 고정한다.

## 3. 언어별 기본 목표 비율

사용자가 명시 override 하지 않으면 아래 기본 목표를 사용한다.

- `cpp`: `0.95`
- `dotnet`: `0.90`
- `java`: `0.90`
- `rust`: `0.95`
- `go`: `0.85`
- `node`: `0.75`
- `python`: `0.75`

override 규칙:

- wrapper가 `BINDINGS_PERF_TARGET_<LANG>` 환경 변수를 주면 그 값을 우선 사용한다.
- 새로운 언어가 추가됐는데 목표가 정의되지 않았으면 기본값 `0.80` 으로 시작한다.
- 사용자가 직접 비율을 지정하면 guide 안의 기본값보다 그 지시를 우선한다.

## 4. 작업 원칙

- 사용자 호칭은 항상 `팀장님` 으로 유지한다.
- 변경 범위는 우선 `bindings/<lang>/` 와 해당 언어의 `perf/` 정합성 범위에 한정한다.
- `perf/` 수정은 core baseline 의미와 비교 surface를 맞추는 정합성 수정일 때만 허용한다.
- `perf/` 안에만 의미가 있고 제품 동작에는 반영되지 않는 성능 전용 우회 코드는 금지한다.
- benchmark 결과를 좋게 보이게 만들기 위한 shortcut, 조건 완화, payload 축소, 샘플 수 왜곡은 금지한다.
- 반복 측정은 같은 조건으로 비교한다. 가능한 한 `tcp`, `callback`, baseline과 같은 warmup/duration을 유지한다.
- perf 측정 실행은 항상 한 번에 하나만 수행한다.
- 두 개 이상의 perf runner, build, test, benchmark를 병렬로 돌려 측정치를 오염시키면 안 된다.
- 다른 언어 perf run, 같은 언어의 다른 pattern run, 별도 build/test job도 active perf 측정과 겹치지 않게 순차 실행한다.
- pre-existing dirty state는 건드리지 않는다.
- core bug가 확인되면 `core/` 를 임의 수정하지 말고 bug report를 먼저 남긴다.

## 5. iteration 절차

선택된 각 언어에 대해 아래 순서를 반복한다.
언어 간 순서는 반드시 선택된 언어 목록의 나열 순서를 따른다.

1. 해당 언어 perf surface와 현재 결과 파일 구조를 확인한다.
2. `doc/perf/PERF_POLICY.md`, `doc/perf/PERF_MULTI_TEST_POLICY.md`,
   `doc/perf/PERF_SINGLE_TEST_POLICY.md` 기준으로 현재 surface가
   `core/perf` 와 같은 recv/send/poller/callback 의미를 유지하는지 확인한다.
   위반이 보이면 성능 최적화보다 먼저 정책 준수 상태로 수정한다.
   특히 `STREAM` shared client 같은 explicit shared component 계약은
   "binding-local 구현 누락"으로 오판하면 안 된다.
3. core baseline과 비교 가능한 transport/pattern/size 조합을 식별한다.
4. perf 의미가 어긋난 부분이 있으면 먼저 그 정합성을 고친다.
5. 먼저 전체 패턴/전체 사이즈 범위에서 perf가 정상 동작하는지 확인하고,
   실패하는 pattern/size가 있으면 성능 개선보다 먼저 그 정상 동작을 복구한다.
6. 정상 동작과 동일 의미 비교가 확보되면 바인딩 라이브러리 hot path 병목을 찾는다.
7. 작은 메시지와 큰 payload를 분리해서 원인을 찾는다.
8. 실제로 성능이 오른 변경만 남기고, 효과 없거나 회귀인 실험은 원복한다.
9. 부분 probe로 방향을 확인한 뒤 전체 대상 조합을 다시 측정한다.
10. 각 작업 묶음을 끝낼 때마다 다시 한 번 `doc/perf` 규칙 준수 여부와
    전체 패턴/전체 사이즈 정상 동작 여부를 확인한다.
11. 매 턴 시작 시와 종료 직전에도 현재 변경이 `doc/perf` 규칙을 계속 지키는지 다시 확인한다.
12. probe, repro, full comparable run 결과는 항상 해당 언어의
    `bindings/<lang>/perf/results/` 아래 report로 남긴다.
13. 정상 동작과 정책 준수가 확보된 뒤에도 목표 비율 미달 항목이 남아 있으면 다음 병목으로 이어간다.

중요 해석 규칙:

- 언어 간 우선순위는 항상 사용자가 지정한 순서가 절대 우선이다.
- 현재 언어가 `completed` 또는 명시적 `blocked` 로 정리되기 전에는
  다음 언어로 넘어가면 안 된다.
- 현재 언어가 `blocked` 이면 다음 언어로 자동 진행하지 않고,
  `사용자 입력 필요: <짧은 사유>` 또는 blocking 사유를 먼저 보고한다.
- `가장 큰 미달 항목부터` 규칙은 언어 내부 우선순위에만 적용한다.
  즉 현재 활성 언어 안에서 `single`, `multi`, `multi callback(stream, spot)`
  중 가장 큰 미달 항목부터 처리한다.
- 작업 레지스터의 `completed` 또는 `완료` 표시는 "이전 실행에서 마지막으로 확인된 상태"일 뿐이다.
- 새 랄프 실행이 시작되면 선택된 언어는 모두 다시 baseline 대비 현재 상태를 재확인해야 한다.
- 즉 `completed` 언어라도 현재 workspace, baseline, perf runner, report가 바뀌었을 수 있으므로 skip 하면 안 된다.
- skip 이 허용되는 경우는 이번 실행에서 방금 같은 조건으로 재측정했고 목표 충족이 다시 확인된 직후뿐이다.
- perf 수정 후보가 나와도 그것이 정책 정합성 수정인지, benchmark bug 수정인지, 숫자 부스팅용 편법인지 먼저 구분해야 한다.
- perf 수정 후보가 shared component 제거, canonical surface 교체, policy contract
  재해석을 포함하면 먼저 "상위 authority가 실제로 그 변경을 허용하는가"를
  문서로 다시 확인해야 한다.
- 편법이면 수정하지 않고 바인딩 라이브러리 비효율 제거 쪽으로 다시 돌아간다.
- 전체 패턴/전체 사이즈에서 정상 동작하지 않는 상태에서는 성능 수치 개선보다
  정상 동작 복구가 우선이다.
- perf 수정 중 정책 위반을 발견하면 그 턴 안에서 우선 정책 준수 상태로 복구하거나,
  즉시 blocker/bug 후보로 승격해야 한다.
- `doc/perf` 에 정의된 recv/send/poller/callback semantics를 바꾸는 수정은 금지한다.
- 특히 `core/perf` 의 blocking recv / poller drain 의미를 피하려고
  binding perf를 `nonblocking recv` 또는 다른 I/O model로 바꾸면 안 된다.
- 이런 종류의 실패는 workaround가 아니라 binding bug 또는 core bug 후보로 다뤄야 한다.
- `READY,<endpoint>`, `CLIENT_READY,<size>`, `START,<size>` 같은 stdout 제어
  문자열은 외부 orchestration 신호로만 다뤄야 한다.
- 이 문자열들을 delivery-ready 근거, benchmark start gate, 정상 동작 판정 근거로
  서술하거나 사용하면 정책 위반이다.
- 이런 표현이 session 파일이나 handoff에 남아 있으면 다음 작업 전에 먼저 수정한다.
- monitor ready 계약은 `bindings/README.md`, `doc/perf/PERF_POLICY.md`,
  `doc/perf/PERF_SINGLE_TEST_POLICY.md`, `doc/perf/PERF_MULTI_TEST_POLICY.md`
  의 최신 정의를 그대로 따른다.
- `*_READY_CHANGED` monitor event 의 `value` 를 aggregate ready count 나
  expected peer count 계약으로 취급하면 안 된다.
- binding public API 나 perf helper 가 monitor snapshot 에 stable ready-count
  surface 가 있다고 가정하면 안 된다.
- raw perf/pattern 의 실제 ready gate 는 low-cost edge 인
  `CONNECTION_READY` event counting 이다.
- raw perf 에서 snapshot polling, delivery-ready/count 계열 event 를 새 ready
  gate contract 로 승격하면 정책 위반이다.
- SPOT perf 의 실제 ready/start gate 는 explicit `READY/START` barrier 이다.
- SPOT perf 에서는 snapshot polling, ready-count wrapper 를 사용하면 안 된다.
- session 파일과 handoff 에 `READY,...`, `CLIENT_READY,...`, `START,...` 표현이
  남아 있어도 그것이 외부 orchestration 의미로만 기록된 경우는 허용된다.
- 반대로 그 표현을 raw ready 판정, SPOT start 판정, 정상 동작 근거로 서술한
  경우만 정책 위반으로 보고 수정한다.

재발 방지 절차:

- perf 구조를 바꾸기 전에 아래 질문을 먼저 체크한다.
  - 이 변경이 policy authority의 명시 contract를 바꾸는가?
  - 이 변경이 shared component를 binding-local 구현으로 치환하는가?
  - 이 변경이 canonical public surface를 늘리거나 바꾸는가?
  - 이 변경 사유가 policy 정합성/bug 수정이 아니라 구조 선호인가?
- 위 질문 중 하나라도 `예` 이면 코드를 수정하기 전에 상위 authority 근거를
  다시 적고, 사용자 승인 없이 policy 변경으로 진행하면 안 된다.

## 6. 성능 비교 기준

반드시 아래를 지킨다.

- 비교 범위는 `single`, `multi recv`, `multi callback` 전체다.
- `multi callback` 비교에는 최소 `STREAM`, `SPOT` 패턴이 포함되어야 한다.
- `single` 은 build/test/probe 정합성 확인 대상이지만, baseline ratio 종료 판정 대상은 아니다.
  `doc/perf/PERF_SINGLE_TEST_POLICY.md` 기준으로 single 정책에는 baseline 저장/비교 모드가 없다.
- 비교 기준은 `core/perf/baseline` report 의 `RESULT,current,...,throughput,...` 값이다.
- `multi recv` 비교 기준 파일은 이번 실행에서 wrapper가 선택해 출력한
  `BINDINGS_PERF_BASELINE_RECV_FILE` 이다.
- `multi callback` 비교 기준 파일은 이번 실행에서 wrapper가 선택해 출력한
  `BINDINGS_PERF_BASELINE_CALLBACK_FILE` 이다.
- binding 측정은 같은 pattern/transport/size/client-count 의미를 가져야 한다.
- `core/perf` 와 다른 측정 방식, phase 구성, recv/send model, poller/callback model,
  drain 방식으로 얻은 숫자는 비교 근거나 원인 판단 근거로 쓰면 안 된다.
- 이런 차이가 확인되면 멈추는 것이 아니라, 먼저 perf surface를 `core/perf` 와
  동일한 측정 방식으로 고친 뒤에만 다음 판단과 성능 개선을 진행한다.
- 정상 동작 판정에는 throughput만이 아니라 해당 surface에서 `doc/perf` 가
  MUST 로 요구하는 지표가 모두 채워져 있어야 한다.
- CPU/memory/queue 관련 산출물은 `core/perf` 와 같은 결과 shape 를 목표로 맞추되,
  필수/선택 여부와 `N/A` 허용 범위는 `doc/perf` 의 최신 정의를 그대로 따른다.
- 즉 `doc/perf` 가 수집 실패나 미지원 transport 에 대해 `N/A` 를 허용하는
  리소스 컬럼은 그 허용 범위를 넘겨서 실패 판정에 사용하면 안 된다.
- 반대로 `doc/perf` 가 informational 로 분류한 queue metric 을 이 가이드가
  임의로 완료/정상 동작의 필수 조건으로 승격하면 안 된다.
- 이 기준은 전체 패턴/전체 사이즈에 적용한다.
- surface별 기대 산출물은 아래를 따른다.

| surface | 기대 산출물 |
|---|---|
| `single` | `Throughput`, `Bandwidth`, `Lat.Mean`, `Lat.P95`, `Lat.P99`, `CPU%`, `Mem MB` |
| `multi recv` | `Throughput`, `Bandwidth`, `Lat.Mean`, `Lat.P95`, `Lat.P99`, `S.CPU%`, `S.Mem MB`, `Q.Snd.Max`, `Q.Rcv.Max`, `Q.Rcv.End` |
| `multi callback` | `Throughput`, `Bandwidth`, `Lat.Mean`, `Lat.P95`, `Lat.P99`, `S.CPU%`, `S.Mem MB`, `Q.Snd.Max`, `Q.Rcv.Max`, `Q.Rcv.End` |

- throughput/latency 계열 MUST metric 이 비거나 깨지면 정상 동작 아님으로 판정한다.
- 리소스 컬럼은 `doc/perf` 가 허용하는 범위의 `N/A` 를 인정한다.
- queue metric 은 `core/perf` 와 동일한 산출물 정합성 대상으로 추적하되,
  `doc/perf` 의 informational 분류를 넘겨 단독 blocker 로 승격하지 않는다.
- probe나 smoke report라도 baseline과 다른 client count, warmup/duration,
  recv mode면 comparable report로 취급하지 않는다.
- comparable report 는 baseline이 요구하는 pattern/transport/msg-size coverage 와
  같은 범위를 가져야 하며, partial repro/probe report 는 comparable 로 취급하지 않는다.
- report 의 effective options 에 coverage 정보가 비어 있거나, throughput result key
  집합이 baseline 기대 범위를 덜 덮으면 non-comparable 로 처리한다.
- primary 판정 지표는 throughput ratio 이다.
- latency는 진단 보조 지표로만 사용한다.
- report가 여러 개면 가장 최근의 comparable report를 기준으로 삼되, 회귀가 보이면 더 넓은 full run으로 재확인한다.
- 측정 중에는 다른 perf run, 다른 언어 bench, 백그라운드 benchmark, 병렬 `ctest`/`dotnet test`/`gradle test`/`cargo test` 등을 겹치게 실행하지 않는다.
- comparable 의미가 맞지 않으면 perf 정합성을 먼저 고치고, 의미가 이미 맞으면 라이브러리를 고친다.

## 7. 언어별 최소 실행 규칙

각 언어 iteration 에서 최소한 아래를 수행한다.

- 해당 언어 perf runner 존재/실행 가능 여부 사전검사
- 관련 빌드 명령
- 관련 테스트 명령
- 전체 패턴/전체 사이즈 정상 동작 full run 확인
- `single` 부분 perf probe
- `multi` 부분 perf probe
- `multi callback` 부분 perf probe
- 필요 시 전체 comparable perf 재측정

필수 최종 비교 surface:

- `multi recv`
- `multi callback`
- `multi callback` 필수 패턴: `STREAM`, `SPOT`

`single` 은 최종 ratio 비교 surface 가 아니라 build/test/probe 정합성 확인용
validation surface 로 유지한다.

bug 처리 규칙:

- 바인딩 라이브러리 버그: 해당 `bindings/<lang>/` 코드 직접 수정, 관련 검증 후 perf 개선 계속 진행
- 바인딩/통합층에서만 재현되고 `core/tests/` 저장소 재현으로 아직 옮기지 못한 문제는
  우선 `bindings/<lang>` 또는 binding-to-core integration bug 후보로 분류한다.
- core 라이브러리 버그: `/home/hep7/project/kairos/zlink/core/doc/bug/` 디렉터리를 만들고
  `YYYYMMDD_<lang>_<pattern>_<short-title>.md` 형식으로 버그레포트 작성 후 대기
- core bug로 분류하려면 같은 실패가 binding 레이어 밖의 `core/tests/` 저장소 재현에서
  다시 확인돼야 하며, binding-only 원인이 충분히 배제돼야 한다.
- 즉 perf surface 또는 특정 binding에서만 보이는 증상만으로는 core bug로 넘기지 않는다.
- 먼저 binding 코드, FFI/integration layer, perf runner semantics, 입력/수명/동시성 사용을 충분히 확인한다.
- core bug report는 한글로 작성한다.
- core bug로 `blocked` 처리하기 전에는 버그레포트 파일을 먼저 만들고,
  그 절대 경로와 파일명을 `00_run_state.md`, `00_handoff.md`, `00_notes.md`,
  그리고 최종 blocker 메시지에 명시한다.
- core bug를 perf helper 수정이나 binding 우회 코드로 숨기면 안 됨

### 7.1 바인딩 병목 참고 목록

자주 나오는 병목은 아래 범주부터 먼저 의심한다.
실제 수정은 comparable report, probe, build/test로 확인된 경우에만 남긴다.

- 불필요한 메시지 복사
  - native buffer로 바로 넘길 수 있는데 중간 `byte[]`, `Buffer`, `Vec`, `String` 복사를 한 번 더 하는 경우
  - 작은 메시지에서 ratio가 특히 나쁘면 먼저 확인한다
- per-message 객체 생성과 GC/allocator 압박
  - send/recv 한 번마다 wrapper 객체, temporary slice/view, closure, future를 새로 만드는 경우
  - Node/Python/Java/C# 에서 작은 메시지 multi 패턴이 무너지면 우선 점검한다
- thread-local 재사용 누락
  - 반복적인 임시 버퍼, encoder/decoder scratch, callback bridge 작업공간을 thread-local로 재사용할 수 있는데 매 호출마다 새로 할당하는 경우
  - 작은 메시지 hot path에서 allocator 비용이 지배적이면 우선 검토한다
  - 단, thread-safe 계약과 수명 규칙을 깨지 않는 범위에서만 적용하고 cross-thread 공유 캐시로 바꾸면 안 된다
- callback bridge 오버헤드
  - callback마다 boxing, lambda 재생성, 스레드 hop, dispatcher/post를 거치는 경우
  - `multi callback` 이 `multi recv` 보다 유독 더 나쁘면 먼저 본다
- 문자열/인코딩 변환 남발
  - endpoint, topic, service name, metadata를 호출마다 UTF 변환하거나 새 문자열로 만드는 경우
  - setup path로 옮길 수 있는지 먼저 본다
- 불필요한 락 또는 cross-thread handoff
  - thread-safe 보장을 이유로 send/recv fast path 전체를 mutex로 감싸는 경우
  - callback delivery를 위해 queue hop을 추가한 경우
- FFI 경계에서의 과도한 안전 래핑
  - 매 호출마다 bounds check, option normalization, exception translation을 중복 수행하는 경우
  - 정책상 필요한 검증인지 setup 단계로 이동 가능한지 구분한다
- poller/event loop 적응층 비용
  - async wrapper가 매 이벤트마다 추가 wakeup, promise scheduling, selector registration churn을 만드는 경우
  - Node/Python async surface와 callback surface 차이를 같이 본다
- 배치 불가 구조
  - native는 한 번에 처리 가능한데 binding이 per-message 호출만 강제해서 FFI 왕복이 늘어나는 경우
  - multi pattern에서만 비정상적으로 ratio가 떨어지면 확인한다
- 옵션/메타데이터 재적용
  - socket 생성 후 한 번만 하면 되는 설정을 hot path에서 반복 적용하는 경우
- 디버그/추적 코드 잔존
  - perf logging, trace hook, verbose formatting, safety counter가 hot path에 남아 있는 경우

언어별로 먼저 볼 포인트:

- `cpp`: 옵션 재적용, monitor/service helper 우회층, thread-safe 락 범위를 먼저 본다
- `dotnet`: delegate marshaling, `byte[]` 재할당, thread-static/thread-local scratch, pinning 전략, task/async 경계를 먼저 본다
- `java`: JNI copy 여부, direct buffer 사용, `ThreadLocal` scratch, per-call object churn, callback bridge thread hop을 먼저 본다
- `go`: cgo 호출 횟수, slice 재구성, callback trampoline, goroutine/channel handoff를 먼저 본다
- `node`: N-API handle 생성, Buffer 변환, async work queue, JS callback 재진입 비용을 먼저 본다
- `python`: GIL 구간, bytes/object 재생성, ctypes/cffi 변환, callback 시 Python 객체 조립 비용을 먼저 본다
- `rust`: clone, `thread_local!` scratch, closure boxing, channel hop, FFI safety wrapper 중복을 먼저 본다

예시 surface:

- `bindings/<lang>/perf/run_benchmarks.sh`
- `bindings/<lang>/perf/single/run_benchmarks.sh`
- `bindings/<lang>/perf/run_benchmarks_multi.sh`
- 해당 언어 테스트/빌드 surface

## 8. 종료 조건

선택된 모든 언어에 대해 아래가 만족되면 종료한다.

- perf가 `doc/perf` 정책을 만족하는 의미로 동작한다.
- 대상 언어의 perf surface가 전체 패턴/전체 사이즈 범위에서 정상 동작한다.
- 대상 surface에서 `doc/perf` 가 MUST 로 요구하는 지표가 전체 패턴/전체
  사이즈에서 채워진다.
- 리소스/queue 산출물은 `core/perf` 와 같은 shape 로 관리하되, `doc/perf` 가
  허용한 `N/A` 와 informational 분류를 그대로 따른다.
- `single` build/test/probe 가 현재 코드 기준으로 유효하고 정책 위반이 없다.
- `multi recv`, `multi callback` comparable perf 주요 조합이 언어별 목표 비율 이상이다.
- `multi callback` 에서는 최소 `STREAM`, `SPOT` 패턴이 목표 비율 이상이다.
- 남아 있는 미달 항목이 측정 편차가 아니라 구조적 한계인지 확인됐다.
- 더 진행하려면 core bug report 또는 사용자 정책 결정이 필요한 상태다.
- build/test 가 현재 작업본에서 통과한다.

## 9. Codex 종료 메시지 계약

각 iteration 마지막에는 아래 셋 중 하나만 정확히 출력한다.

- 모든 선택 언어가 현재 목표를 충족했고 더 남은 적용 항목이 없으면:
  `미적용 사항이 없습니다.`
- 사용자 정책 결정이나 명시 입력이 없으면 위험한 상태면:
  `사용자 입력 필요: <짧은 사유>`
- 그 외 아직 남은 적용 항목이 있으면:
  `계속 진행 필요`

## 10. 로그 및 보고 규칙

- 실행 중 상태 체크, handoff, 메모, 반복 체크리스트는 실행가이드 본문에 직접 기록하지 않는다.
- 현재 session dir 아래 파일을 기본 기록 위치로 사용한다.
  - `00_run_state.md`
  - `00_checklist.md`
  - `00_notes.md`
  - `00_handoff.md`
- 새 session이 시작되면 먼저 직전 session dir 와 그 안의 상태 파일, 최신 `*_codex.log` 를 확인한다.
- 직전 session에 미완료 사항이 남아 있으면 현재 session 파일에 먼저 이어받고, 그 항목부터 해결한다.
- 직전 session 파일에 `doc/perf` 위반 표현이나 현재 규칙과 충돌하는 서술이 남아 있으면,
  그 표현부터 현재 규칙에 맞게 정리한 뒤에만 다음 작업으로 진행한다.
- perf 측정 결과는 항상 각 언어의 `bindings/<lang>/perf/results/` 아래 report로 남아야 한다.
- probe, repro, full comparable run 모두 결과 파일을 남기고 그 경로를 session 파일에 기록한다.
- 결과를 남기지 않는 ad-hoc perf 실행, 임시 디렉터리 전용 실행, stdout-only 측정은 금지한다.
- 측정 report 경로를 항상 남긴다.
- baseline 대비 ratio는 핵심 미달 항목 위주로 요약한다.
- 변경 파일은 high-signal 만 보고한다.
- bug report가 필요하면 재현 조건, 기대값, 실제값, baseline 근거를 같이 적는다.

## 11. 작업 레지스터

이 섹션은 실행가이드 안에 유지하는 정책 템플릿이다.
실제 iteration 상태 갱신은 session dir 파일에 기록한다.

### 11.1 현재 실행 컨텍스트

- baseline dir: `BINDINGS_PERF_BASELINE_DIR` 또는 기본값
- recv baseline file: `BINDINGS_PERF_BASELINE_RECV_FILE` 또는 최신 recv baseline
- callback baseline file: `BINDINGS_PERF_BASELINE_CALLBACK_FILE` 또는 최신 callback baseline
- selected languages: `BINDINGS_PERF_LANGUAGES` 또는 전체 언어
- target ratios:
  - `cpp`: `BINDINGS_PERF_TARGET_CPP` 또는 `0.95`
  - `dotnet`: `BINDINGS_PERF_TARGET_DOTNET` 또는 `0.90`
  - `go`: `BINDINGS_PERF_TARGET_GO` 또는 `0.85`
  - `java`: `BINDINGS_PERF_TARGET_JAVA` 또는 `0.90`
  - `node`: `BINDINGS_PERF_TARGET_NODE` 또는 `0.75`
  - `python`: `BINDINGS_PERF_TARGET_PYTHON` 또는 `0.75`
  - `rust`: `BINDINGS_PERF_TARGET_RUST` 또는 `0.95`

### 11.2 언어별 상태

상태 해석 규칙:

- `pending`: 아직 이번 실행에서 baseline 재확인을 하지 않음
- `in_progress`: 이번 실행에서 측정/분석/수정 중
- `completed`: 이번 실행에서 정책 준수, 전체 패턴/전체 사이즈 정상 동작,
  현재 목표 충족을 모두 다시 확인함
- `blocked`: 사용자 정책 결정, 환경 문제, core bug report 대기 등으로 진행 중단

권장 기록 형식:

- `<lang>: pending`
- `<lang>: in_progress (<surface/status 요약>)`
- `<lang>: completed (policy ok, full surface ok, single ok, multi ok, callback stream ok, callback spot ok)`
- `<lang>: blocked (<짧은 사유>)`

추가 규칙:

- 선택된 언어 순서에서 가장 앞선 미완료 언어만 `in_progress` 로 둘 수 있다.
- 그 뒤 언어들은 모두 `pending` 이어야 하며 선행 측정/수정 상태로 올리면 안 된다.
- 현재 언어가 `blocked` 이면 뒤 언어를 `in_progress` 로 바꾸지 않는다.
- 실제 값은 현재 session dir의 `00_run_state.md` 와 `00_notes.md` 에 기록한다.

### 11.3 반복 체크리스트

이 체크리스트는 현재 session dir 의 `00_checklist.md` 에서 체크한다.
실행가이드 본문에는 체크 표시를 남기지 않는다.

최소 필수 항목:

- [ ] previous session unresolved items reviewed
- [ ] session 파일/로그 해석에 `doc/perf` 금지 handshake 또는 start gate 표현이 없는지 확인
- [ ] `core/perf` 와 동일한 측정 방식인지 확인
- [ ] 전체 패턴/전체 사이즈 정상 동작 확인
- [ ] `doc/perf` MUST 지표 채워짐 확인
- [ ] 리소스/queue 산출물의 `N/A` 허용 여부와 informational 분류 확인
- [ ] baseline comparable surface 확인
- [ ] perf 의미 정합성 점검
- [ ] binding hot path 병목 식별
- [ ] build/test 검증
- [ ] active perf 측정과 겹치는 다른 benchmark/test/build job 없음
- [ ] 부분 perf probe 확인
- [ ] full comparable rerun 확인
- [ ] 결과 파일이 `bindings/<lang>/perf/results` 아래 저장됐는지 확인
- [ ] 목표 ratio 미달 항목 갱신
- [ ] 회귀 실험 원복 여부 확인

## 12. 실행 시작 시 첫 행동

루프가 시작되면 먼저 아래를 수행한다.

1. 현재 session dir 의 `00_handoff.md`, `00_run_state.md`, `00_checklist.md`, `00_notes.md` 위치를 확인한다.
2. 직전 session dir 가 있으면 그 안의 `00_handoff.md`, `00_run_state.md`, `00_checklist.md`, `00_notes.md`, 최신 `*_codex.log` 를 먼저 확인한다.
3. 직전 session 의 미완료 항목과 blocker를 현재 session 파일로 이어받는다.
4. 직전 session 파일에 `READY,...`, `CLIENT_READY,...`, `START,...` 를 raw ready
   판정이나 benchmark start gate 로 오해하게 만드는 서술, 또는 그와 동등한
   monitor 정책 위반 서술이 남아 있으면 먼저 수정한다.
5. 선택 언어 목록과 target ratio를 요약한다.
6. 각 언어의 perf runner 존재 여부를 확인한다.
7. 각 선택 언어에 대해 먼저 `doc/perf` 정책 준수 여부와 전체 패턴/전체 사이즈 정상 동작 여부를 확인하는 초기 상태표를 만든다.
8. 가장 최근 report가 probe/smoke/non-comparable이면 제외하고, 가장 최근 comparable report와 baseline을 비교해 현재 미달 항목을 표로 만든다.
9. 선택된 순서에서 가장 앞선 미완료 언어 하나를 현재 활성 언어로 고정한다.
10. 현재 활성 언어 안에서는 먼저 정책 준수와 전체 정상 동작을 만족시키고, 그 다음에만 `single`, `multi`, `multi callback(stream, spot)` 성능 미달 항목을 줄인다.
11. 현재 활성 언어가 `completed` 또는 명시적 `blocked` 로 정리되기 전에는 다음 언어를 시작하지 않는다.

## 13. session 기록 규칙

실행별 구현 메모와 검증 증거는 실행가이드 본문에 누적하지 않는다.
현재 session dir 파일에만 기록한다.

- 구현/원인 분석 메모: `00_notes.md`
- 현재 상태 대시보드: `00_run_state.md`
- 반복 체크: `00_checklist.md`
- 다음 실행에 넘길 요약: `00_handoff.md`

권장 기록 내용:

- 변경한 파일과 이유
- 실행한 명령과 핵심 결과
- perf report 경로와 comparable 여부
- 목표 ratio 미달 항목, 현재 활성 언어, 다음 작업
- blocker 또는 사용자 결정 필요 사항

`00_run_state.md` 권장 필드:

- `active_language`: 현재 작업 중인 바인딩 언어
- `active_mode`: 예: `single`, `multi recv`, `multi callback`
- `current_focus`: 현재 턴의 핵심 작업
- `current_issue`: 현재 잡고 있는 실패/병목/정책 이슈
- `worst_ratio`: 현재 최악 ratio 와 key 정보
- `comparable_report`: 현재 기준으로 보는 latest comparable report 경로
- `current_action`: 지금 수행 중인 수정/검증
- `next_action`: 다음 바로 수행할 작업
- `blocker`: 막혀 있으면 짧은 사유, 아니면 `none`
- `latest_verification`: 가장 최근 검증 명령과 결과
- `changed_files`: 이번 session에서 건드린 핵심 파일, 없으면 `none`

이 필드들은 매 턴마다 최신 상태로 갱신한다.
특히 `active_language`, `active_mode`, `current_issue`, `worst_ratio`,
`current_action`, `next_action`, `latest_verification` 은 비워 두지 않는다.
`blocker`, `changed_files` 도 빈칸으로 두지 않고 `none` 또는 실제 값을 적는다.
`comparable_report` 는 comparable report가 없으면 이유를 함께 적고,
`worst_ratio` 도 `pending` 인 사유를 같이 적는다.
