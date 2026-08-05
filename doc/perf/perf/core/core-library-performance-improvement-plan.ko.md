# core 라이브러리 성능 개선 계획

> 이 문서는 `bindings/c/perf`를 기준 측정 도구로 사용해 `core/` 라이브러리의 실제
> 런타임 성능을 지속적으로 개선하기 위한 실행 계획이다.
>
> perf runner, perf client/server, report 계산식은 개선 대상이 아니다. perf에만 의미가
> 있는 shortcut이나 benchmark 전용 우회는 하지 않는다. 성능 개선은 실제 core runtime
> hot path에 남아야 한다.

## 1. 범위

대상은 `/home/hep7/project/kairos/zlink/core` 아래의 core 라이브러리 구현이다.
필요한 회귀 테스트는 `core/tests`에 추가할 수 있다. 측정은 `bindings/c/perf`의
공식 runner를 사용한다.

이번 계획에서 perf 코드는 아래 경우가 아니면 수정하지 않는다.

- perf가 core 공개 계약과 다른 의미를 측정하고 있음이 재현으로 확인된 경우
- runner가 stale `core/build` runtime을 사용하거나, 결과 파일을 잘못 저장하는 경우
- 결과 계산이나 실패 판정 자체가 명백히 틀린 경우

위 경우에도 perf 수정은 별도 버그 수정으로 분리한다. core 성능 개선 결과로 계산하지
않는다.

## 2. 고정 원칙

- 성능 개선은 core 사용자에게도 의미가 있어야 한다.
- `bindings/c/perf`만 빠르게 만드는 변경은 금지한다.
- 보안 하드닝을 되돌려 성능을 얻지 않는다.
- `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`에서 2026-06-14에 처리
  완료된 항목은 성능 작업의 보호 제약으로 본다.
- perf 기본 조건을 바꿔서 수치를 올리지 않는다.
- `core/src` 또는 `core/include`를 수정한 뒤에는 반드시 `cmake --build core/build`로
  실제 runtime을 다시 만든다.
- perf 결과는 runner가 출력한 `Perf runtime libzlink:` 경로가 `core/build` 아래인지
  확인한 뒤에만 비교 기준으로 쓴다.
- 단일 측정은 흔들릴 수 있으므로 `5%` 미만 차이는 원칙적으로 오차로 본다.
- `10%` 이상 반복되는 차이만 성능 회귀 또는 의미 있는 개선으로 본다.
- 실패 수정과 성능 개선은 분리한다. 실패가 있으면 먼저 실패 0개 상태를 만든다.
- 원인 분석 없이 수치만 보고 변경하지 않는다. 변경 전에는 병목 가설과 검증 방법을
  작업 로그에 적는다.
- 개선 효과가 없거나 `5%` 미만이면 변경을 남기지 않는다. 단, 실패 수정이나 명확한
  코드 품질 개선은 성능 개선과 별도로 판단한다.

### 2.1 보안 하드닝 보호 규칙

성능 개선 라운드는 아래 보안 수정의 의미를 유지해야 한다.

| 보안 항목 | 보호해야 할 의미 |
|-----------|------------------|
| mtrie 비재귀화 | 원격 구독 prefix가 깊어져도 소멸자와 `visit_values`가 C++ 호출 스택을 prefix 깊이만큼 쓰지 않아야 한다. |
| WS/WSS 버퍼 사본 제거 | 큰 WebSocket 메시지에서 `pending_message` 같은 두 번째 전체 사본을 다시 만들지 않아야 한다. |
| 포트 파싱 검증 | 포트와 zone id는 전체 숫자 소비와 범위 검사를 유지해야 한다. |
| IPC bind unlink 순서 | 검증 전에 임의 파일 경로를 unlink하지 않아야 한다. |
| decoder/message/send guard | NULL, 닫힌 메시지, 크기 산술 overflow 방어를 제거하지 않아야 한다. |
| `maxmsgsize` 정책 | 큰 메시지 호환성에 영향을 주는 기본값 변경은 성능 라운드에 섞지 않는다. |

성능을 위해 위 의미를 완화하는 패치는 금지한다. 예를 들어 WS/WSS 처리량을 올리기 위해
`pending_message` 전체 사본을 되살리거나, send hot path 분기를 줄이기 위해 public message
guard를 제거하는 방식은 허용하지 않는다. 보안 수정이 실제 병목으로 확인되면 되돌리는 대신
같은 보안 의미를 유지하는 다른 구현을 설계한다.

각 작업 로그에는 아래 체크를 반드시 남긴다.

```markdown
## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
- 보안 의미를 유지한 근거:
- 추가로 실행한 회귀 테스트:
```

## 3. 기준선

현재 비교 기준은 아래 두 report다.

| 구분 | report | 설명 |
|------|--------|------|
| 과거 기준 | `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt` | 2026-05-13 full multi 기준 |
| 현재 문제 | `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt` | 2026-06-14 실패와 64B 회귀가 함께 보인 결과 |

2026-06-14 현재 공통 64B throughput 비교에서는 평균 `-15.6%`, 중앙값 `-14.9%`,
10% 이상 하락 항목 18개가 확인되었다. one-way 계열 평균 하락은 약 `-27.4%`,
echo 계열 평균 하락은 약 `-8.3%`다.

이 숫자는 실패가 섞인 report에서 나온 값이므로, 첫 라운드에서는 core 실패 수정 후
새 full report를 다시 만들고 그 결과를 작업 기준선으로 승격한다.

## 4. 목표 수치

성능 오차를 감안해 목표는 한 번에 과거 최고점을 완전히 복구하는 방식이 아니라,
재현 가능한 개선 폭으로 둔다. 이 계획의 주 목표는 특정 pattern 하나가 아니라
전체 64B 공통 항목의 전반적인 성능 향상이다.

1차 목표는 현재 문제 report 대비 전체 64B 공통 항목에서 약 `10%` 처리량 향상을 만드는
것이다. 단일 항목의 우연한 상승이 아니라, 여러 pattern과 transport에 걸쳐 중앙값과 평균이
함께 올라야 한다. 과거 기준 대비 완전 복구는 후속 목표로 둔다.

| 목표 | 완료 기준 |
|------|-----------|
| 실패 안정화 | full multi perf에서 실패 0개 |
| 1차 전체 64B 중앙값 | 현재 기준 대비 `+10%` 이상 |
| 1차 전체 64B 평균 | 현재 기준 대비 `+8%` 이상 |
| 1차 one-way 64B 평균 | 현재 기준 대비 `+10%` 이상 |
| 1차 echo 64B 평균 | 현재 기준 대비 `+5%` 이상 |
| 큰 회귀 항목 | 현재 기준 대비 `+10%` 이상 또는 과거 기준 대비 하락폭 `-10%` 이내 |
| 복구 목표 전체 64B 중앙값 | 과거 기준 대비 하락폭 `-5%` 이내 |
| 복구 목표 전체 64B 평균 | 과거 기준 대비 하락폭 `-8%` 이내 |

`MULTI_STREAM tcp 64B`는 중요한 관찰 항목이지만, 이 계획의 단독 목표가 아니다. stream
수치가 좋아져도 전체 64B 평균과 중앙값이 오르지 않으면 목표를 달성한 것으로 보지 않는다.
반대로 전체 hot path가 좋아졌는데 stream만 남으면 별도 라운드로 분리한다.

## 5. 우선순위

1. full multi perf 실패 제거
2. `MULTI_SPOT` 64B one-way 회귀 분석
3. `MULTI_PUBSUB` 64B one-way 회귀 분석
4. `MULTI_DEALER_DEALER` 64B one-way 회귀 분석
5. `MULTI_STREAM tcp 64B` 회귀 분석
6. `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`, SPOT echo 계열 확인
7. full multi perf 재측정과 목표 달성 판단

현재 숫자로는 one-way hot path 하락이 가장 크다. 따라서 stream 하나만 먼저 맞추지
않고, 공통 message, pipe, poller, fanout 경로에서 전체 64B 처리량을 올릴 수 있는
회귀를 찾는다.

## 6. 반복 실행 방식

각 개선 라운드는 goal 하나로 진행한다. goal에는 이번 라운드의 구체 목표와 종료 기준을
명확히 적는다.

예시:

```text
core 64B one-way hot path 회귀를 줄인다.
완료 기준: 공통 64B targeted set의 중앙값이 현재 기준 대비 +10% 이상,
core targeted tests 통과, 작업 로그 작성.
```

goal을 시작한 뒤에는 아래 순서를 따른다.

1. 작업 로그 파일을 `doc/plan/perf/core/log/` 아래에 만든다.
2. 기준 report, 현재 report, 실행 명령, git 상태를 기록한다.
3. 병목 가설을 두 가지 이상 적고, 먼저 검증할 가설을 고른다.
4. core 코드와 테스트를 읽어 실제 hot path를 확인한다.
5. 최소 변경으로 core를 수정한다.
6. core build와 관련 테스트를 실행한다.
7. targeted perf로 개선 여부를 확인한다.
8. 효과가 없으면 변경을 되돌리고 로그에 실패 근거를 남긴다.
9. 효과가 있으면 같은 계열의 인접 pattern 또는 transport로 재확인한다.
10. goal 완료 기준을 만족하면 full 또는 축소 full perf로 최종 검증한다.
11. 최종 상태, 남은 위험, 다음 goal 후보를 로그에 적는다.

goal은 완료 기준을 만족할 때만 완료로 본다. 단순히 시간이 오래 걸렸거나 일부 수치가
좋아졌다는 이유로 완료하지 않는다.

## 7. 측정 규칙

기본 full 측정은 `bindings/c/perf/run_benchmarks_multi.sh`를 사용한다.

core 수정 뒤에는 항상 아래 순서를 지킨다.

```bash
cmake --build core/build -j$(nproc)
bindings/c/perf/run_benchmarks_multi.sh --reuse-build ...
```

targeted perf는 원인 확인을 위해 범위를 줄일 수 있다. 예시는 아래와 같다.

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern MULTI_SPOT \
  --transports tcp \
  --msg-sizes 64 \
  --duration 5
```

측정 결과를 비교할 때는 아래 항목을 함께 확인한다.

- `META,commit`
- `META,load_avg`
- `Effective Options`
- `Perf runtime libzlink`
- `clients`
- `duration_seconds`
- `server_io_threads`
- `client_io_threads`
- `ctx_auto_hwm_enable`
- `Auto-HWM`의 `MsgUnit(B)`
- 실패 항목과 실패 원인

load가 baseline보다 높거나 다른 작업이 함께 돌고 있으면 재측정한다. 그래도 같은 방향의
차이가 반복될 때만 성능 판단에 사용한다.

## 8. core hot path 후보

우선 검토할 core 후보는 아래와 같다.

| 후보 | 관련 경로 | 확인할 질문 |
|------|-----------|-------------|
| message allocation/refcount | `core/src/runtime/core/msg.*`, `core/src/runtime/protocol/*` | 64B에서 allocation 또는 refcount 비용이 늘었는가 |
| pipe enqueue/dequeue | `core/src/runtime/core/pipe.*`, `session_base*` | one-way fanout에서 pipe 이동 비용이 늘었는가 |
| mailbox/wakeup | `core/src/runtime/core/mailbox.*`, `signaler.*`, `io_thread.*` | 작은 메시지에서 wakeup 빈도나 lock 비용이 늘었는가 |
| poller | `core/src/runtime/core/socket_poller.*`, `engine/asio/asio_poller.*` | 이벤트 처리 batch가 줄었는가 |
| ASIO read/write batching | `core/src/runtime/engine/asio/*` | batch target, gather, speculative write 정책이 실제 처리량을 낮추는가 |
| PUB/SUB matching | `core/src/runtime/sockets/pubsub*`, `mtrie*` | subscription matching 비용이 늘었는가 |
| SPOT fanout | `core/src/runtime/services/spot/*` | pubsub fanout, ready state, control path가 data path에 섞였는가 |
| STREAM routing-id send | `core/src/runtime/sockets/stream*`, `engine/asio/*` | routing id와 packet echo 경로에서 copy나 lock이 늘었는가 |

각 후보는 반드시 실제 call path를 읽고 검증한다. 추측만으로 변경하지 않는다.

## 9. 작업 로그 규칙

모든 라운드 기록은 `doc/plan/perf/core/log/` 아래에 둔다.

파일명은 아래 형식을 사용한다.

```text
YYYY-MM-DD-round-N-short-topic.ko.md
```

예시:

```text
2026-06-14-round-1-core-64b-baseline.ko.md
2026-06-14-round-2-spot-oneway-hotpath.ko.md
```

로그에는 최소한 아래 내용을 적는다.

```markdown
# 라운드 N: 제목

- goal:
- 시작 시각:
- 기준 commit:
- 시작 git status:
- 기준 report:
- 비교 report:
- 대상 pattern/transport/size:

## 가설

- 가설 1:
- 가설 2:
- 선택한 가설:

## 읽은 코드

- `path`: 확인한 내용

## 변경

- 변경 파일:
- 변경 이유:
- perf 전용 변경이 아닌 이유:

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
- 보안 의미를 유지한 근거:
- 추가로 실행한 회귀 테스트:

## 검증

- build:
- test:
- targeted perf:
- full perf:

## 결과

- 개선 전:
- 개선 후:
- delta:
- 목표 달성 여부:

## 다음 작업

- 남은 위험:
- 다음 goal 후보:
```

로그는 작업 중간에도 갱신한다. 마지막에 한 번에 회고처럼 쓰지 않는다. 실패한 가설과
되돌린 변경도 기록한다.

## 10. 완료 정의

이 계획은 아래 조건을 모두 만족하면 완료로 본다.

- full multi perf가 실패 없이 끝난다.
- 현재 기준 대비 64B 공통 항목 중앙값이 `+10%` 이상 오른다.
- 현재 기준 대비 64B 공통 항목 평균이 `+8%` 이상 오른다.
- 현재 기준 대비 one-way 64B 평균이 `+10%` 이상 오른다.
- 큰 회귀 항목은 현재 기준 대비 `+10%` 이상 개선되거나 과거 기준 대비 하락폭이
  `-10%` 이내로 들어온다.
- STREAM만 개선되고 전체 64B 평균과 중앙값이 오르지 않는 상태를 완료로 보지 않는다.
- core targeted tests와 관련 regression tests가 통과한다.
- `doc/plan/perf/core/log/`에 라운드별 작업 기록이 남아 있다.

완료 기준 중 일부가 구조적으로 불가능하다고 확인되면, 그 근거를 로그에 남기고 목표를
수정한다. 목표 수정은 측정 오차나 임시 실패를 이유로 하지 않는다. core 외부 조건,
perf 측정 구조 변화, 공개 계약상 유지해야 하는 비용처럼 구체적인 근거가 있어야 한다.

## 11. 비범위

- perf runner 기본값 변경
- perf client/server hot path 최적화
- 결과 계산식 변경
- benchmark 전용 core option 추가
- public API 의미 변경
- 실패를 숨기는 sleep, retry, timeout 완화
- 특정 report만 좋아지는 환경 변수 고정
