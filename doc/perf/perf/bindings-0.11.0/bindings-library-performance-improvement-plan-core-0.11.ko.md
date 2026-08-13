# core 0.11.0 bindings 성능 측정 및 개선 시트

> 시작일: 2026-08-14
>
> 작업 브랜치: `core-0.10.0-bindings-performance`

이 시트는 Core 0.11.0 release runtime을 기준으로 C와 각 bindings를 비교하는
측정 결과와 판정만 기록한다. 이전 버전의 수치와 완료 판정은 가져오지 않는다.
측정 원본, 재측정 결과와 개선 전후 비교는 같은 폴더의 `log/`에 남긴다.

## 측정 기준

- Core library는 Git release로 배포한 0.11.0 runtime을 사용한다. 측정 목적으로 Core를 다시 build하지 않는다.
- C 기준과 binding은 같은 transport, pattern, message size, duration, client 수, HWM, timeout을 사용한다.
- C와 binding은 동시에 실행하지 않는다. perf는 항상 한 번에 하나만 실행한다.
- 전체 matrix를 한 번에 실행하지 않는다. 현재 개선 대상 transport·pattern 하나만 C와 binding으로 차례로 측정하고 비교한다.
- C perf와 binding perf는 같은 socket 배선, ready-event 처리, send/receive 의미, timestamp 경계를 사용해야 한다. 차이가 확인되면 binding 성능 개선 전에 harness 의미를 맞춘다.
- throughput 비율은 `binding throughput / C throughput * 100`으로 계산한다.
- 한 transport·pattern의 판정값은 지원하는 모든 message size throughput 비율의 산술평균이다. 개별 size 값은 병목 확인 자료로 남기며, 개별 값만으로 통과·미달을 정하지 않는다.
- public contract의 public interface는 변경하지 않는다. 성능 개선과 POSDDD 기반 리팩터링은 기존 계약 안에서 진행한다.

## 핵심 유의 사항

- 측정 전에는 C runner, binding runner, `doc/perf` 정책 문서, 이 시트의 pattern·transport·size·client 수를 대조한다. binding runner에 실제로 등록되지 않은 pattern은 `미지원`으로 표시한다.
- 실제 Core runtime 버전은 runner 또는 binding public version API와 `share/zlink/core-package-provenance.json`으로 확인한다. 이전 local package, 오래된 runtime, source build 결과를 기준값으로 사용하지 않는다.
- C와 binding report가 모두 `status: complete`이고 같은 session 조건을 가질 때만 공식 비교와 표 갱신에 사용한다. timeout, runtime mismatch, message size·client 수 불일치는 성능 판정이 아니라 측정 조건 오류다.
- Effective Options, 실제 client 수, I/O thread 수, auto-HWM profile과 적용 `SNDHWM`·`RCVHWM`, memory guard cap 발생 여부를 C와 binding에서 함께 확인한다. 무시되는 runner option이나 한쪽에만 적용된 cap이 있으면 그 결과를 paired 비교에 사용하지 않는다.
- perf harness는 C와 의미가 다르거나 실제 버그 또는 정책 위반이 있을 때만 수정한다. throughput을 높이기 위해 public API를 우회하거나 C API를 직접 호출하거나, timeout·sleep·retry를 늘리거나, client 수를 줄이지 않는다.
- 개선 전후 비교는 같은 Core release runtime, host session, transport·pattern·size 조건으로 실행한다. Core release, package provenance, host boot 또는 성능 관련 환경이 바뀌면 C 기준을 다시 측정한다.
- `log/`에는 paired C·binding 원본 수치, 계산 결과, 개선 전후 비교와 최종 판정만 남긴다.

## 목표

아래 값은 `개별 size 참고 최소값 / transport·pattern 산술평균 목표`다. 완료 gate는
두 번째 값인 산술평균 목표다.

| 언어 | 단순 one-way | routed one-way | socket request/reply | multi routed echo |
|---|---:|---:|---:|---:|
| C++ | 85% / 90% | 80% / 85% | 75% / 85% | 80% / 85% |
| .NET | 64% / 85% | 75% / 80% | 50% / 70% | 50% / 70% |
| Java | 70% / 90% | 75% / 85% | 50% / 70% | 50% / 70% |
| Node | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |
| Go | 55% / 65% | 50% / 57% | 40% / 53% | 40% / 53% |
| Rust | 85% / 95% | 70% / 85% | 70% / 85% | 70% / 85% |
| Python | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |

Pattern 그룹은 다음과 같이 분류한다.

| 그룹 | 대상 pattern |
|---|---|
| 단순 one-way | `PAIR`, `PUBSUB`, `DEALER_DEALER`, `MULTI_PUBSUB`, `MULTI_STREAM` |
| routed one-way | `DEALER_ROUTER`, `ROUTER_ROUTER` |
| socket request/reply | `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP` |
| multi routed echo | `MULTI_DEALER_ROUTER_SENDSEND`, `MULTI_ROUTER_ROUTER_SENDSEND` |

## 진행과 보류 규칙

1. 대상 transport·pattern의 C 기준과 binding baseline을 측정한다.
2. binding hot path와 책임 경계를 검토하고, 성능 개선 또는 POSDDD 리팩터링 후보를 구현해 같은 조건으로 다시 비교한다.
3. 평균 목표에 미달하고 명확한 다음 후보가 없으면 성능 관점의 Sol 리뷰를 요청한다. 구조 변경은 이 단계에서만 함께 검토한다.
4. 자체 개선과 Sol 리뷰 뒤에도 유효한 후보가 없을 때만 `보류`로 기록한다. 측정 변동, 노트북 환경 또는 안정성은 보류 사유가 아니다.

## 측정 결과

모든 행은 `미측정`에서 시작한다. binding runner에 등록되지 않은 pattern은 `미지원`으로
표시하고 runner 등록 근거를 `log/`에 남긴다.

| 언어 | Suite / pattern | Transport | C 대비 throughput 비율 | 산술평균 | 목표 | 판정 | 결과 기록 |
|---|---|---|---|---:|---:|---|---|
| C++ | - | - | 미측정 | - | - | 미측정 | - |
| .NET | - | - | 미측정 | - | - | 미측정 | - |
| Java | - | - | 미측정 | - | - | 미측정 | - |
| Node | - | - | 미측정 | - | - | 미측정 | - |
| Go | - | - | 미측정 | - | - | 미측정 | - |
| Rust | - | - | 미측정 | - | - | 미측정 | - |
| Python | - | - | 미측정 | - | - | 미측정 | - |

## 언어별 평균

완료 또는 보류로 판정한 transport·pattern 산술평균을 같은 가중치로 평균한다. `미측정`과
`미지원` 행은 포함하지 않는다.

| 언어 | 포함 transport·pattern 수 | C 대비 단순평균 | 상태 |
|---|---:|---:|---|
| C++ | 0 | - | 미측정 |
| .NET | 0 | - | 미측정 |
| Java | 0 | - | 미측정 |
| Node | 0 | - | 미측정 |
| Go | 0 | - | 미측정 |
| Rust | 0 | - | 미측정 |
| Python | 0 | - | 미측정 |
