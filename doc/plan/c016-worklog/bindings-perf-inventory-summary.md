# bindings 0.17.0 inventory gate 요약

## 결과

- C++/.NET/Java/Node/Go/Rust/Python runner의 source registry, CLI parser, README와
  single/multi wrapper `--help`를 대조했다.
- 7개 binding 모두 canonical 등록 목록은 Single 7개, Multi 7개로 C 공식 runner와 같다.
- 계획서에 누락됐던 Node Multi REQREP 2개와 Go/Rust/Python Single·Multi REQREP 행을
  추가했다. runner에 미등록된 기존 pattern은 없어 제외 행은 없다.
- C의 Single/Multi 기본 크기와 `MULTI_STREAM` 예외는 계획서 §3과 일치한다.
- Core REQUEST 계약 `7d8205a028`, binding 8개 REQUEST 포팅, C perf REQREP runner
  `e5770ec569` 및 후속 정렬 `0f9c329764`의 완료 상태를 §10.1에 반영했다.
- 실제 host/toolchain/Core artifact 정보와 순차 측정 규칙을 환경 manifest에 기록했다.

## 변경 파일

- `doc/perf/perf/bindings-0.17.0/bindings-library-performance-improvement-plan-core-0.17.0.ko.md`
- `doc/perf/perf/bindings-0.17.0/log/2026-09-05-environment.ko.md`

## 검증

- 7개 언어별 Single 42행(7 pattern × 6 transport), Multi 28행(7 pattern × 4 transport)
  구성과 중복 부재를 정적 검사했다.
- 적용 가능한 결과 셀은 모두 `미측정`이며, `MULTI_STREAM`의 정책 제외 크기만 기존
  `해당 없음`을 유지했다.
- `git diff --check`: PASS
- 빌드와 benchmark는 요청에 따라 실행하지 않았다. 다만 Node 공식 wrapper 두 개는
  `--help`를 처리하기 전에 자체 incremental TypeScript/native build 단계를 실행했다.
  전달한 인자는 `--help`뿐이며 tracked 파일 변경은 없었다.

## 남은 확인

- 측정 시작 전 Core public version API, runner option 적용 여부와 memory guard를 확인해야 한다.
- .NET Multi, Java, Node, Python README 일부는 현재 REQREP registry보다 뒤처져 있으나
  수정 허용 범위 밖이어서 환경 manifest에 불일치만 기록했다.
