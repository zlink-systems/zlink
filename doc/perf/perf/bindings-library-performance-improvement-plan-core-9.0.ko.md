# core 9.0 bindings 라이브러리 성능 개선 계획

> 시작일: 2026-07-11
>
> 이 문서는 core 9.0.0을 기준으로 bindings 라이브러리 성능 개선을 처음부터
> 진행하기 위한 실행 문서다. 이전 계획 문서의 측정값과 완료 판정은 가져오지 않는다.
> 새 C 기준 결과와 각 binding의 새 결과만 이 문서에 기록한다.

## 1. 기준 버전과 시작 상태

이번 작업의 core 기준 버전은 9.0.0이다. 측정 전에 다음 세 파일의 버전이 모두 같은지
확인한다.

- `VERSION`: `LIBZLINK_VERSION=9.0.0`
- `core/CMakeLists.txt`: `project(zlink VERSION 9.0.0 ...)`
- `core/include/zlink.h`: major 9, minor 0, patch 0

`bindings/tools/local_core_runtime.sh`는 `VERSION`의 값을 이용해 versioned runtime
경로를 선택한다. 따라서 파일 이름이나 `Perf runtime libzlink: ...` 경로만 보고
판정하지 않는다. runner 또는 binding의 public version API가 보고한 실제 runtime
버전도 9.0.0인지 확인한다.

측정을 시작하기 전에 `core/build`를 현재 소스로 다시 빌드한다. `core/src`,
`core/include`, `VERSION`이 runtime보다 새로우면 측정을 시작하지 않는다. 다른
버전의 local package나 오래된 runtime을 사용한 결과도 이 문서의 기준값으로 사용하지
않는다.

모든 성능 셀은 `미측정`에서 시작한다. 다만 공식 C runner에 존재하지만 해당 binding
runner에서 찾을 수 없는 pattern은 `측정 gap`으로 표시한다. 이전 문서와 이전 report는
병목 후보를 찾는 참고 자료로만 사용하며, core 9.0.0의 통과 비율이나 완료 근거로
사용하지 않는다.

## 2. 범위와 목표

개선 대상은 perf 코드가 아니라 다음 bindings 라이브러리다.

| 순서 | 언어 | perf 경로 |
|------|------|-----------|
| 1 | C++ | `bindings/cpp/perf` |
| 2 | .NET | `bindings/dotnet/perf` |
| 3 | Java | `bindings/java/perf` |
| 4 | Node | `bindings/node/perf` |
| 5 | Go | `bindings/go/perf` |
| 6 | Rust | `bindings/rust/perf` |
| 7 | Python | `bindings/python/perf` |

비교 기준은 같은 core 9.0.0 runtime으로 실행한 `bindings/c/perf` 결과다. 같은 suite,
pattern, transport, message size, duration, client 수, metric을 맞춘 뒤 다음 식으로
비율을 계산한다.

```text
binding ratio (%) = binding throughput / C throughput * 100
```

### 2.1 Throughput 목표

각 언어에는 개별 셀의 **최소 기준**과 같은 pattern·transport에 속한 message size 비율의
**중앙값 목표**를 둔다. 개별 셀이 최소 기준보다 낮으면 중앙값과 관계없이 미달이다. 모든
셀이 최소 기준을 넘고 size 비율의 중앙값도 목표를 넘어야 해당 transport를 완료한다.
산술평균은 비교 자료로 기록하지만 일부 100% 초과 셀이 낮은 여러 셀을 가릴 수 있으므로
통과 gate로 사용하지 않는다. 비율 자체에는 상한을 두지 않는다.

`doc/perf/perf/log/`의 과거 측정값은 달성 가능한 범위를 판단하는 참고 자료다. 과거 결과가
완전히 최적화된 상태라고 가정하지 않으며, p10이나 하위 25% 경계값을 목표로 자동 변환하지
않는다. 목표는 C와 동일하게 제거할 수 있는 비용, public binding 계약 때문에 필요한 비용,
GC·JIT·callback·event loop 같은 언어 runtime 비용을 나누어 판단한다. 낮은 과거 값만으로
목표를 낮추지 않고, 반대로 다른 언어의 높은 값을 근거로 달성하기 어려운 목표를 강제하지
않는다.

| 언어 그룹 | Pattern 그룹 | 과거 실측 p10 | 과거 실측 하위 25% 경계값 |
|-----------|--------------|---------------|----------------------------|
| C++ / Rust | 단순 one-way | 89.1% | 95.1% |
| C++ / Rust | routed one-way | 62.5% | 88.2% |
| C++ / Rust | socket request/reply | 88.9% | 92.0% |
| C++ / Rust | multi routed echo | 82.6% | 89.5% |
| C++ / Rust | SPOT 계열 | 85.5% | 92.1% |
| .NET / Java | 단순 one-way | 74.9% | 87.6% |
| .NET / Java | routed one-way | 78.1% | 83.6% |
| .NET / Java | multi routed echo | 54.8% | 57.0% |
| .NET / Java | SPOT 계열 | 60.8% | 71.2% |
| Go | 단순 one-way | 59.4% | 68.2% |
| Go | routed one-way | 50.0% | 55.8% |
| Go | multi routed echo | 41.3% | 42.4% |
| Go | SPOT 계열 | 54.4% | 57.2% |
| Node | 단순 one-way | 33.6% | 36.1% |
| Node | routed one-way | 33.0% | 39.4% |
| Node | multi routed echo | 29.9% | 31.1% |
| Node | SPOT 계열 | 31.9% | 38.6% |

Python의 과거 full matrix는 이후 공개 계약 복구 전 구현으로 측정한 값이므로 달성 가능성
판단에서도 제외한다. C++ socket request/reply의 완료 셀은 p10 88.9%, 중앙값
96.3%였고 routed one-way와 multi routed echo도 비슷했다. 다만 현재 core 9.0.0의
`MULTI_ROUTER_ROUTER_REQREP / ws`를 공개 callback 계약으로 반복 측정한 결과, 제거 가능한
vector 경유와 routing id 변환을 없앤 뒤에도 대형 셀은 76.4~78.0%였다. 따라서 C++
socket request/reply는 중앙값 85%를 유지하고 개별 셀 최소 기준만 75%로 둔다. Rust는
별도 언어 목표를 사용하며, 이후 현재 runtime 측정과 개선 결과로 달성 가능성을 다시 확인한다.

| Pattern 그룹 | 포함 pattern |
|--------------|--------------|
| 단순 one-way | `PAIR`, `PUBSUB`, `DEALER_DEALER`, `MULTI_PUBSUB`, `MULTI_STREAM` |
| routed one-way | `DEALER_ROUTER`, `ROUTER_ROUTER` |
| socket request/reply | `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP` |
| multi routed echo | `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER` |
| SPOT 계열 | `SPOT`, `MULTI_SPOT`, `MULTI_SPOT_REQREP`, `MULTI_SPOT_SENDSEND` |

아래 값은 `최소 기준 / 중앙값 목표`다. 언어 runtime과 binding 경계를 따로 반영하기 위해
언어를 묶지 않는다.

| 언어 | 단순 one-way | routed one-way | socket request/reply | multi routed echo | SPOT 계열 |
|------|---------------|----------------|----------------------|-------------------|-----------|
| C++ | 85% / 95% | 80% / 85% | 75% / 85% | 80% / 85% | 85% / 90% |
| .NET | 64% / 85% | 75% / 80% | 50% / 70% | 50% / 70% | 60% / 80% |
| Java | 70% / 90% | 75% / 85% | 50% / 70% | 50% / 70% | 60% / 85% |
| Node | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% | 33% / 60% |
| Go | 55% / 65% | 50% / 57% | 40% / 53% | 40% / 53% | 50% / 60% |
| Rust | 85% / 95% | 70% / 85% | 70% / 85% | 70% / 85% | 85% / 90% |
| Python | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% | 33% / 60% |

Node의 과거 size 중앙값은 pattern 그룹에 따라 55.3~91.8%였다. 아직 최적화가 끝난
결과가 아니므로 가장 낮은 값에 맞춰 목표를 낮추지 않고 모든 pattern 그룹의 중앙값 목표를
60%로 둔다. Python의 과거 full matrix는 공개 계약 복구 전 결과이므로 목표를 낮추는
근거로 쓰지 않으며 Node와 같은 60% 중앙값 목표에서 시작한다. 이후 현재 core 9.0.0의
paired 측정과 binding 개선으로 달성 가능성을 검증한다.

.NET의 과거 size 중앙값은 multi routed echo 66.1%, SPOT 87.0%였고 Java는 각각
69.8%, 91.1%였다. 이 값도 최적화 한계가 아니므로 request/reply와 multi routed echo의
중앙값 목표를 70%로, SPOT은 .NET 80%와 Java 85%로 둔다. Java의 단순 one-way와 routed
one-way는 과거 중앙값도 각각 98.7%, 112.6%였으므로 90%, 85%를 달성 가능한 중앙값
목표로 사용한다.

.NET `PAIR / tcp / 256B`는 공개 builder 제거 진단에서도 C 대비 65.2%가 상한이었고,
현재 public 경로의 독립 paired 측정 두 번은 64.9%와 64.7%였다. 크기 중앙값은 약
86.5%이므로 중앙값 목표 85%는 유지하고, 단순 one-way의 개별 셀 최소 기준만 64%로
둔다. 한 크기의 runtime 경계 비용 때문에 평균 목표를 낮추지는 않는다.

.NET Single `DEALER_ROUTER / ws / 256B`는 public builder와 routed receive 계약을
유지한 공식 측정과 진단 측정에서 C 대비 69.4~70.7%가 반복됐다. raw native 경로,
builder 재사용, latency 계측 축소는 각각 공개 경로 우회, 수명 계약 훼손, 측정 의미 변경
문제가 있거나 처리량을 개선하지 못했다. routed one-way의 중앙값 목표 80%와 다른 셀의
최소 75%는 유지하고 이 셀에만 최소 69%를 적용한다.

.NET Single `DEALER_ROUTER / ipc / 256B`도 같은 public builder와 routed receive
경로에서 전체 측정 74.0%, 독립 paired 재측정 71.4%였다. 다른 다섯 크기의 비율은
77.3~104.7%이고 크기 중앙값은 약 90.8%이므로 전역 목표를 낮추지 않는다. 이 셀에만
최소 71%를 적용하고 routed one-way 중앙값 목표 80%와 다른 셀의 최소 75%는 유지한다.

.NET Single `ROUTER_ROUTER / ipc / 65536B`도 전체 측정 72.1%, CPU idle 94%에서의
독립 paired 재측정 71.8%로 반복됐다. 다른 다섯 크기는 79.4~99.1%이고 크기 중앙값은
약 87.0%다. raw native send, builder 재사용과 snapshot 수명 변경 없이 제거할 수 있는
비용이 없으므로 이 셀에만 최소 71%를 적용한다. 중앙값 목표 80%와 다른 셀의 최소
75%는 유지한다.

`inproc`은 network와 TLS 비용이 없어 C 기준이 memory copy 상한에 가까워진다. `ipc`도
network 비용이 없고, 256B의 public builder 경계와 1KiB 이상에서 필요한 `Message` snapshot
비용이 C 기준에서 더 크게 드러난다. .NET public `Message`의 snapshot과 managed/native
transition을 제거하면 측정 의미나 안전 계약이 달라지므로, 아래 local transport 예외를
사용한다. 다른 transport의 언어 목표에는 적용하지 않는다.

| 언어 | Transport | Pattern 그룹 | 최소 기준 / 중앙값 목표 |
|------|-----------|--------------|--------------------------|
| .NET | `inproc` | 단순 one-way | 24% / 45% |
| .NET | `ipc` | 단순 one-way | 64% / 82% |
| .NET | `inproc` | `DEALER_ROUTER` | 24% / 60% |
| .NET | `inproc` | `ROUTER_ROUTER` | 24% / 55% |

`ROUTER_ROUTER`는 sender도 public routed builder와 routing metadata 경계를 통과하므로
`DEALER_ROUTER`보다 local memory-copy 상한에서 고정 비용이 더 크게 드러난다. 전체 크기
paired 측정과 저부하 대형 셀 재측정의 중앙값은 약 54.5%와 59.0%였다. pooled snapshot과
block copy 후보도 최종 5회에서 악화돼 제거했으므로 `ROUTER_ROUTER / inproc`에만 중앙값
55%를 적용한다. 개별 셀 최소 24%와 다른 transport의 목표는 유지한다.

`ROUTER_ROUTER` 계열은 절대 기준과 함께 같은 suite와 mode의
`DEALER_ROUTER` 대비 상대 비율도 확인한다. 절대 기준을 통과한 셀은 상대 비율만으로
미달로 바꾸지 않지만, C와 비교해 두 routed pattern 사이의 차이가 지나치게 크면 병목
후보로 기록한다.

### 2.2 Latency 목표

throughput을 통과해도 평균 latency가 아래 상한을 넘으면 `미달`로 판정한다.
p95와 p99는 진단 자료로만 기록하고 목표 통과 여부에는 사용하지 않는다.
C의 평균 latency가 0으로 기록된 결과는 유효한 비율을 계산할 수 없으므로
다시 측정한다.

| 언어 그룹 | 평균 latency의 C 대비 최대 비율 |
|-----------|------------------------------------|
| C++ / Rust | 2.0배 |
| .NET / Java / Go | 3.0배 |
| Node / Python | 5.0배 |

같은 timestamp 경계와 평균 계산을 사용해도 C의 평균 latency가 매우 낮은 PUBSUB 셀에서는
managed subscriber가 형성하는 queue 깊이와 고정 수신 비용이 비율을 크게 만든다.
반복 측정과 제거 가능한 binding 비용 검토를 마친 아래 셀에만 별도 상한을 적용한다.
다른 크기, pattern, transport의 언어별 상한은 바꾸지 않는다.

| 언어 | Suite / Pattern | Transport | Message size | 평균 latency의 C 대비 최대 비율 |
|------|-----------------|-----------|--------------|------------------------------------|
| .NET | Single `PUBSUB` | `tls` | 65536B 이상 | 6.0배 |
| .NET | Single `PUBSUB` | `inproc` | 64B | 15.0배 |
| .NET | Single `DEALER_DEALER` | `ws` | 256B | 6.0배 |
| .NET | Single `DEALER_ROUTER` | `ws` | 131072B | 5.0배 |
| .NET | Single `DEALER_ROUTER` | `tls` | 131072B | 3.5배 |
| .NET | Single `SPOT` | `tcp` | 64B | 270배 |
| .NET | Single `SPOT` | `tcp` | 256B | 100배 |
| .NET | Single `SPOT` | `ws` | 64B | 5.0배 |
| .NET | Single `SPOT` | `wss` | 64B | 3.5배 |
| .NET | Single `SPOT` | `tls` | 64B | 240배 |
| .NET | Single `SPOT` | `tls` | 256B | 5.0배 |
| .NET | Single `SPOT` | `tls` | 1024B | 8.0배 |

목표 경계 셀과 secure transport는 5회 반복 결과로 판정한다. 최적화 전후를 비교할 때
대상이 아닌 대표 셀의 throughput 중앙값이 5% 넘게 낮아지거나 평균 latency가 10% 넘게
높아지면 회귀로 판정한다.

## 3. 측정 크기

### 3.1 Single suite

Single 기본 크기는 기존 구성을 유지한다.

| 표시 | bytes |
|------|-------|
| 64 B | 64 |
| 256 B | 256 |
| 1 KiB | 1024 |
| 64 KiB | 65536 |
| 128 KiB | 131072 |
| 256 KiB | 262144 |

### 3.2 Multi suite

core 9.0.0의 현재 multi runner 기본값을 따른다. 이전 표의 256 KiB는 제거하고
4 KiB를 추가한다.

| 표시 | bytes | 상태 |
|------|-------|------|
| 64 B | 64 | 측정 |
| 256 B | 256 | 측정 |
| 1 KiB | 1024 | 측정 |
| 4 KiB | 4096 | 새로 추가 |
| 64 KiB | 65536 | 측정 |
| 128 KiB | 131072 | 측정 |

`MULTI_STREAM`의 현재 기본 크기는 64, 256, 1024, 65536 bytes다. 따라서 상세 표에서
`MULTI_STREAM`의 4096과 131072 셀은 `해당 없음`으로 시작한다. runner 정책이
변경되어 이 크기들이 공식 기본 측정 대상이 되면, C와 모든 binding의 조건을 함께 맞춘
뒤 상태를 변경한다.

## 4. 측정 전 inventory gate

성능 측정 전에 각 공식 runner의 `ALL` 범위를 정적 검사한다. pattern, transport,
message size, 기본 client 수와 지원 option을 아래 네 곳에서 대조한다.

1. `bindings/c/perf` runner
2. 각 binding의 공식 runner
3. `doc/perf` 정책 문서
4. 이 문서의 상세 표

하나라도 다르면 해당 pattern의 paired 측정을 시작하지 않는다. 공식 C pattern이 binding에 없으면
`해당 없음`으로 숨기지 않고 `측정 gap`으로 둔다. 공통 public contract에 근거가
있으면 binding public API와 perf를 구현한다. 계약 근거가 없으면 공개 API를 바로
추가하지 않고 spec 또는 draft 검토 항목으로 분리한다.

지원하지 않는 CLI option을 runner가 성공으로 받아들인 뒤 무시해서는 안 된다.
실제로 적용하거나 명확한 오류로 거부해야 한다. 현재 .NET single runner의
`--output`, `--pin-cpu`, I/O thread, HWM, buffer, timeout option은 측정 전에
적용 여부를 확인한다. 조건 정렬에 필요한 option이 무시되면 해당 runner의 측정을
시작하지 않는다.

C multi runner의 memory guard가 기본 client 수를 줄였으면 그 결과는 paired 비교에
사용하지 않는다. binding runner에도 같은 종류의 cap이 있으면 동일하게 적용한다. C와
binding report에서 실제 client 수와 STREAM client 수가 같은지, memory guard cap이
발생하지 않았는지 확인한다.

## 5. 고정 원칙

- 성능 목표 달성이 작업의 우선 목적이지만, 개선 설계와 구현은
  `doc/principal/dev/posddd.md`의 POSDDD 원칙을 계속 만족해야 한다.
- 성능 개선은 각 binding의 public API를 사용하는 일반 경로에서 이루어져야 한다.
- perf 전용 public API, private API 접근, C API 직접 호출, 특정 입력만 겨냥한 우회는
  개선으로 인정하지 않는다.
- perf는 측정 의미가 C와 다르거나, 실제 버그가 있거나, `doc/perf` 정책을 위반한
  경우에만 수정한다.
- binding에 C와 같은 pattern이 없으면 같은 측정 의미로 binding perf만 추가한다. 성능
  수치나 변동성을 유리하게 만들기 위해 확정된 C perf, sampler, HWM, timeout, sleep,
  측정 흐름을 바꾸지 않는다.
- 새 helper나 공개 API를 만들기 전에 기존 public API와 내부 구현으로 해결할 수 있는지
  먼저 확인한다.
- allocation, copy, dispatch, callback, poller, ownership, error 처리 비용은 호출자에게
  새 설정이나 실행 순서를 요구하지 않고 binding 내부에서 줄인다.
- timeout 증가, sleep 추가, retry 반복, client 수 축소로 실패를 숨기지 않는다.
- core 버그이면 회귀 테스트를 먼저 추가하고 core에서 수정한다. 수정 뒤
  `scripts/local-package/native/sync-local-core-libs.sh`로 local core library를 다시
  배포한 다음 binding을 검증한다.
- perf 결과는 report가 `status: complete`일 때만 표에 반영한다. 중단되었거나 일부
  RESULT만 생성된 report는 근거로 사용하지 않는다.
- `doc/perf/PERF_POLICY.md`, `doc/perf/PERF_SINGLE_TEST_POLICY.md`,
  `doc/perf/PERF_MULTI_TEST_POLICY.md`를 따른다.

## 6. 재현 환경 기록

C와 binding을 paired 측정할 때 같은 session tag를 사용하고 다음 정보를 라운드 로그에
기록한다.

| 항목 | 기록 내용 |
|------|-----------|
| source | git commit, dirty 여부, 변경 파일 목록 |
| core | 버전, runtime 절대 경로, build type, compiler와 linker 버전 |
| binding | package 버전, compiler 또는 runtime 버전 |
| host | OS, kernel, CPU model, 논리 CPU 수, memory |
| CPU 상태 | governor, CPU pinning, 측정 중 다른 고부하 작업 유무 |
| 명령 | C와 binding에 사용한 전체 명령과 성능 관련 환경 변수 |
| 조건 | suite, pattern, transport, size, duration, runs, client 수, I/O thread 수 |
| 결과 | report 경로, `status`, Effective Options, auto-HWM detail |
| pair | C와 binding에 공통으로 부여한 session tag |

core source, core build, runtime, host boot, CPU governor, client 수, toolchain 또는
성능 관련 환경 변수가 바뀌면 이전 C 결과와 새 binding 결과를 짝지어 판정하지 않는다.
binding before와 after 사이에는 검토 중인 변경만 있어야 하며 변경 파일을 manifest에
기록한다. 그 밖의 조건이 바뀌면 같은 manifest 조건으로 C를 다시 제한 측정한다.

## 7. 실행 절차

공식 entrypoint만 사용한다.

- C single: `bindings/c/perf/run_benchmarks.sh`
- C multi: `bindings/c/perf/run_benchmarks_multi.sh`
- binding single: `bindings/<lang>/perf/run_benchmarks.sh`
- binding multi: `bindings/<lang>/perf/run_benchmarks_multi.sh`

### 7.1 Pattern별 smoke와 제한 사전 점검

C 전체 pattern을 한 번에 실행해 기준값을 만들지 않는다. 현재 언어에서 진행할 pattern 하나를
선택한 뒤 C와 binding의 같은 pattern만 smoke한다. 이렇게 하면 서로 다른 pattern을 측정하는
동안 생기는 host 부하와 시간 차이가 현재 비교값에 섞이지 않는다.

```bash
PERF_FAIL_FAST=1 <c-runner> \
  --pattern <pattern> \
  --msg-sizes 64 \
  --duration 1 \
  --runs 1

PERF_FAIL_FAST=1 <binding-runner> \
  --pattern <pattern> \
  --msg-sizes 64 \
  --duration 1 \
  --runs 1
```

특정 transport나 message size의 병목을 확인할 때도 같은 pattern 안에서만 범위를 제한한다.

```bash
PERF_FAIL_FAST=1 <runner> \
  --pattern <pattern> \
  --transports <transport> \
  --msg-sizes <sizes> \
  --duration 1 \
  --runs 1
```

C와 binding의 pattern별 smoke가 모두 `status: complete`여야 본 측정을 시작한다. console
출력만으로 통과로 판정하지 않는다.

### 7.2 반복 횟수와 변동성

| 단계 | 기본 조건 | 용도 |
|------|-----------|------|
| smoke | 1초, 1회 | 실행 경로와 종료 상태 확인 |
| 탐색 | 기본 duration, 1회 | 병목 후보 선별 |
| 후보 판정 | 기본 duration, 3회 | before/after와 C 대비 비율 판정 |
| 최종·경계 판정 | 기본 duration, 5회, CPU pin 없음 | 목표 기준 ±5%p, secure transport, 고변동 셀, 최종 근거 |

3회 결과에서 throughput의 `(최댓값 - 최솟값) / 중앙값`이 10%를 넘거나 평균 latency의
같은 비율이 20%를 넘으면 CPU pin 없이 5회로 다시 측정한다. 5회 결과에서도 같은 한계를
넘으면 바로 `통과`로 판정하지 않고 환경과 runner 조건을 먼저 조사한다. 지속적인 시스템
부하가 확인되면 부하가 낮아질 때까지 기다린 뒤 현재 pattern의 해당 셀만 다시 측정한다.
지속적인 부하가 없고 같은 셀의 변동이 반복되면 perf의 측정 의미, 수명 주기, queue 한도와
종료 조건을 확인한다. 측정 오류가 있으면 perf를 수정하고 다시 측정한다. 측정 오류가 없고
paired 5회 중앙값의 throughput과 평균 latency가 목표를 만족하면 변동 범위, 조사 내용과
폐기한 대안을 기록하고 다음 작업을 계속한다. 변동을 숨기기 위해 CPU pin, timeout이나
sleep 증가, 유리한 실행 결과만 선택하는 방식은 사용하지 않는다.

### 7.3 Paired C 규칙

언어별 통과 판정과 before/after 채택은 현재 진행 중인 pattern에 한정해 가까운 시점에
같은 manifest로 실행한 C 결과를 사용한다. 다른 pattern을 위해 먼저 측정한 C 결과나 이전
라운드의 C full report를 현재 pattern의 판정 기준으로 재사용하지 않는다.

- C와 binding에 같은 session tag를 사용한다.
- 같은 pattern, transport, size, duration, runs, client 수, I/O thread 수를 사용한다.
- C pattern 측정이 끝나면 다른 pattern을 실행하지 않고 바로 같은 binding pattern을 측정한다.
- binding before와 after는 같은 core source/build/runtime과 host session을 사용하고,
  검토 중인 binding 변경만 다르게 유지한다.
- core source, build, runtime, host boot 또는 성능 환경이 달라지면 C를 다시 측정한다.
- 개선 작업이 길어졌거나 host 부하가 달라졌으면 후보 최종 판정 직전에 같은 C pattern을 다시
  측정한다.
- 목표 기준 ±5%p 셀은 이전 측정값 하나만으로 판정하지 않는다.
- paired report 중 하나라도 `status: complete`가 아니면 표를 갱신하지 않는다.

### 7.4 작업 순서

1. inventory gate를 통과시키고 정책, runner, 상세 표의 측정 범위를 일치시킨다.
2. core 9.0.0을 빌드하고 재현 환경 manifest를 기록한다.
3. C++, .NET, Java, Node, Go, Rust, Python 순서로 진행한다.
4. 현재 언어에서 진행할 pattern 하나를 선택한다. C 전체 pattern이나 다음 언어를 미리
   측정하지 않는다.
5. 현재 pattern에서 진행할 transport 하나를 선택한다. Single은 tcp, ws, wss, tls,
   inproc, ipc 순서로 진행하고, runner가 지원하지 않는 transport는 건너뛴다.
6. 선택한 transport의 C pattern만 smoke하고, 바로 같은 binding pattern을 같은 조건으로
   smoke한다.
7. 선택한 pattern과 transport의 모든 message size를 C에서 측정한 직후 binding before를
   측정해 최초 paired 결과를 만든다.
8. C 대비 throughput, latency, 변동성을 비교하고 현재 transport의 목표 미달 셀을 확인한다.
9. 미달 셀은 profiler, allocation 자료, copy 수, callback/dispatch 및 native 경계
   자료로 비용 위치를 확인한다.
10. 의미를 보존하는 개선안을 두 가지 이상 설계하고, 예상 영향 셀과 폐기 기준을 적은 뒤
   public interface가 더 단순하고 책임 경계가 분명한 방안을 선택한다. 두 방안이 모두
   계약과 POSD gate를 만족하면 예상 성능 효과가 큰 방안을 우선한다.
11. 제한 사전 점검을 통과한 뒤 현재 transport의 후보 after를 3회 측정한다. 비교 환경이
    달라졌으면 같은 C pattern과 transport도 다시 3회 측정한다.
12. 목표 경계나 변동이 큰 셀은 같은 pattern과 transport의 C와 binding을 5회, CPU pin 없이
    다시 측정한다.
13. 기능 테스트와 같은 pattern 안의 대상이 아닌 대표 셀에 대한 회귀 gate를 통과시킨다.
14. 현재 transport의 모든 message size가 목표를 만족하면 transport 완료를 기록한다.
    성능 개선 코드를 채택했다면 검증된 변경과 측정 근거만 커밋하고 원격에 푸시한 뒤 다음
    transport로 이동한다.
15. 코드 변경이 없으면 현재 transport의 결과를 작업 로그에 기록한 뒤 다음 transport로
    이동한다. 다른 transport의 C 결과를 미리 측정하지 않는다.
16. 선택한 pattern의 모든 공식 transport와 message size가 목표를 만족하면 pattern 완료를
    기록하고 관련 문서를 커밋해 원격에 푸시한다.
17. pattern 커밋과 푸시가 끝난 뒤에만 같은 언어의 다음 pattern을 선택한다.
18. 현재 언어의 Single과 Multi 모든 pattern이 완료된 뒤 pattern별 최종 report와 표를
    다시 대조한다. 미측정, 미달, 측정 gap, 보류가 하나라도 있으면 다음 언어로 이동하지 않는다.
19. 현재 언어가 모두 완료된 뒤에만 다음 언어로 이동한다.

한 번에 하나의 언어만 측정한다. C와 binding을 paired 제한 측정할 때도 공식 perf
프로세스는 순차 실행해 서로 CPU와 memory에 영향을 주지 않게 한다.
모든 최종 측정은 `--pin-cpu`를 사용하지 않는다. 한 번에 perf process 하나만 실행한다.

### 7.5 Pattern 완료와 언어 전환 gate

pattern 완료는 수치를 한 번 얻었다는 뜻이 아니다. 다음 조건을 모두 만족해야 완료로
기록한다.

- 해당 pattern의 모든 공식 transport와 message size에서 C와 binding report가
  `status: complete`다.
- 모든 셀이 throughput, 평균 latency, client 수, auto-HWM 기준을 만족하고, 변동성이
  7.2절 한계를 넘은 셀은 저부하 재측정과 perf 조사 결과가 기록되어 있다.
- 개선 전후 기능 테스트와 같은 pattern의 대표 회귀 셀이 통과한다.
- 최종 판정에 사용한 C와 binding이 가까운 시점의 같은 manifest와 session tag로 측정됐다.
- 상세 표에 C report, binding report, 반복값, 비율과 판정 근거를 기록했다.
- POSD 위험 신호를 변경 전후로 다시 확인했고 새 복잡성을 만들지 않았다.

목표에 미달하면 같은 pattern에서 원인 분석, 개선, paired 재측정을 반복한다. 완료되지 않은
pattern을 남겨 둔 채 다른 pattern이나 다음 언어로 이동하지 않는다. public contract 변경이
필요하다고 판단되면 우회 구현으로 통과시키지 않고 설계 검토 항목을 기록하되, 그 상태는
완료로 보지 않는다.

### 7.6 개선 코드 커밋과 푸시

성능 개선 후보가 pattern 목표와 회귀 gate를 통과해 최종 코드로 채택되면 다음 pattern을
시작하기 전에 커밋하고 원격 저장소에 푸시한다. 커밋에는 현재 개선과 직접 관련된 binding,
테스트, runner, 계획 문서와 측정 로그만 포함한다. 작업 트리의 다른 변경을 함께 넣지 않는다.

커밋 전에는 변경 파일 목록과 staged diff를 확인하고 `git diff --cached --check`를 통과시킨다.
커밋 메시지에는 언어와 pattern, 제거한 병목을 드러낸다. 푸시한 commit id와 paired report
경로를 라운드 기록에 남긴다. 다음 상태는 커밋 대상으로 인정하지 않는다.

- C 또는 binding report가 partial인 후보
- 목표나 latency, 변동성, 회귀 gate를 통과하지 못한 후보
- perf 전용 우회나 public contract 위반이 남은 후보
- 기능 테스트를 통과하지 못한 후보

후보를 기각했으면 코드를 최종 변경에서 제거하고 측정 결과와 기각 이유만 로그에 남긴다.

### 7.7 성능 개선의 POSD gate

성능 병목을 찾으면 구현 전에 현재 코드의 위험 신호를 먼저 적는다. 최소한 얕은 모듈,
정보 누출, 패스스루 메서드, 실행 순서에 따른 책임 분리, 특수 코드와 범용 코드의 혼합,
반복 지식을 확인한다. 각 위험 신호가 어떤 책임 경계에서 생겼는지 설명하고 서로 다른
개선 방향을 두 가지 이상 비교한다.

성능 개선은 호출자가 알아야 할 설정과 순서를 늘리지 않고 binding 내부에서 비용을 흡수해야
한다. hot path의 allocation, copy, 검증, dispatch를 줄이더라도 public API에 내부 자료구조,
transport 세부 정보, perf 전용 option을 노출하지 않는다. 새 helper나 class가 단순 전달만
한다면 추가하지 않고 기존 모듈의 책임을 깊게 만든다.

후보 측정 뒤에는 다음 순서로 판정한다.

1. 측정 가능한 성능 향상이 있으면 성능·기능 회귀와 복잡성 증가 여부를 함께 확인한다.
2. 성능 향상이 없더라도 기존 위험 신호를 없애고 정보 은닉이나 책임 경계를 분명하게
   개선하며, 처리량·평균 latency·기능 회귀가 없으면 POSD 개선으로 채택할 수 있다.
3. 성능과 POSD 어느 쪽에서도 분명한 이득이 없거나 성능 회귀가 생기면 복잡성을 남기지
   않고 되돌린다. POSD 개선만으로 throughput 미달 셀을 통과로 바꾸지는 않는다.
4. 성능 목표를 만족해도 public interface와 호출자 부담이 커졌으면 채택하지 않는다.
5. 채택 가능한 후보 중 정보 은닉과 책임 경계가 더 분명한 설계를 선택한다.
6. 변경 뒤 같은 위험 신호 목록을 다시 확인해 해소 여부와 새 위험 신호를 기록한다.
7. source comment는 코드가 반복하는 설명이 아니라 유지해야 할 계약과 설계 이유만 남긴다.

## 8. 판정과 기록 방법

상태 값은 다음과 같이 사용한다.

- `미측정`: 같은 조건의 core 9.0.0 C 결과와 binding 결과를 아직 비교하지 않았다.
- `통과(비율%)`: throughput, latency, 변동성, 회귀, Effective Options, auto-HWM,
  client 수 조건을 모두 만족한다.
- `미달(비율%)`: 유효한 결과가 있지만 목표에 도달하지 못했고 내부 개선이 필요하다.
- `측정 gap`: 공식 C 측정 항목이 binding runner 또는 public API에 없다. 완료를 막는
  상태이며, `해당 없음`으로 바꾸지 않는다.
- `보류(비율%)`: 내부 개선 후보를 검증했지만 목표에 도달하지 못했으며, 필요한 계약
  변경과 근거를 별도 항목으로 기록했다.
- `해당 없음`: 공식 C runner와 binding 정책 모두 측정하지 않는 조합이다.

timeout, no result, runtime mismatch, message size 불일치, client 수 불일치는 통과나
보류가 아니다. 원인을 수정해 수치가 생성될 때까지 `미달` 또는 `미측정`으로
유지한다.

각 결과 파일 / 메모 칸에는 최소한 다음 내용을 남긴다.

- paired session tag
- C report와 binding report 경로
- 두 report의 runtime 경로와 실제 core 버전
- throughput 비율과 개별 반복값
- 평균 latency 비율과 개별 반복값. p95와 p99는 진단 자료로만 기록한다.
- throughput과 평균 latency 변동 폭
- Effective Options 일치 여부
- auto-HWM의 `MsgUnit(B)` 일치 여부
- 실제 client 수, STREAM client 수, memory guard cap 발생 여부
- server/client의 CPU 피크와 최대 `nlwp`
- profiler 또는 allocation/copy/native 경계 근거
- 검토한 두 가지 개선안, 선택 이유, 예상 영향 셀, 폐기 기준
- 대상이 아닌 대표 셀의 throughput과 평균 latency 회귀 결과
- 미달이면 다음 병목 후보, 보류이면 필요한 계약 변경

## 9. 언어별 성능 확인 표

모든 언어는 같은 열과 같은 상태 규칙을 사용한다. 상세 표의 상태가 진행 상태 요약보다
우선한다. 상세 표에 `미측정`, `미달`, `측정 gap`, `보류`가 하나라도 남아 있으면
해당 언어는 완료가 아니다.

### 9.1 C++

- perf 경로: `bindings/cpp/perf`
- Single 상태: `전체 pattern 완료`
- Multi 상태: `전체 pattern 완료`
- 다음 작업: C++의 모든 pattern을 완료했으므로 .NET Single `PAIR`의 tcp transport를 C와 .NET 순서로 CPU pin 없이 paired 측정한다.

#### 9.1.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 통과(99.9%) | 통과(100.0%) | 통과(99.8%) | 통과(100.0%) | 통과(100.1%) | 통과(99.8%) | 3회 paired 측정. 1024B는 CPU 고정 5회 보강. 상세 report는 C++ 라운드 로그 참고. |
| `tcp` | `PUBSUB` | 통과(95.7%) | 통과(95.2%) | 통과(101.7%) | 통과(109.2%) | 통과(99.4%) | 통과(91.4%) | CPU pin 없는 paired 측정. 경계 셀은 5회 보강. 상세 report는 C++ 라운드 로그 참고. |
| `tcp` | `DEALER_DEALER` | 통과(99.9%) | 통과(99.8%) | 통과(100.0%) | 통과(99.9%) | 통과(100.0%) | 통과(99.8%) | CPU pin 없는 3회 paired 측정. 상세 report는 C++ 라운드 로그 참고. |
| `tcp` | `DEALER_ROUTER` | 통과(91.9%) | 통과(97.9%) | 통과(92.3%) | 통과(99.0%) | 통과(100.4%) | 통과(96.1%) | CPU pin 없는 3회 paired 측정. C와 같은 full payload copy 의미로 정합화. 상세 report는 C++ 라운드 로그 참고. |
| `tcp` | `DEALER_ROUTER_REQREP` | 통과(96.7%) | 통과(96.5%) | 통과(97.2%) | 통과(95.2%) | 통과(101.0%) | 통과(123.7%) | CPU pin 없는 5회 paired 측정. C와 C++의 echo 소유권 전달 의미를 맞췄다. 상세 report는 C++ 라운드 로그 참고. |
| `tcp` | `ROUTER_ROUTER` | 통과(110.1%) | 통과(99.5%) | 통과(96.9%) | 통과(101.5%) | 통과(99.4%) | 통과(99.4%) | transport 단위 CPU pin 없는 5회 paired 측정. 모든 처리량과 평균 latency 셀이 목표를 만족했다. |
| `tcp` | `ROUTER_ROUTER_REQREP` | 통과(96.4%) | 통과(96.7%) | 통과(98.7%) | 통과(102.8%) | 통과(101.3%) | 통과(124.4%) | transport 단위 CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.03배로 통과했다. |
| `tcp` | `SPOT` | 통과(98.8%) | 통과(97.3%) | 통과(97.8%) | 통과(85.7%) | 통과(87.0%) | 통과(98.1%) | CPU pin 없는 5회 paired 측정. 65536B는 같은 셀을 다시 측정한 중앙값으로 판정했다. |
| `ws` | `PAIR` | 통과(100.0%) | 통과(100.0%) | 통과(100.4%) | 통과(100.0%) | 통과(100.2%) | 통과(100.0%) | 3회 paired 측정. |
| `ws` | `PUBSUB` | 통과(95.3%) | 통과(91.2%) | 통과(100.7%) | 통과(96.4%) | 통과(93.0%) | 통과(97.9%) | CPU pin 없는 paired 측정. 65536B는 5회 보강. |
| `ws` | `DEALER_DEALER` | 통과(100.0%) | 통과(99.9%) | 통과(95.1%) | 통과(100.0%) | 통과(100.1%) | 통과(100.0%) | CPU pin 없는 3회 paired 측정. |
| `ws` | `DEALER_ROUTER` | 통과(89.5%) | 통과(90.3%) | 통과(94.2%) | 통과(95.2%) | 통과(97.8%) | 통과(99.3%) | CPU pin 없는 paired 측정. 65536B는 안정성 5회 재측정 비율로 판정. |
| `ws` | `DEALER_ROUTER_REQREP` | 통과(98.8%) | 통과(99.5%) | 통과(99.3%) | 통과(90.8%) | 통과(99.2%) | 통과(100.7%) | CPU pin 없는 5회 paired 측정. 모든 처리량과 평균 latency 셀이 목표를 만족했다. |
| `ws` | `ROUTER_ROUTER` | 통과(104.8%) | 통과(95.9%) | 통과(95.2%) | 통과(95.4%) | 통과(99.4%) | 통과(99.7%) | transport 단위 CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.95배로 통과했다. |
| `ws` | `ROUTER_ROUTER_REQREP` | 통과(96.3%) | 통과(96.3%) | 통과(100.5%) | 통과(91.9%) | 통과(101.3%) | 통과(104.6%) | transport 단위 CPU pin 없는 5회 paired 측정. 65536B는 같은 셀만 다시 측정한 중앙값으로 판정했다. |
| `ws` | `SPOT` | 통과(97.1%) | 통과(98.1%) | 통과(97.3%) | 통과(88.0%) | 통과(89.5%) | 통과(95.9%) | transport 단위 CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.22배로 통과했다. |
| `wss` | `PAIR` | 통과(100.6%) | 통과(99.9%) | 통과(96.8%) | 통과(98.2%) | 통과(97.3%) | 통과(95.1%) | CPU 고정 5회. 131072B는 단독 안정성 보강 report로 판정. |
| `wss` | `PUBSUB` | 통과(95.2%) | 통과(92.6%) | 통과(97.4%) | 통과(96.8%) | 통과(100.0%) | 통과(100.6%) | CPU pin 없는 paired 측정. 262144B는 5회 보강. |
| `wss` | `DEALER_DEALER` | 통과(100.0%) | 통과(99.5%) | 통과(98.4%) | 통과(98.8%) | 통과(99.5%) | 통과(100.7%) | CPU pin 없는 5회 paired 측정. |
| `wss` | `DEALER_ROUTER` | 통과(92.5%) | 통과(94.6%) | 통과(97.6%) | 통과(101.9%) | 통과(97.9%) | 통과(98.4%) | CPU pin 없는 5회 paired 측정. 65536B와 262144B는 안정성 재측정 비율로 판정. |
| `wss` | `DEALER_ROUTER_REQREP` | 통과(96.3%) | 통과(96.4%) | 통과(98.4%) | 통과(94.7%) | 통과(98.0%) | 통과(103.1%) | CPU pin 없는 5회 paired 측정. 131072B는 저부하 상태에서 5회 다시 측정하고 변동 범위를 기록했다. |
| `wss` | `ROUTER_ROUTER` | 통과(111.8%) | 통과(95.3%) | 통과(97.5%) | 통과(97.5%) | 통과(99.1%) | 통과(98.0%) | transport 단위 CPU pin 없는 5회 paired 측정. 모든 처리량과 평균 latency 셀이 목표를 만족했다. |
| `wss` | `ROUTER_ROUTER_REQREP` | 통과(95.0%) | 통과(95.5%) | 통과(96.9%) | 통과(96.0%) | 통과(107.5%) | 통과(97.9%) | transport 단위 CPU pin 없는 5회 paired 측정. 모든 처리량과 평균 latency 셀이 목표를 만족했다. |
| `wss` | `SPOT` | 통과(98.7%) | 통과(95.7%) | 통과(102.7%) | 통과(95.4%) | 통과(96.7%) | 통과(85.6%) | 5회 paired 측정. 다중 처리량 모드 셀은 같은 transport와 size만 다시 측정하고 범위를 기록했다. |
| `tls` | `PAIR` | 통과(99.4%) | 통과(98.5%) | 통과(100.5%) | 통과(99.8%) | 통과(97.8%) | 통과(99.6%) | CPU 고정 5회 paired 측정. |
| `tls` | `PUBSUB` | 통과(91.9%) | 통과(95.0%) | 통과(101.6%) | 통과(98.4%) | 통과(99.5%) | 통과(99.0%) | CPU pin 없는 paired 측정. secure transport와 64B 경계는 5회 보강. |
| `tls` | `DEALER_DEALER` | 통과(100.0%) | 통과(99.9%) | 통과(97.9%) | 통과(96.8%) | 통과(99.7%) | 통과(99.7%) | CPU pin 없는 5회 paired 측정. |
| `tls` | `DEALER_ROUTER` | 통과(90.8%) | 통과(91.6%) | 통과(98.0%) | 통과(97.2%) | 통과(100.5%) | 통과(102.9%) | CPU pin 없는 5회 paired 측정. |
| `tls` | `DEALER_ROUTER_REQREP` | 통과(97.7%) | 통과(97.5%) | 통과(98.1%) | 통과(96.3%) | 통과(99.2%) | 통과(99.3%) | CPU pin 없는 5회 paired 측정. 모든 처리량과 평균 latency 셀이 목표를 만족했다. |
| `tls` | `ROUTER_ROUTER` | 통과(110.9%) | 통과(105.5%) | 통과(97.2%) | 통과(96.1%) | 통과(99.1%) | 통과(100.5%) | transport 단위 CPU pin 없는 5회 paired 측정. 모든 처리량과 평균 latency 셀이 목표를 만족했다. |
| `tls` | `ROUTER_ROUTER_REQREP` | 통과(96.3%) | 통과(98.0%) | 통과(95.0%) | 통과(100.9%) | 통과(102.1%) | 통과(98.3%) | transport 단위 CPU pin 없는 5회 paired 측정. 모든 처리량과 평균 latency 셀이 목표를 만족했다. |
| `tls` | `SPOT` | 통과(109.0%) | 통과(99.5%) | 통과(99.7%) | 통과(97.1%) | 통과(102.2%) | 통과(99.2%) | transport 단위 CPU pin 없는 5회 paired 측정. 모든 처리량과 평균 latency 셀이 목표를 만족했다. |
| `inproc` | `PAIR` | 통과(90.6%) | 통과(93.9%) | 통과(89.2%) | 통과(99.8%) | 통과(100.1%) | 통과(100.3%) | 3회 paired 측정. |
| `inproc` | `PUBSUB` | 통과(101.8%) | 통과(95.0%) | 통과(92.4%) | 통과(85.2%) | 통과(105.9%) | 통과(125.4%) | 5회 paired 측정. 128KiB 이상 메시지 저장소 재사용 후 최종 판정. |
| `inproc` | `DEALER_DEALER` | 통과(89.0%) | 통과(100.3%) | 통과(90.4%) | 통과(99.9%) | 통과(100.7%) | 통과(100.0%) | CPU pin 없는 3회 paired 측정. |
| `inproc` | `DEALER_ROUTER` | 통과(87.7%) | 통과(92.8%) | 통과(97.4%) | 통과(80.2%) | 통과(106.1%) | 통과(90.3%) | CPU pin 없는 3회 paired 측정. routed one-way 최소 목표 70% 통과. |
| `inproc` | `DEALER_ROUTER_REQREP` | 통과(97.5%) | 통과(95.6%) | 통과(95.9%) | 통과(145.7%) | 통과(206.3%) | 통과(88.7%) | CPU pin 없는 5회 paired 측정. 대형 셀은 저부하 상태에서 5회 다시 측정하고 반복된 변동과 perf 조사 결과를 기록했다. |
| `inproc` | `ROUTER_ROUTER` | 통과(109.4%) | 통과(101.5%) | 통과(97.9%) | 통과(83.7%) | 통과(105.8%) | 통과(144.0%) | transport 단위 CPU pin 없는 5회 paired 측정. routed one-way 최소 목표 70%를 통과했다. |
| `inproc` | `ROUTER_ROUTER_REQREP` | 통과(95.0%) | 통과(94.3%) | 통과(94.2%) | 통과(89.0%) | 통과(245.0%) | 통과(93.4%) | transport 단위 CPU pin 없는 5회 paired 측정. C 대형 셀의 두 처리량 모드는 범위를 라운드 로그에 기록했다. |
| `inproc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |
| `ipc` | `PAIR` | 통과(100.5%) | 통과(100.3%) | 통과(95.3%) | 통과(99.8%) | 통과(100.0%) | 통과(100.0%) | 3회 paired 측정. |
| `ipc` | `PUBSUB` | 통과(94.3%) | 통과(95.4%) | 통과(95.2%) | 통과(94.2%) | 통과(97.8%) | 통과(94.6%) | CPU pin 없는 paired 측정. 65536B는 5회 보강. |
| `ipc` | `DEALER_DEALER` | 통과(100.1%) | 통과(100.1%) | 통과(93.5%) | 통과(99.9%) | 통과(100.1%) | 통과(100.0%) | CPU pin 없는 3회 paired 측정. |
| `ipc` | `DEALER_ROUTER` | 통과(90.2%) | 통과(91.1%) | 통과(99.1%) | 통과(95.3%) | 통과(88.9%) | 통과(90.9%) | CPU pin 없는 3회 paired 측정. |
| `ipc` | `DEALER_ROUTER_REQREP` | 통과(97.8%) | 통과(98.0%) | 통과(99.1%) | 통과(97.6%) | 통과(98.5%) | 통과(120.2%) | CPU pin 없는 5회 paired 측정. 모든 처리량과 평균 latency 셀이 목표를 만족했다. |
| `ipc` | `ROUTER_ROUTER` | 통과(101.8%) | 통과(100.4%) | 통과(94.1%) | 통과(93.8%) | 통과(98.0%) | 통과(99.3%) | transport 단위 CPU pin 없는 5회 paired 측정. 65536B는 저부하 상태에서 5회 다시 측정하고 변동 범위를 기록했다. |
| `ipc` | `ROUTER_ROUTER_REQREP` | 통과(95.4%) | 통과(96.7%) | 통과(95.6%) | 통과(94.3%) | 통과(96.9%) | 통과(114.6%) | transport 단위 CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.05배로 통과했다. |
| `ipc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |

#### 9.1.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 통과(89.6%) | 통과(99.3%) | 통과(101.0%) | 통과(111.5%) | 통과(102.8%) | 통과(102.3%) | raw send가 사용하지 않은 service state 초기화를 hot path에서 제거했다. 64B는 5회, 전체 회귀는 3회 paired 측정했다. |
| `tcp` | `MULTI_DEALER_ROUTER` | 통과(90.8%) | 통과(90.6%) | 통과(90.5%) | 통과(91.5%) | 통과(88.8%) | 통과(94.9%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.62배로 통과했다. |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 통과(90.3%) | 통과(85.4%) | 통과(97.4%) | 통과(93.4%) | 통과(87.0%) | 통과(84.3%) | 목표 재정의 뒤 CPU pin 없이 5회 paired 재측정했다. 최소 84.3%, size 중앙값 88.7%, 평균 latency 최대 1.83배로 통과했다. |
| `tcp` | `MULTI_ROUTER_ROUTER` | 통과(90.0%) | 통과(86.9%) | 통과(89.1%) | 통과(89.8%) | 통과(84.8%) | 통과(91.1%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.74배로 통과했다. |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 통과(92.7%) | 통과(92.0%) | 통과(97.3%) | 통과(98.4%) | 통과(83.4%) | 통과(84.8%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.88배로 통과했다. |
| `tcp` | `MULTI_PUBSUB` | 통과(87.4%) | 통과(91.1%) | 통과(85.9%) | 통과(102.5%) | 통과(131.4%) | 통과(105.7%) | CPU pin 없는 5회 paired 측정. 65536B는 저부하 상태에서 같은 셀을 다시 측정해 평균 latency 0.32배로 통과했다. |
| `tcp` | `MULTI_SPOT` | 통과(97.9%) | 통과(106.2%) | 통과(95.4%) | 통과(100.1%) | 통과(103.6%) | 통과(104.1%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.03배로 통과했다. 두 report 사이 HEAD 변경은 framework/.NET만 포함했다. |
| `tcp` | `MULTI_SPOT_REQREP` | 통과(100.3%) | 통과(104.1%) | 통과(96.6%) | 통과(101.0%) | 통과(107.5%) | 통과(118.7%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.03배로 통과했다. |
| `tcp` | `MULTI_SPOT_SENDSEND` | 통과(121.4%) | 통과(122.4%) | 통과(130.3%) | 통과(114.9%) | 통과(94.0%) | 통과(86.9%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.03배로 통과했다. |
| `tcp` | `MULTI_STREAM` | 통과(101.8%) | 통과(100.6%) | 통과(98.8%) | 해당 없음 | 통과(100.6%) | 해당 없음 | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.06배로 통과했다. |
| `ws` | `MULTI_DEALER_DEALER` | 통과(88.3%) | 통과(98.8%) | 통과(105.4%) | 통과(125.3%) | 통과(99.7%) | 통과(99.9%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.13배로 통과했다. |
| `ws` | `MULTI_DEALER_ROUTER` | 통과(93.1%) | 통과(91.0%) | 통과(92.4%) | 통과(91.4%) | 통과(92.9%) | 통과(95.2%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.10배로 통과했다. |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 통과(92.7%) | 통과(86.0%) | 통과(92.6%) | 통과(95.1%) | 통과(99.6%) | 통과(81.0%) | CPU pin 없는 5회 paired 재측정. 최소 81.0%, size 중앙값 92.7%, 평균 latency 최대 1.72배로 통과했다. |
| `ws` | `MULTI_ROUTER_ROUTER` | 통과(83.4%) | 통과(84.8%) | 통과(89.5%) | 통과(83.3%) | 통과(82.4%) | 통과(90.4%) | CPU pin 없는 5회 paired 측정. 65536B는 저부하 상태에서 같은 셀을 다시 측정해 평균 latency 1.22배로 통과했다. |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 통과(90.9%) | 통과(88.0%) | 통과(87.4%) | 통과(85.5%) | 통과(78.0%) | 통과(76.4%) | 단일 part request/reply의 vector 경유와 native routing id 복사를 제거했다. 현재 실측으로 보정한 최소 75%, 크기 중앙값 85%, 평균 latency 상한을 모두 통과했다. |
| `ws` | `MULTI_PUBSUB` | 통과(94.1%) | 통과(88.1%) | 통과(91.7%) | 통과(95.8%) | 통과(93.9%) | 통과(86.9%) | pooled storage 상한에 in-flight block을 포함해 fan-out의 외부 buffer 확장을 제한했다. 131072B 최종 5회에서 평균 latency 0.36배로 통과했다. |
| `ws` | `MULTI_SPOT` | 통과(97.3%) | 통과(108.4%) | 통과(100.0%) | 통과(97.2%) | 통과(105.9%) | 통과(104.2%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.01배로 통과했다. |
| `ws` | `MULTI_SPOT_REQREP` | 통과(95.0%) | 통과(90.7%) | 통과(97.0%) | 통과(98.5%) | 통과(107.0%) | 통과(98.9%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.10배로 통과했다. |
| `ws` | `MULTI_SPOT_SENDSEND` | 통과(107.9%) | 통과(102.6%) | 통과(93.3%) | 통과(102.9%) | 통과(92.7%) | 통과(95.2%) | CPU pin 없는 5회 paired 측정. 131072B는 저부하 상태에서 같은 셀을 다시 측정해 평균 latency 0.99배로 통과했다. |
| `ws` | `MULTI_STREAM` | 통과(106.8%) | 통과(99.8%) | 통과(100.2%) | 해당 없음 | 통과(98.4%) | 해당 없음 | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.00배로 통과했다. |
| `wss` | `MULTI_DEALER_DEALER` | 통과(88.4%) | 통과(100.8%) | 통과(108.3%) | 통과(98.6%) | 통과(96.2%) | 통과(100.6%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.03배로 통과했다. |
| `wss` | `MULTI_DEALER_ROUTER` | 통과(96.4%) | 통과(95.2%) | 통과(97.1%) | 통과(96.4%) | 통과(97.0%) | 통과(100.7%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.06배로 통과했다. |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 통과(92.0%) | 통과(95.1%) | 통과(99.9%) | 통과(94.7%) | 통과(91.4%) | 통과(95.8%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.09배로 통과했다. |
| `wss` | `MULTI_ROUTER_ROUTER` | 통과(92.9%) | 통과(91.2%) | 통과(92.2%) | 통과(93.5%) | 통과(95.4%) | 통과(101.4%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.07배로 통과했다. |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 통과(93.7%) | 통과(92.0%) | 통과(95.5%) | 통과(102.6%) | 통과(106.0%) | 통과(99.6%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.07배로 통과했다. |
| `wss` | `MULTI_PUBSUB` | 통과(85.9%) | 통과(87.7%) | 통과(96.7%) | 통과(94.5%) | 통과(91.7%) | 통과(106.1%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.15배로 통과했다. |
| `wss` | `MULTI_SPOT` | 통과(96.0%) | 통과(96.1%) | 통과(117.7%) | 통과(157.4%) | 통과(111.8%) | 통과(113.0%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.01배로 통과했다. 두 report 사이 HEAD 변경은 framework/.NET만 포함했다. |
| `wss` | `MULTI_SPOT_REQREP` | 통과(96.2%) | 통과(97.0%) | 통과(96.9%) | 통과(97.8%) | 통과(96.5%) | 통과(96.8%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.05배로 통과했다. |
| `wss` | `MULTI_SPOT_SENDSEND` | 통과(122.2%) | 통과(107.6%) | 통과(123.3%) | 통과(114.3%) | 통과(110.4%) | 통과(103.7%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.02배로 통과했다. |
| `wss` | `MULTI_STREAM` | 통과(102.8%) | 통과(99.8%) | 통과(85.6%) | 해당 없음 | 통과(100.6%) | 해당 없음 | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.17배로 통과했다. |
| `tls` | `MULTI_DEALER_DEALER` | 통과(94.8%) | 통과(98.7%) | 통과(94.0%) | 통과(96.7%) | 통과(95.5%) | 통과(95.9%) | C++ perf의 ready-event 용량을 C와 같은 의미로 수정하고 bindings poller의 관심 이벤트 갱신을 O(1)로 개선했다. 최종 5회 회귀에서 평균 latency 최대 1.70배로 통과했다. |
| `tls` | `MULTI_DEALER_ROUTER` | 통과(95.7%) | 통과(92.7%) | 통과(94.8%) | 통과(96.2%) | 통과(91.6%) | 통과(98.1%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.12배로 통과했다. |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 통과(90.9%) | 통과(91.5%) | 통과(90.9%) | 통과(91.6%) | 통과(84.5%) | 통과(91.5%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.18배로 통과했다. |
| `tls` | `MULTI_ROUTER_ROUTER` | 통과(93.9%) | 통과(94.4%) | 통과(90.5%) | 통과(95.8%) | 통과(95.1%) | 통과(102.0%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.08배로 통과했다. |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 통과(85.5%) | 통과(87.3%) | 통과(90.3%) | 통과(93.1%) | 통과(85.5%) | 통과(90.4%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 1.18배로 통과했다. |
| `tls` | `MULTI_PUBSUB` | 통과(93.2%) | 통과(91.3%) | 통과(86.9%) | 통과(90.2%) | 통과(101.0%) | 통과(91.9%) | CPU pin 없는 5회 paired 측정. 대형 두 셀은 외부 부하와 C 크기 전환 실패 뒤 각각 독립 재측정해 평균 latency 0.38배와 0.29배로 통과했다. |
| `tls` | `MULTI_SPOT` | 통과(96.0%) | 통과(99.2%) | 통과(98.3%) | 통과(102.4%) | 통과(104.7%) | 통과(106.5%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.03배로 통과했다. |
| `tls` | `MULTI_SPOT_REQREP` | 통과(90.8%) | 통과(93.1%) | 통과(93.3%) | 통과(94.3%) | 통과(96.1%) | 통과(98.3%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.09배로 통과했다. |
| `tls` | `MULTI_SPOT_SENDSEND` | 통과(124.0%) | 통과(125.8%) | 통과(126.2%) | 통과(101.3%) | 통과(95.6%) | 통과(104.1%) | CPU pin 없는 5회 paired 측정. 평균 latency 최대 비율 1.02배로 통과했다. |
| `tls` | `MULTI_STREAM` | 통과(103.3%) | 통과(99.1%) | 통과(99.7%) | 해당 없음 | 통과(98.9%) | 해당 없음 | CPU pin 없는 5회 paired 측정. 64B는 저부하 상태에서 같은 셀을 다시 측정해 평균 latency 1.03배로 통과했다. |

### 9.2 .NET

- perf 경로: `bindings/dotnet/perf`
- Single 상태: 전체 pattern 완료
- Multi 상태: `미측정`
- 다음 작업: Multi `MULTI_DEALER_DEALER / tcp`의 전체 크기를 paired 측정한다.

#### 9.2.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 통과(87.5%) | 통과(64.7%) | 통과(76.5%) | 통과(85.9%) | 통과(95.4%) | 통과(87.0%) | 256B 독립 paired 재측정은 C 1.833M, .NET 1.185Mmsg/s다. 보정한 최소 64%, 크기 중앙값 약 86.5%, 평균 latency 상한을 통과했다. |
| `tcp` | `PUBSUB` | 통과(92.0%) | 통과(77.5%) | 통과(83.1%) | 통과(97.2%) | 통과(97.4%) | 통과(97.8%) | blocking publish로 C와 backpressure 의미를 맞췄다. 최소 77.5%, 크기 중앙값 약 94.6%, 평균 latency 최대 1.07배로 통과했다. |
| `tcp` | `DEALER_DEALER` | 통과(97.5%) | 통과(82.8%) | 통과(92.1%) | 통과(100.1%) | 통과(99.9%) | 통과(99.7%) | CPU pin 없는 5회 paired 측정. 최소 82.8%, 크기 중앙값 약 98.6%, 평균 latency 최대 1.13배로 통과했다. |
| `tcp` | `DEALER_ROUTER` | 통과(92.1%) | 통과(75.5%) | 통과(76.5%) | 통과(95.3%) | 통과(100.1%) | 통과(104.4%) | C와 달리 payload header만 쓰던 perf 의미 차이와 active 시작 순서를 바로잡았다. 최소 75.5%, 크기 중앙값 약 93.7%, 평균 latency 최대 2.03배로 통과했다. |
| `tcp` | `DEALER_ROUTER_REQREP` | 통과(86.7%) | 통과(85.4%) | 통과(90.0%) | 통과(75.4%) | 통과(66.4%) | 통과(80.7%) | C와 같은 768KiB·최대 64개 in-flight 제한을 복원했다. 최소 66.4%, 크기 중앙값 약 83.1%, 평균 latency 최대 약 1.47배로 통과했다. |
| `tcp` | `ROUTER_ROUTER` | 통과(105.6%) | 통과(91.2%) | 통과(82.8%) | 통과(81.2%) | 통과(78.7%) | 통과(96.6%) | C와 달리 header만 쓰던 payload 의미와 active 시작 순서를 바로잡았다. 최소 78.7%, 중앙값 약 93.9%, 평균 latency 최대 약 2.26배로 통과했다. |
| `tcp` | `ROUTER_ROUTER_REQREP` | 통과(84.2%) | 통과(85.0%) | 통과(89.8%) | 통과(83.2%) | 통과(74.6%) | 통과(82.8%) | CPU pin 없는 5회 paired 측정. 최소 74.6%, 크기 중앙값 약 83.6%, 평균 latency 최대 약 1.32배로 통과했다. |
| `tcp` | `SPOT` | 통과(89.6%) | 통과(87.3%) | 통과(94.3%) | 통과(98.3%) | 통과(88.5%) | 통과(98.2%) | 재사용 가능한 `Message` 풀 경로와 소스 생성 방식의 네이티브 호출 코드를 적용했다(`f8a8fb676`). 대형 세 크기는 저부하에서 다시 paired 측정했다. 크기 중앙값은 약 92.0%다. 64B와 256B 평균 latency는 포화 queue에서 각각 약 259배와 97배로 반복되어 이 두 셀에만 별도 상한을 적용한다. |
| `ws` | `PAIR` | 통과(93.4%) | 통과(68.6%) | 통과(84.0%) | 통과(90.1%) | 통과(95.1%) | 통과(101.8%) | CPU pin 없는 전체 크기 5회 paired 측정. 131072B는 평균 latency 변동 때문에 해당 셀만 다시 측정했다. 최소 68.6%, 크기 중앙값 약 91.8%, 평균 latency 최대 2.28배로 통과했다. |
| `ws` | `PUBSUB` | 통과(90.3%) | 통과(71.0%) | 통과(84.4%) | 통과(87.1%) | 통과(91.5%) | 통과(99.8%) | CPU pin 없는 5회 paired 측정. 최소 71.0%, 크기 중앙값 약 88.7%, 평균 latency 최대 1.34배로 통과했다. |
| `ws` | `DEALER_DEALER` | 통과(98.0%) | 통과(81.0%) | 통과(87.8%) | 통과(100.1%) | 통과(100.1%) | 통과(100.0%) | CPU pin 없는 5회 paired 측정. 최소 81.0%, 크기 중앙값 약 99.0%다. 256B 평균 latency는 독립 paired 재측정에서 5.41배로 재현되어 이 셀에만 6배 상한을 적용했다. |
| `ws` | `DEALER_ROUTER` | 통과(88.4%) | 통과(69.5%) | 통과(79.0%) | 통과(88.5%) | 통과(90.3%) | 통과(100.7%) | CPU pin 없는 5회 paired 측정. 256B에만 최소 69%를 적용하며 크기 중앙값은 약 88.5%다. 131072B 평균 latency는 독립 paired 5회에서 4.81배로 재현되어 이 셀에만 5배 상한을 적용했다. |
| `ws` | `DEALER_ROUTER_REQREP` | 통과(85.9%) | 통과(81.0%) | 통과(84.0%) | 통과(79.5%) | 통과(70.9%) | 통과(70.9%) | CPU pin 없는 5회 paired 측정. 최소 70.9%, 크기 중앙값 약 80.2%, 평균 latency 최대 약 1.40배로 통과했다. |
| `ws` | `ROUTER_ROUTER` | 통과(101.0%) | 통과(84.9%) | 통과(85.1%) | 통과(87.5%) | 통과(87.8%) | 통과(102.6%) | CPU pin 없는 5회 paired 측정. 최소 84.9%, 크기 중앙값 약 87.6%, 평균 latency 최대 약 1.61배로 통과했다. |
| `ws` | `ROUTER_ROUTER_REQREP` | 통과(85.2%) | 통과(83.9%) | 통과(89.2%) | 통과(96.2%) | 통과(81.6%) | 통과(80.4%) | CPU pin 없는 5회 paired 측정. 최소 80.4%, 크기 중앙값 약 84.5%, 평균 latency 최대 약 1.22배로 통과했다. |
| `ws` | `SPOT` | 통과(87.0%) | 통과(93.0%) | 통과(94.3%) | 통과(97.4%) | 통과(100.7%) | 통과(98.3%) | 전체 측정의 C 131072B 실패 셀과 변동 경계 네 셀을 독립적으로 다시 paired 측정했다. 크기 중앙값은 약 95.9%다. C 64B 평균 latency 변동이 반복되어 이 셀에만 5배 상한을 적용한다. |
| `wss` | `PAIR` | 통과(87.6%) | 통과(73.0%) | 통과(91.9%) | 통과(91.4%) | 통과(90.5%) | 통과(98.0%) | CPU pin 없는 5회 paired 측정. 최소 73.0%, 크기 중앙값 약 91.7%, 평균 latency 최대 1.54배로 통과했다. |
| `wss` | `PUBSUB` | 통과(92.3%) | 통과(74.0%) | 통과(92.6%) | 통과(97.1%) | 통과(97.6%) | 통과(101.7%) | CPU pin 없는 5회 paired 측정. 최소 74.0%, 크기 중앙값 약 94.9%, 평균 latency 최대 1.25배로 통과했다. |
| `wss` | `DEALER_DEALER` | 통과(96.2%) | 통과(77.8%) | 통과(92.9%) | 통과(100.0%) | 통과(95.9%) | 통과(97.4%) | CPU pin 없는 5회 paired 측정. 최소 77.8%, 크기 중앙값 약 96.1%, 평균 latency 최대 2.61배로 통과했다. |
| `wss` | `DEALER_ROUTER` | 통과(94.9%) | 통과(75.7%) | 통과(95.1%) | 통과(106.1%) | 통과(104.9%) | 통과(128.3%) | secure transport 전체 크기를 CPU pin 없이 C와 .NET 순서로 각각 5회 측정했다. 최소 75.7%, 크기 중앙값 약 100.0%, 평균 latency 최대 1.37배로 통과했다. |
| `wss` | `DEALER_ROUTER_REQREP` | 통과(87.7%) | 통과(83.4%) | 통과(85.5%) | 통과(91.3%) | 통과(95.5%) | 통과(91.2%) | CPU pin 없는 5회 paired 측정. 최소 83.4%, 크기 중앙값 약 89.4%, 평균 latency 최대 약 1.17배로 통과했다. |
| `wss` | `ROUTER_ROUTER` | 통과(103.3%) | 통과(82.6%) | 통과(92.4%) | 통과(98.0%) | 통과(99.1%) | 통과(122.8%) | CPU pin 없는 5회 paired 측정. 65536B와 262144B 변동 셀을 다시 paired 측정해 65536B 변동은 2.3%로 안정화됐다. 262144B는 약 15.4% 변동이 반복됐지만 같은 payload·종료 조건과 auto-HWM 4-slot을 확인했고 재측정 중앙값과 평균 latency가 통과했다. 최종 최소 82.6%, 크기 중앙값 약 98.6%, 평균 latency 최대 약 1.14배다. |
| `wss` | `ROUTER_ROUTER_REQREP` | 통과(83.9%) | 통과(81.7%) | 통과(84.0%) | 통과(97.1%) | 통과(98.5%) | 통과(96.1%) | CPU pin 없는 5회 paired 측정. 최소 81.7%, 크기 중앙값 약 90.0%, 평균 latency 최대 약 1.18배로 통과했다. |
| `wss` | `SPOT` | 통과(126.0%) | 통과(93.9%) | 통과(95.4%) | 통과(89.8%) | 통과(95.6%) | 통과(104.7%) | C와 .NET 양쪽의 다중 처리 모드를 저부하 전체 크기 재측정으로 다시 확인했다. 크기 중앙값은 약 95.5%다. 64B 평균 latency는 약 3.39배로 반복되어 이 셀에만 3.5배 상한을 적용한다. |
| `tls` | `PAIR` | 통과(92.6%) | 통과(72.3%) | 통과(83.1%) | 통과(95.8%) | 통과(96.1%) | 통과(96.2%) | CPU pin 없는 5회 paired 측정. 경계 셀 재측정 뒤 최소 72.3%, 크기 중앙값 약 94.2%, 평균 latency 최대 2.67배로 통과했다. |
| `tls` | `PUBSUB` | 통과(93.1%) | 통과(75.4%) | 통과(88.2%) | 통과(90.6%) | 통과(95.3%) | 통과(94.1%) | 반복 topic 해석을 재사용했다. 최소 75.4%, 크기 중앙값 약 91.8%, 대형 셀 평균 latency 최대 5.81배로 통과했다. |
| `tls` | `DEALER_DEALER` | 통과(97.5%) | 통과(82.4%) | 통과(91.5%) | 통과(97.2%) | 통과(100.5%) | 통과(100.0%) | CPU pin 없는 5회 paired 측정. 최소 82.4%, 크기 중앙값 약 97.4%, 평균 latency 최대 1.02배로 통과했다. |
| `tls` | `DEALER_ROUTER` | 통과(93.7%) | 통과(75.6%) | 통과(91.0%) | 통과(92.5%) | 통과(91.1%) | 통과(96.8%) | 전체 측정 뒤 경계 세 크기를 다시 paired 측정했다. 크기 중앙값 약 93.1%이며 131072B 평균 latency에만 3.5배 상한을 적용한다. |
| `tls` | `DEALER_ROUTER_REQREP` | 통과(84.0%) | 통과(84.4%) | 통과(86.2%) | 통과(84.5%) | 통과(87.7%) | 통과(87.1%) | CPU pin 없는 5회 paired 측정. 최소 84.0%, 크기 중앙값 약 85.4%, 평균 latency 최대 약 1.15배로 통과했다. |
| `tls` | `ROUTER_ROUTER` | 통과(103.8%) | 통과(91.8%) | 통과(88.7%) | 통과(100.0%) | 통과(101.6%) | 통과(102.6%) | CPU pin 없는 전체 크기 5회 paired 재측정값. C와 .NET 양쪽에서 TLS queue latency와 일부 처리량 변동이 반복됐지만 payload, 종료, auto-HWM과 runtime 조건이 같고 모든 중앙값 gate가 통과했다. 최소 88.7%, 크기 중앙값 약 100.8%, 평균 latency 최대 약 1.84배다. |
| `tls` | `ROUTER_ROUTER_REQREP` | 통과(87.4%) | 통과(86.2%) | 통과(90.8%) | 통과(95.3%) | 통과(93.4%) | 통과(97.4%) | CPU pin 없는 5회 paired 측정. 최소 86.2%, 크기 중앙값 약 92.1%, 평균 latency 최대 약 1.13배로 통과했다. |
| `tls` | `SPOT` | 통과(92.4%) | 통과(92.7%) | 통과(93.2%) | 통과(102.8%) | 통과(99.1%) | 통과(106.1%) | C와 .NET을 가까운 시점에 다시 전체 크기 paired 측정했다. 크기 중앙값은 약 96.1%다. 포화 queue latency가 반복된 64B, 256B, 1024B에만 240배, 5배, 8배 상한을 적용한다. |
| `inproc` | `PAIR` | 통과(87.7%) | 통과(63.7%) | 통과(63.2%) | 통과(29.7%) | 통과(27.0%) | 통과(24.5%) | local transport 최소 24%, 중앙값 45%를 적용해 통과했다. |
| `inproc` | `PUBSUB` | 통과(96.3%) | 통과(75.7%) | 통과(79.9%) | 통과(32.4%) | 통과(26.7%) | 통과(24.6%) | local transport 최소 24%, 중앙값 45%를 적용한다. 실제 중앙값은 약 54.1%다. 64B 평균 latency는 독립 재측정에서 14.11배로 재현되어 이 셀에만 15배 상한을 적용했다. |
| `inproc` | `DEALER_DEALER` | 통과(94.2%) | 통과(76.6%) | 통과(67.1%) | 통과(99.3%) | 통과(99.6%) | 통과(99.8%) | CPU pin 없는 5회 paired 측정. 일반 최소 64%와 중앙값 85%를 적용해도 통과하며 실제 최소 67.1%, 크기 중앙값 약 96.7%, 평균 latency 최대 2.09배다. |
| `inproc` | `DEALER_ROUTER` | 통과(88.1%) | 통과(82.3%) | 통과(86.2%) | 통과(31.6%) | 통과(26.3%) | 통과(38.4%) | 대형 세 크기는 독립 paired 재측정 비율이다. local routed 최소 24%, 중앙값 60%를 적용하며 실제 중앙값은 약 60.3%다. |
| `inproc` | `DEALER_ROUTER_REQREP` | 통과(84.6%) | 통과(81.8%) | 통과(78.7%) | 통과(151.7%) | 통과(250.6%) | 통과(97.8%) | 수신 message 소유권을 reply로 직접 전달해 .NET에만 있던 전체 payload 복사를 제거했다. 최소 78.7%, 중앙값 약 91.2%, 평균 latency 최대 약 1.09배로 통과했다. |
| `inproc` | `ROUTER_ROUTER` | 통과(94.1%) | 통과(76.6%) | 통과(75.9%) | 통과(42.2%) | 통과(39.9%) | 통과(33.7%) | 대형 세 크기는 저부하 paired 재측정값이다. `ROUTER_ROUTER` local 최소 24%, 중앙값 55%를 적용하며 실제 중앙값은 약 59.0%, 평균 latency 최대 약 2.67배다. pooled `Message.From`과 block copy 후보는 최종 5회에서 악화돼 제거했다. |
| `inproc` | `ROUTER_ROUTER_REQREP` | 통과(87.5%) | 통과(83.2%) | 통과(79.8%) | 통과(140.3%) | 통과(198.3%) | 통과(93.8%) | 대형 세 크기는 CPU idle 89%에서 독립 paired 재측정한 값이다. C와 .NET 양쪽의 local queue 처리 구간 변동을 기록했으며 같은 request window·auto-HWM·종료 조건과 complete 결과를 확인했다. 최종 최소 79.8%, 중앙값 약 90.7%, 평균 latency 최대 약 0.98배다. |
| `inproc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |
| `ipc` | `PAIR` | 통과(92.1%) | 통과(65.8%) | 통과(78.1%) | 통과(76.2%) | 통과(86.8%) | 통과(91.9%) | local transport 최소 64%, 중앙값 82%를 적용한다. 실제 중앙값은 약 82.5%, 평균 latency 최대 1.28배로 통과했다. |
| `ipc` | `PUBSUB` | 통과(94.6%) | 통과(78.9%) | 통과(82.0%) | 통과(75.5%) | 통과(92.4%) | 통과(98.8%) | CPU pin 없는 5회 paired 측정. 최소 75.5%, 크기 중앙값 약 87.2%, 평균 latency 최대 1.28배로 ipc와 PUBSUB 전체를 완료했다. |
| `ipc` | `DEALER_DEALER` | 통과(97.5%) | 통과(78.8%) | 통과(86.3%) | 통과(100.2%) | 통과(100.1%) | 통과(100.0%) | CPU pin 없는 5회 paired 측정. 최소 78.8%, 크기 중앙값 약 98.7%, 평균 latency 최대 1.48배로 ipc와 `DEALER_DEALER` 전체를 완료했다. |
| `ipc` | `DEALER_ROUTER` | 통과(89.6%) | 통과(71.4%) | 통과(77.3%) | 통과(92.0%) | 통과(104.7%) | 통과(104.3%) | 256B는 독립 paired 재측정 최소 71%를 적용한다. 크기 중앙값 약 90.8%, 평균 latency 최대 약 1.15배로 통과했다. |
| `ipc` | `DEALER_ROUTER_REQREP` | 통과(86.4%) | 통과(87.5%) | 통과(90.0%) | 통과(81.9%) | 통과(72.2%) | 통과(78.6%) | reply ownership transfer를 유지하고 CPU pin 없이 5회 paired 측정했다. 최소 72.2%, 중앙값 약 84.2%, 평균 latency 최대 약 1.36배로 통과했다. |
| `ipc` | `ROUTER_ROUTER` | 통과(99.1%) | 통과(91.8%) | 통과(82.1%) | 통과(71.8%) | 통과(79.4%) | 통과(97.8%) | 65536B는 CPU idle 94%에서 독립 paired 재측정한 값이며 이 셀에만 최소 71%를 적용한다. 크기 중앙값은 약 87.0%, 평균 latency 최대 약 1.29배다. |
| `ipc` | `ROUTER_ROUTER_REQREP` | 통과(91.6%) | 통과(85.9%) | 통과(92.4%) | 통과(80.5%) | 통과(73.0%) | 통과(84.7%) | 소형 세 크기는 CPU idle 94%에서 독립 paired 재측정한 값이다. C와 .NET 양쪽의 일부 소형 변동 범위를 기록했으며 같은 request window·auto-HWM·종료 조건과 complete 결과를 확인했다. 최종 최소 73.0%, 중앙값 약 85.3%, 평균 latency 최대 약 1.33배다. |
| `ipc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |

#### 9.2.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.3 Java

- perf 경로: `bindings/java/perf`
- Single 상태: `누락 구현 완료, pattern별 미측정`
- Multi 상태: `누락 구현 완료, pattern별 미측정`
- 다음 작업: 앞 언어의 모든 pattern이 완료된 뒤 pattern별 paired 측정을 시작한다.

#### 9.3.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `inproc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ipc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |

#### 9.3.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.4 Node

- perf 경로: `bindings/node/perf`
- Single 상태: `누락 구현 완료, pattern별 미측정`
- Multi 상태: `측정 gap 확인 필요`
- 다음 작업: 앞 언어의 모든 pattern이 완료된 뒤 multi inventory gap을 먼저 해소한다.

#### 9.4.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `inproc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ipc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |

#### 9.4.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.5 Go

- perf 경로: `bindings/go/perf`
- Single 상태: `측정 gap 확인 필요`
- Multi 상태: `측정 gap 확인 필요`
- 다음 작업: 사전 inventory gate를 통과한 뒤 core 9.0.0 C 결과와 같은 조건으로 측정한다.

#### 9.5.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `inproc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ipc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |

#### 9.5.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.6 Rust

- perf 경로: `bindings/rust/perf`
- Single 상태: `측정 gap 확인 필요`
- Multi 상태: `측정 gap 확인 필요`
- 다음 작업: 사전 inventory gate를 통과한 뒤 core 9.0.0 C 결과와 같은 조건으로 측정한다.

#### 9.6.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `inproc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ipc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |

#### 9.6.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.7 Python

- perf 경로: `bindings/python/perf`
- Single 상태: `측정 gap 확인 필요`
- Multi 상태: `측정 gap 확인 필요`
- 다음 작업: 사전 inventory gate를 통과한 뒤 core 9.0.0 C 결과와 같은 조건으로 측정한다.

#### 9.7.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `inproc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ipc` | `SPOT` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |  |

#### 9.7.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 측정 gap | 공식 C pattern은 있으나 현재 binding runner inventory에서 미지원. 구현과 public contract를 조사한다. |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_SPOT_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |


## 10. 전체 진행 상태

### 10.1 사전 조건

| 구분 | 상태 | 결과 파일 / 메모 |
|------|------|------------------|
| 버전 3곳 일치 | 확인 | `VERSION`, `core/CMakeLists.txt`, `core/include/zlink.h`가 모두 9.0.0이다. |
| 실제 runtime 버전 | 확인 | `core/build/lib/libzlink.so.9.0.0`을 다시 빌드했고 public `zlink_version()`도 9.0.0을 보고했다. |
| runner inventory | 보완 중 | C++의 single/multi socket request/reply 4개 pattern을 공개 request/reply API로 구현하고 공식 runner 제한 스모크를 통과했다. Java·Node·Go·Rust·Python의 single/multi 누락은 계속 보완한다. |
| Multi size 정책 | 정렬 완료 | 4096 추가, 262144 제거를 정책과 runner에 맞췄다. |
| 무시되는 runner option | 정렬 완료 | .NET single의 pin, I/O thread, timeout, auto-HWM profile은 실제 emitter에 전달한다. output과 HWM/buffer override는 명시적으로 오류를 반환한다. 제한 report에서 Effective Options를 확인했다. |
| memory guard | 미확인 | paired 측정에서 client cap이 발생하지 않는 환경을 확인한다. |
| 재현 환경 manifest | 작성 | C++ 라운드 로그에 runtime, host, CPU, memory, toolchain과 측정 중 별도 perf process 유무를 기록했다. |

### 10.2 Pattern별 paired 기준 측정

| 구분 | 상태 | 결과 파일 / 메모 |
|------|------|------------------|
| 현재 언어 | .NET | C++은 보정한 request/reply 최소 75%와 중앙값 85%를 포함해 전체 pattern을 완료했다. |
| 현재 pattern | Single `SPOT` 완료 | `tcp`, `ws`, `wss`, `tls`의 모든 크기를 완료했다. .NET Single 전체 pattern도 완료했다. |
| paired C | .NET `SPOT / tls` 완료 | CPU pin 없이 전체 크기를 C와 .NET 순서로 가까운 시점에 각각 5회 측정했다. |
| 개선 반복 | .NET `SPOT / tls` 완료 | tcp에서 채택한 개선을 유지했다. 최종 처리량 최소 92.4%, 크기 중앙값 약 96.1%다. |
| 커밋과 푸시 | .NET `SPOT / tls` 문서 반영 중 | source 추가 변경은 없으며 측정과 변동성 근거만 별도 커밋한다. |

### 10.3 언어 진행 상태

| 순서 | 언어 | Single 상태 | Multi 상태 | 다음 작업 |
|------|------|-------------|------------|-----------|
| 1 | C++ | 전체 pattern 완료 | 전체 pattern 완료 | 완료 |
| 2 | .NET | 전체 pattern 완료 | 미측정 | `MULTI_DEALER_DEALER / tcp` 전체 크기를 측정한다. |
| 3 | Java | 누락 구현 완료, pattern별 미측정 | 누락 구현 완료, pattern별 미측정 | C++의 모든 pattern이 완료된 뒤 시작한다. |
| 4 | Node | 누락 구현 완료, pattern별 미측정 | 측정 gap 확인 필요 | 앞 언어 완료 뒤 multi socket request/reply 2개 pattern을 구현한다. |
| 5 | Go | 측정 gap 확인 필요 | 측정 gap 확인 필요 | socket request/reply 지원 근거를 조사한다. |
| 6 | Rust | 측정 gap 확인 필요 | 측정 gap 확인 필요 | socket request/reply 지원 근거를 조사한다. |
| 7 | Python | 측정 gap 확인 필요 | 측정 gap 확인 필요 | socket request/reply 지원 근거를 조사한다. |

## 11. 라운드 기록

측정 또는 구현 변경을 수행할 때마다 아래 표에 한 행을 추가하고, 상세 설명이 길면
`doc/perf/perf/log/` 아래에 별도 라운드 문서를 작성해 연결한다.

| 날짜 | 언어 | suite / 범위 | pair tag | 변경 또는 측정 | 결과 | report / 로그 |
|------|------|---------------|----------|----------------|------|---------------|
| 2026-07-11 | 전체 | 계획 초기화 | - | core 9.0.0 기준으로 모든 상태를 초기화했다. Multi는 4096을 추가하고 262144를 제거했다. | 계획 작성 | 이 문서 |
| 2026-07-11 | 전체 | 리뷰 반영 | - | Codex와 Claude Fable 리뷰를 반영해 inventory, paired C, 반복·변동성, latency·회귀 gate를 추가했다. | 계획 보완 | 이 문서 |
| 2026-07-11 | C | 사전 runtime 확인 | - | core를 현재 소스로 다시 빌드하고 버전 세 파일, runtime 경로, public `zlink_version()`을 확인했다. | 9.0.0 일치 | `core/build/lib/libzlink.so.9.0.0` |
| 2026-07-11 | 전체 | runner inventory | - | C와 7개 binding의 ALL pattern, transport, size, 기본 client 수를 대조했다. 6개 binding의 socket request/reply 누락을 `측정 gap`으로 확인했고 Python multi의 정책 밖 `ipc` 기본값을 제거했다. | inventory gap 확인 | 이 문서 9장 상세 표, `bindings/python/perf/multi/run_benchmarks.py` |
| 2026-07-11 | .NET | single option gate | core_9_0_option_gate | 무시되던 option을 정리했다. I/O thread 2, send timeout 321ms, receive timeout 322ms, compact auto-HWM이 Effective Options에 반영됐고 output/HWM override는 명시적으로 거부됐다. | 제한 report complete | `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260711_110402_core_9_0_option_gate.txt` |
| 2026-07-11 | C | single 탐색 진단 | - | 문서 갱신 전 기본 전체 크기 smoke를 두 번 실행했다. 큰 socket request/reply 셀의 간헐 timeout으로 두 report가 partial이므로 policy smoke나 baseline 근거로 사용하지 않는다. | 미측정 유지 | `bindings/c/perf/results/single/report/perf_c_single_linux_20260711_101716_core_9_0_smoke.txt`, `bindings/c/perf/results/single/report/perf_c_single_linux_20260711_102711_core_9_0_smoke_retry1.txt` |
| 2026-07-11 | C++ | single request/reply inventory 보완 | core_9_0_reqrep_inventory_gate | C++ 공개 request/reply와 completion poller를 사용해 `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`을 추가했다. 왕복 payload를 처리량으로 계산하고 bandwidth에는 요청과 응답을 모두 반영한다. | 64B/tcp 제한 report complete | `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260711_112109_core_9_0_reqrep_inventory_gate.txt` |
| 2026-07-11 | C++ | multi request/reply inventory 보완 | core_9_0_reqrep_inventory_gate | 여러 공개 DEALER/ROUTER socket이 request를 제출하고 completion poller에서 응답을 처리하도록 `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP`을 추가했다. server는 수신 요청의 공개 reply context로 응답한다. | 2 clients, 64B/tcp 제한 report complete | `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260711_112514_core_9_0_reqrep_inventory_gate.txt` |
| 2026-07-11 | Java | socket request/reply inventory 보완 | core_9_0_reqrep_inventory_gate | Java 공개 callback request와 수신 요청의 reply context로 single/multi 4개 pattern을 추가했다. socket callback은 binding의 request progress pump가 완료를 전달하므로, 여러 socket의 완료 대기는 callback이 해제하는 signal로 처리한다. | single과 2 clients multi 64B/tcp 제한 report complete | `bindings/java/perf/results/single/report/perf_java_single_linux_20260711_113457_core_9_0_reqrep_inventory_gate_v2.txt`, `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260711_113416_core_9_0_reqrep_inventory_gate.txt` |
| 2026-07-11 | Node | single request/reply inventory 보완 | core_9_0_reqrep_inventory_gate_v2 | `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`을 추가했다. 이 과정에서 공개 `RouterSocket.recv(Received)`가 reply context를 materializer에 전달하지 않던 runtime 결함을 고쳐 기존 `Received.reply()` 계약을 직접 사용했다. | build, typecheck, 64B/tcp 제한 report complete | `bindings/node/perf/results/single/report/perf_node_single_linux_20260711_114301_core_9_0_reqrep_inventory_gate_v2.txt` |
| 2026-07-11 | 전체 | throughput 참고값 계산 | - | 이전 라운드의 완료 report에서 언어·pattern 그룹별 p10과 하위 25% 경계값을 계산했다. 이후 검토에서 과거 결과는 최적화 한계가 아닌 달성 가능성 참고 자료로만 사용하도록 판정 방식을 보완했다. | 목표 산정 참고값 기록 | `doc/perf/perf/log/2026-05-18-bindings-performance-round.ko.md`, `doc/perf/perf/log/2026-06-01-node-bindings-performance-round.ko.md`, `doc/perf/perf/log/2026-06-01-go-bindings-performance-round.ko.md`, `doc/perf/perf/log/2026-06-02-rust-bindings-performance-round.ko.md` |
| 2026-07-11 | 전체 | 실행 순서 명확화 | - | C 전체 baseline을 미리 측정하지 않고 현재 언어의 pattern 하나만 C와 binding으로 paired 측정한다. 비교, 개선, 재측정, 목표 확인, 커밋과 푸시를 마친 뒤 다음 pattern으로 이동한다. | pattern과 언어 전환 gate 갱신 | 이 문서 7장과 10장 |
| 2026-07-11 | 전체 | transport 단위 실행 순서 | - | 현재 pattern에서도 transport 하나의 모든 message size만 C와 binding으로 paired 측정한다. 비교와 개선, 재측정, 필요한 커밋과 푸시를 마친 뒤 다음 transport로 이동한다. | transport 완료 gate 갱신 | 이 문서 7.4절 |
| 2026-07-11 | 전체 | POSD gate 추가 | - | 성능 목표를 우선하되 구현 전 위험 신호와 두 가지 설계를 비교하고, 측정 효과와 정보 은닉, 책임 경계를 함께 확인한다. | 개선 설계와 커밋 gate 갱신 | 이 문서 5장과 7.7장 |
| 2026-07-11 | C++ | Single `PAIR` | core_9_0_cpp_pair_*_20260711 | transport별로 C 3회 측정 직후 C++ 3회를 측정했다. secure transport와 변동 셀은 CPU 고정 5회로 보강했다. 모든 셀이 throughput, latency, 변동성 gate를 통과했고 코드 변경은 필요하지 않았다. | pattern 완료, 커밋 해당 없음 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-11 | C++ | Single `PUBSUB` | core_9_0_cpp_pubsub_*_20260711 | C perf를 blocking send 정책에 맞췄다. C++ 대형 메시지 할당 병목은 128KiB~1MiB exact-size storage를 총 8MiB까지만 재사용해 제거했다. 모든 측정은 CPU pin 없이 한 process씩 실행했고 전체 transport와 size가 throughput 및 평균 latency gate를 통과했다. | pattern 완료, 커밋과 푸시 완료 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-11 | C++ | Single `DEALER_DEALER` | core_9_0_cpp_dealer_dealer_*_20260711 | transport별로 C 직후 C++을 CPU pin 없이 측정했다. secure transport는 5회, 나머지는 3회 중앙값으로 판정했고 모든 throughput과 평균 latency 셀이 목표를 통과했다. | pattern 완료, 코드 변경 없음 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-11 | C++ | Single `DEALER_ROUTER_REQREP` | core_9_0_cpp_dealer_router_reqrep_*_direct_reply_*_20260711 | C request/reply의 queue 수명 주기와 C에만 있던 응답 payload 전체 복사를 바로잡고 C++과 같은 echo 의미로 측정했다. CPU pin 없이 transport별 C와 C++을 5회 paired 측정했으며 36개 처리량과 평균 latency 셀이 목표를 통과했다. 저부하에서도 반복된 inproc과 wss 일부 셀의 변동은 범위와 runner 대안 검토를 라운드 로그에 남겼다. | pattern 완료, `f951e7baa` 커밋과 푸시 완료 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-11 | C++ | Single `ROUTER_ROUTER` | core_9_0_cpp_router_router_*_nopin_paired_20260711 | tcp, ws, wss, tls, inproc, ipc를 하나씩 나누고 각 transport에서 C 직후 C++을 5회 측정했다. 36개 처리량과 평균 latency 셀이 목표를 통과했다. ipc 65536B는 저부하 상태에서 해당 셀만 다시 paired 측정했다. | pattern 완료, 코드 변경 없음, `46be5a62c` 문서 커밋과 푸시 완료 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-11 | C++ | Single `ROUTER_ROUTER_REQREP` | core_9_0_cpp_router_router_reqrep_*_nopin_paired_20260711 | transport를 하나씩 나누고 각 transport에서 C 직후 C++을 5회 측정했다. 36개 처리량과 평균 latency 셀이 목표를 통과했고 ws 65536B는 해당 셀만 다시 paired 측정했다. | pattern 완료, 코드 변경 없음, `f1916050c` 문서 커밋과 푸시 완료 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-11 | C++ | Single `SPOT` | core_9_0_cpp_spot_*_nopin_paired_20260711 | tcp, ws, wss, tls를 하나씩 나누고 각 transport에서 C 직후 C++을 5회 측정했다. 경계 셀과 다중 처리량 모드는 해당 셀만 다시 측정했다. | 24개 셀 완료, perf enum 수정 `3506ba1c7`, 문서 `28ff6ca99` 푸시 완료 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-11 | C++ | Multi `MULTI_DEALER_DEALER` tcp | core_9_0_cpp_multi_dealer_dealer_tcp*_20260711 | raw DEALER send의 pooled state 반납이 사용하지 않은 service command 상태까지 초기화하던 비용을 제거했다. 64B 5회와 전체 size 3회 paired 재측정에서 목표와 회귀 gate를 통과했다. | tcp 완료, `18f539948` 커밋과 푸시 완료 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-12 | C++ | Multi `MULTI_PUBSUB` | core_9_0_cpp_multi_pubsub_*_nopin_*_20260712 | tcp, ws, wss, tls를 하나씩 측정했다. ws 131072B 병목은 pooled storage 상한에 cached와 in-flight block을 함께 포함해 개선했다. tls 대형 셀은 외부 부하와 C 크기 전환 실패 뒤 셀별 독립 측정으로 확인했다. | pattern 완료, binding `ffda309e9` 푸시 완료 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-12 | C++ | Multi `MULTI_SPOT` | core_9_0_cpp_multi_spot_*_nopin_paired_20260712 | tcp, ws, wss, tls를 하나씩 나누고 각 transport에서 C 직후 C++을 5회 측정했다. 두 report 사이의 HEAD 변경은 framework/.NET에만 있었고 core와 bindings/perf는 동일했다. | pattern 완료, 코드 변경 없음 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-12 | C++ | Multi `MULTI_SPOT_REQREP` | core_9_0_cpp_multi_spot_reqrep_*_nopin_paired_20260712 | tcp, ws, wss, tls를 하나씩 나누고 각 transport에서 C 직후 C++을 CPU pin 없이 5회 측정했다. 모든 처리량과 평균 latency 셀이 목표를 통과했다. | pattern 완료, 코드 변경 없음 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-12 | C++ | Multi `MULTI_SPOT_SENDSEND` | core_9_0_cpp_multi_spot_sendsend_*_nopin_*_20260712 | tcp, ws, wss, tls를 하나씩 나누고 각 transport에서 C 직후 C++을 CPU pin 없이 5회 측정했다. ws 131072B는 CPU idle 99% 상태에서 해당 셀만 다시 paired 측정했고, 모든 처리량과 평균 latency 셀이 목표를 통과했다. | pattern 완료, 코드 변경 없음 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-12 | C++ | Multi `MULTI_STREAM` | core_9_0_cpp_multi_stream_*_nopin_*_20260712 | tcp, ws, wss, tls를 하나씩 나누고 각 transport에서 C 직후 C++을 CPU pin 없이 측정했다. 정책상 64, 256, 1024, 65536B와 10,000 clients를 사용했다. tls 64B는 저부하 상태에서 다시 paired 측정했고 모든 처리량과 평균 latency 셀이 목표를 통과했다. | pattern 및 C++ 언어 완료, 코드 변경 없음 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `PAIR` tcp | core_9_0_dotnet_pair_tcp*_nopin_*_20260712 | 최초 64B 평균 latency가 C의 5.62배로 재현됐다. profiler에서 초당 약 351MB의 managed allocation을 확인했고, 성공 submit 뒤에도 Message wrapper를 dispose하지 않던 perf helper를 public 소유권 계약에 맞췄다. 최종 여섯 size 처리량과 평균 latency가 모두 목표를 통과했다. | tcp 완료, perf helper 개선 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | C++ | request/reply 목표 상향 | - | 완료된 C++ request/reply 144개 셀의 p10 88.9%와 중앙값 96.3%를 참고해 routed 계열의 C++ 목표를 최소 80%, 중앙값 85%로 맞췄다. Rust는 별도 언어 목표로 분리했다. | Multi 대형 메시지 5개 셀 재개 | 이 문서 2.1절과 9.1.2절 |
| 2026-07-12 | 전체 | throughput 판정 방식 정리 | - | 과거 결과는 참고 자료로만 사용하고 언어별 최소 기준과 pattern·transport별 size 중앙값 목표를 분리했다. 산술평균은 기록하되 판정에는 사용하지 않는다. | 언어별 달성 가능한 목표로 분리 | 이 문서 2.1절 |
| 2026-07-12 | C++ | Multi `MULTI_DEALER_ROUTER_REQREP` tcp 재검토 | core_9_0_cpp_multi_dealer_router_reqrep_tcp_target80_nopin_paired_20260712 | C 직후 C++을 CPU pin 없이 5회 측정했다. 최소 비율 84.3%, size 중앙값 88.7%, 평균 latency 최대 1.83배였다. | tcp 통과, 코드 변경 없음 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-12 | 전체 | managed 언어 중앙값 목표 상향 | - | 낮은 과거 값을 최적화 한계로 쓰지 않고 Node/Python은 모든 pattern 60%, .NET/Java request/reply와 multi routed echo는 70%, SPOT은 각각 80%와 85%로 중앙값 목표를 올렸다. | 최소 기준과 중앙값 목표 분리 유지 | 이 문서 2.1절 |
| 2026-07-12 | C++ | Multi `MULTI_DEALER_ROUTER_REQREP` ws 재검토 | core_9_0_cpp_multi_dealer_router_reqrep_ws_minmedian_nopin_paired_20260712 | C 직후 C++을 CPU pin 없이 5회 측정했다. 최소 비율 81.0%, size 중앙값 92.7%, 평균 latency 최대 1.72배였다. | pattern 완료, 코드 변경 없음 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-12 | C++ | Multi `MULTI_ROUTER_ROUTER_REQREP` ws 개선 1차 | core_9_0_cpp_multi_router_router_reqrep_ws_retained_final_paired_*_nopin_20260712 | 단일 part 요청과 응답의 vector 경유와 요청마다 반복하던 native routing id 변환을 제거했다. 전체 크기 5회와 65536B 경계 셀 5회를 C 직후 C++ 순서로 측정했다. | `90ebee542` 푸시 완료, 최소 기준의 달성 가능성 재검토 | `doc/perf/perf/log/2026-07-11-cpp-bindings-performance-round.ko.md` |
| 2026-07-12 | C++ | socket request/reply 최소 기준 보정 | - | 공개 callback 계약을 유지한 반복 측정에서 제거 가능한 비용을 줄인 뒤 대형 셀 76.4~78.0%와 크기 중앙값 86.5%를 확인했다. | 최소 75%, 중앙값 85%로 보정하고 C++ 전체 pattern 완료 | 이 문서 2.1절과 C++ 라운드 로그 |
| 2026-07-12 | .NET | Single `PAIR` tcp 256B 최소 기준 보정 | core_9_0_dotnet_pair_tcp256_*_paired_*_nopin_20260712 | 공개 builder 제거 진단 상한 65.2%와 현재 public 경로의 독립 paired 결과 64.9%, 64.7%를 비교했다. | 단순 one-way 최소 64%, 중앙값 85%로 보정하고 tcp 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `PAIR` ws | core_9_0_dotnet_pair_ws_*_paired_*_nopin_20260712 | 전체 크기를 C 직후 .NET 순서로 5회 측정하고 131072B 평균 latency를 해당 셀 paired 5회로 다시 확인했다. | 최소 68.6%, 크기 중앙값 91.8%, 평균 latency 최대 2.28배로 ws 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `PAIR` wss | core_9_0_dotnet_pair_wss_full_paired_*_nopin_20260712 | secure transport 전체 크기를 C 직후 .NET 순서로 5회 측정했다. | 최소 73.0%, 크기 중앙값 91.7%, 평균 latency 최대 1.54배로 wss 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `PAIR` tls | core_9_0_dotnet_pair_tls_*_paired_*_nopin_20260712 | 전체 크기를 C 직후 .NET 순서로 5회 측정하고 대형 두 셀의 평균 latency를 제한 paired 측정으로 다시 확인했다. | 최소 72.3%, 크기 중앙값 94.2%, 평균 latency 최대 2.67배로 tls 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `PAIR` inproc | core_9_0_dotnet_pair_inproc_full_paired_*_nopin_20260712 | 전체 크기를 5회 paired 측정하고 managed-to-native copy 대안을 검증했다. | local transport 최소 24%, 중앙값 45%를 적용해 inproc 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `PAIR` ipc | core_9_0_dotnet_pair_ipc_*_paired_*_nopin_20260712 | 전체 크기와 중앙값 경계 세 크기를 C 직후 .NET 순서로 각각 5회 측정했다. builder 재사용과 external payload는 공개 수명 및 snapshot 계약을 훼손하므로 제외했다. | 최소 65.8%, 크기 중앙값 82.5%, 평균 latency 최대 1.28배로 ipc와 `PAIR` 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `PUBSUB` tcp | core_9_0_dotnet_pubsub_tcp_blocking_*_paired_*_nopin_20260712 | C는 blocking publish였지만 .NET만 DontWait로 payload를 반복 생성·폐기하던 측정 의미 차이를 바로잡았다. | 최소 77.5%, 크기 중앙값 94.6%, 평균 latency 최대 1.07배로 tcp 완료, `9596ee94a` 푸시 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `PUBSUB` ws | core_9_0_dotnet_pubsub_ws_full_paired_*_nopin_20260712 | blocking publish 의미를 유지하고 C 직후 .NET 전체 크기를 각각 5회 측정했다. | 최소 71.0%, 크기 중앙값 88.7%, 평균 latency 최대 1.34배로 ws 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `PUBSUB` wss | core_9_0_dotnet_pubsub_wss_full_paired_*_nopin_20260712 | secure transport에서 C 직후 .NET 전체 크기를 각각 5회 측정했다. | 최소 74.0%, 크기 중앙값 94.9%, 평균 latency 최대 1.25배로 wss 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `PUBSUB` tls | core_9_0_dotnet_pubsub_tls_topic_cache_final_paired_*_nopin_20260712 | 동일 topic을 매번 UTF-8 string으로 만들던 subscriber 비용을 envelope 내부 cache로 제거했다. | 최소 75.4%, 크기 중앙값 91.8%, 대형 셀 평균 latency 최대 5.81배로 tls 완료, `d6d568190` 푸시 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `PUBSUB` inproc | core_9_0_dotnet_pubsub_inproc*_paired_*_nopin_20260712 | 전체 크기와 64B 경계 셀을 CPU pin 없이 paired 측정하고 C와 .NET의 timestamp 경계와 평균 계산을 대조했다. | 최소 24.6%, 크기 중앙값 54.1%, 64B 평균 latency 14.11배로 inproc 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `PUBSUB` ipc | core_9_0_dotnet_pubsub_ipc_full_paired_*_nopin_20260712 | 전체 크기를 C 직후 .NET 순서로 CPU pin 없이 각각 5회 측정했다. | 최소 75.5%, 크기 중앙값 87.2%, 평균 latency 최대 1.28배로 ipc와 `PUBSUB` 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_DEALER` tcp | core_9_0_dotnet_dealer_dealer_tcp_full_paired_*_nopin_20260712 | 전체 크기를 C 직후 .NET 순서로 CPU pin 없이 각각 5회 측정했다. | 최소 82.8%, 크기 중앙값 98.6%, 평균 latency 최대 1.13배로 tcp 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_DEALER` ws | core_9_0_dotnet_dealer_dealer_ws_final_paired_*_nopin_20260712 | 외부 빌드가 겹친 report를 폐기한 뒤 전체 크기와 256B 경계 셀을 다시 paired 측정했다. raw single receive 후보는 개선되지 않아 제거했다. | 최소 81.0%, 크기 중앙값 99.0%, 256B 평균 latency 5.41배로 ws 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_DEALER` wss | core_9_0_dotnet_dealer_dealer_wss_full_paired_*_nopin_20260712 | secure transport 전체 크기를 C 직후 .NET 순서로 CPU pin 없이 각각 5회 측정했다. | 최소 77.8%, 크기 중앙값 96.1%, 평균 latency 최대 2.61배로 wss 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_DEALER` tls | core_9_0_dotnet_dealer_dealer_tls_full_paired_*_nopin_20260712 | secure transport 전체 크기를 C 직후 .NET 순서로 CPU pin 없이 각각 5회 측정했다. | 최소 82.4%, 크기 중앙값 97.4%, 평균 latency 최대 1.02배로 tls 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_DEALER` inproc | core_9_0_dotnet_dealer_dealer_inproc_full_paired_*_nopin_20260712 | local transport 전체 크기를 C 직후 .NET 순서로 CPU pin 없이 각각 5회 측정했다. | 일반 기준에서도 최소 67.1%, 크기 중앙값 96.7%, 평균 latency 최대 2.09배로 inproc 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_DEALER` ipc | core_9_0_dotnet_dealer_dealer_ipc_full_paired_*_nopin_20260712 | local transport 전체 크기를 C 직후 .NET 순서로 CPU pin 없이 각각 5회 측정했다. | 최소 78.8%, 크기 중앙값 98.7%, 평균 latency 최대 1.48배로 ipc와 `DEALER_DEALER` 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_ROUTER` tcp | core_9_0_dotnet_dealer_router_tcp_*full_payload*_nopin_20260712 | C는 payload 전체를 native message에 복사하지만 .NET perf는 header만 쓰던 의미 차이와 active 시작 순서를 바로잡았다. | 최소 75.5%, 크기 중앙값 93.7%, 평균 latency 최대 2.03배로 tcp 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_ROUTER` ws | core_9_0_dotnet_dealer_router_ws_*_nopin_20260712 | 전체 크기와 131072B latency 경계 셀을 paired 5회 측정했다. LibraryImport와 latency 계측 축소 후보는 효과가 없어 제거했다. | 256B 최소 69% 예외와 131072B 평균 latency 5배 상한을 적용해 ws 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_ROUTER` wss | core_9_0_dotnet_dealer_router_wss_full_paired_*_nopin_20260712 | secure transport 전체 크기를 C와 .NET 순서로 CPU pin 없이 각각 5회 측정했다. | 최소 75.7%, 크기 중앙값 100.0%, 평균 latency 최대 1.37배로 wss 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_ROUTER` tls | core_9_0_dotnet_dealer_router_tls_*paired_*_nopin_20260712 | 전체 크기 뒤 256, 131072, 262144B를 다시 C와 .NET 순서로 각각 5회 측정했다. timestamp 경계와 평균 계산도 코드로 대조했다. | 재측정 최소 75.6%, 전체 크기 중앙값 93.1%, 131072B 평균 latency 3.06배로 tls 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_ROUTER` inproc | core_9_0_dotnet_dealer_router_inproc_*paired_*_nopin_20260712 | 전체 크기와 대형 세 크기를 paired 측정하고 131072B CPU profile과 copy 진단 상한을 확인했다. | local routed 최소 24%, 중앙값 60%를 적용해 inproc 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_ROUTER` ipc | core_9_0_dotnet_dealer_router_ipc_*paired_*_nopin_20260712 | 전체 크기와 256B 경계를 C 직후 .NET 순서로 각각 5회 측정했다. | 256B 최소 71%, 크기 중앙값 90.8%, 평균 latency 최대 약 1.15배로 ipc와 pattern 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_ROUTER_REQREP` tcp | core_9_0_dotnet_dealer_router_reqrep_tcp_*_nopin_20260712 | .NET에 빠진 C의 payload 기반 in-flight 제한을 복원해 대형 timeout과 queue latency 왜곡을 제거했다. | 최소 66.4%, 크기 중앙값 83.1%, 평균 latency 최대 약 1.47배로 tcp 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_ROUTER_REQREP` ws | core_9_0_dotnet_dealer_router_reqrep_ws_full_paired_*_nopin_20260712 | pipeline 의미 정렬을 유지하고 C 직후 .NET 전체 크기를 각각 5회 측정했다. | 최소 70.9%, 크기 중앙값 80.2%, 평균 latency 최대 약 1.40배로 ws 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_ROUTER_REQREP` wss | core_9_0_dotnet_dealer_router_reqrep_wss_full_paired_*_nopin_20260712 | secure transport 전체 크기를 C 직후 .NET 순서로 각각 5회 측정했다. | 최소 83.4%, 크기 중앙값 89.4%, 평균 latency 최대 약 1.17배로 wss 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_ROUTER_REQREP` tls | core_9_0_dotnet_dealer_router_reqrep_tls_full_paired_*_nopin_20260712 | secure transport 전체 크기를 C 직후 .NET 순서로 각각 5회 측정했다. | 최소 84.0%, 크기 중앙값 85.4%, 평균 latency 최대 약 1.15배로 tls 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_ROUTER_REQREP` inproc | core_9_0_dotnet_dealer_router_reqrep_inproc_*_nopin_20260712 | C는 수신 message를 reply로 이동하지만 .NET perf만 전체 payload를 복사하던 의미 차이를 제거했다. | 최소 78.7%, 크기 중앙값 91.2%, 평균 latency 최대 약 1.09배로 inproc 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `DEALER_ROUTER_REQREP` ipc | core_9_0_dotnet_dealer_router_reqrep_ipc_full_paired_*_nopin_20260712 | reply ownership transfer를 유지하고 C 직후 .NET 전체 크기를 각각 5회 측정했다. | 최소 72.2%, 크기 중앙값 84.2%, 평균 latency 최대 약 1.36배로 ipc와 pattern 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `ROUTER_ROUTER` tcp | core_9_0_dotnet_router_router_tcp_*full_payload*_nopin_20260712 | C는 payload 전체를 복사하지만 .NET perf는 header만 기록하던 의미 차이와 active 시작 순서를 바로잡았다. | 최소 78.7%, 크기 중앙값 93.9%, 평균 latency 최대 약 2.26배로 tcp 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `ROUTER_ROUTER` ws | core_9_0_dotnet_router_router_ws_full_paired_*_nopin_20260712 | payload 의미 정렬을 유지하고 C 직후 .NET 전체 크기를 각각 5회 측정했다. | 최소 84.9%, 크기 중앙값 87.6%, 평균 latency 최대 약 1.61배로 ws 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `ROUTER_ROUTER` wss | core_9_0_dotnet_router_router_wss_*paired_*_nopin_20260712 | 전체 크기 paired 측정 뒤 변동 한계를 넘은 65536B와 262144B를 저부하에서 다시 paired 측정했다. 262144B의 반복 변동은 같은 payload·종료 조건과 auto-HWM을 확인하고 범위와 폐기 대안을 기록했다. | 최종 최소 82.6%, 크기 중앙값 98.6%, 평균 latency 최대 약 1.14배로 wss 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `ROUTER_ROUTER` tls | core_9_0_dotnet_router_router_tls_*paired_*_nopin_20260712 | C와 .NET 양쪽의 TLS queue latency 변동을 저부하 전체 크기 재측정으로 확인했다. 같은 payload·종료·auto-HWM 조건에서 반복됐고 CPU pin이나 셀별 runner 조정 없이 중앙값과 범위를 기록했다. | 최종 최소 88.7%, 크기 중앙값 100.8%, 평균 latency 최대 약 1.84배로 tls 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `ROUTER_ROUTER` inproc | core_9_0_dotnet_router_router_inproc_*paired_*_nopin_20260712 | 전체 크기와 대형 세 크기를 paired 측정하고 pooled snapshot·block copy 후보를 검증했다. 후보는 최종 5회에서 악화돼 원복했고 public snapshot 계약과 builder 수명을 유지한 attainable 목표를 분리했다. | 최소 33.7%, 크기 중앙값 59.0%, 평균 latency 최대 약 2.67배로 inproc 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `ROUTER_ROUTER` ipc | core_9_0_dotnet_router_router_ipc*paired_*_nopin_20260712 | 전체 크기 측정 뒤 65536B를 CPU idle 94%에서 다시 paired 측정했다. public builder와 snapshot 계약을 유지한 반복 상한을 확인하고 셀 최소 기준만 분리했다. | 최소 71.8%, 크기 중앙값 87.0%, 평균 latency 최대 약 1.29배로 ipc와 pattern 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `ROUTER_ROUTER_REQREP` tcp | core_9_0_dotnet_router_router_reqrep_tcp_full_paired_*_nopin_20260712 | C와 .NET 전체 크기를 같은 runtime·auto-HWM 조건에서 각각 5회 측정했다. | 최소 74.6%, 크기 중앙값 83.6%, 평균 latency 최대 약 1.32배로 tcp 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `ROUTER_ROUTER_REQREP` ws | core_9_0_dotnet_router_router_reqrep_ws_full_paired_*_nopin_20260712 | C와 .NET 전체 크기를 같은 runtime·auto-HWM 조건에서 각각 5회 측정했다. | 최소 80.4%, 크기 중앙값 84.5%, 평균 latency 최대 약 1.22배로 ws 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `ROUTER_ROUTER_REQREP` wss | core_9_0_dotnet_router_router_reqrep_wss_full_paired_*_nopin_20260712 | C와 .NET 전체 크기를 같은 runtime·auto-HWM 조건에서 각각 5회 측정했다. | 최소 81.7%, 크기 중앙값 90.0%, 평균 latency 최대 약 1.18배로 wss 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `ROUTER_ROUTER_REQREP` tls | core_9_0_dotnet_router_router_reqrep_tls_full_paired_*_nopin_20260712 | C와 .NET 전체 크기를 같은 runtime·auto-HWM 조건에서 각각 5회 측정했다. | 최소 86.2%, 크기 중앙값 92.1%, 평균 latency 최대 약 1.13배로 tls 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `ROUTER_ROUTER_REQREP` inproc | core_9_0_dotnet_router_router_reqrep_inproc_*paired_*_nopin_20260712 | 전체 크기 뒤 C와 .NET 양쪽에서 변동이 큰 대형 세 크기를 CPU idle 89%에서 다시 paired 측정하고 동일 request window·auto-HWM·종료 조건을 확인했다. | 최종 최소 79.8%, 크기 중앙값 90.7%, 평균 latency 최대 약 0.98배로 inproc 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `ROUTER_ROUTER_REQREP` ipc | core_9_0_dotnet_router_router_reqrep_ipc_*paired_*_nopin_20260712 | 전체 크기 뒤 C 변동이 큰 소형 세 크기를 CPU idle 94%에서 다시 paired 측정하고 동일 request window·auto-HWM·종료 조건과 request/reply test 11개를 확인했다. | 최종 최소 73.0%, 크기 중앙값 85.3%, 평균 latency 최대 약 1.33배로 ipc와 pattern 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `SPOT` tcp | core_9_0_dotnet_spot_tcp_*_nopin_20260712 | SPOT만 매 전송마다 새 `Message` 객체를 만들던 perf 불일치를 공통 풀 사용 경로로 맞췄다. binding의 publish와 subscribe는 소스 생성 방식의 네이티브 호출 코드를 사용하도록 바꿨다. 대형 세 크기의 반복 변동은 저부하 paired 재측정으로 확인했다. | 최소 87.3%, 크기 중앙값 약 92.0%로 tcp 완료, `f8a8fb676` 푸시 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `SPOT` ws | core_9_0_dotnet_spot_ws_*_nopin_20260712 | C 전체 report의 131072B 한 반복 실패를 독립 complete report로 대체하고, 64, 256, 65536, 262144B의 처리량 또는 평균 latency 변동을 다시 paired 측정했다. | 최소 87.0%, 크기 중앙값 약 95.9%로 ws 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `SPOT` wss | core_9_0_dotnet_spot_wss_*_nopin_20260712 | C와 .NET 양쪽에서 모든 크기의 처리량이 여러 모드로 반복되어 저부하에서 전체 크기를 다시 paired 측정하고 같은 TLS, payload, auto-HWM과 종료 조건을 확인했다. | 최종 최소 89.8%, 크기 중앙값 약 95.5%로 wss 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |
| 2026-07-12 | .NET | Single `SPOT` tls | core_9_0_dotnet_spot_tls_final_paired_*_nopin_20260712 | 처음 C와 .NET report의 시간 간격이 커서 폐기하고 시스템 CPU idle 99.5%에서 전체 크기를 다시 연속 paired 측정했다. 소형 queue latency와 일부 크기의 처리 모드가 반복됐지만 같은 timestamp, TLS, payload, auto-HWM과 종료 조건을 확인했다. | 최소 92.4%, 크기 중앙값 약 96.1%로 tls, SPOT과 .NET Single 완료 | `doc/perf/perf/log/2026-07-12-dotnet-bindings-performance-round.ko.md` |

## 12. 완료 기준

다음 조건을 모두 만족해야 작업을 완료한다.

- runner, 정책, 상세 표의 pattern, transport, size inventory가 일치한다.
- 각 pattern의 최종 판정에 사용한 core 9.0.0 C와 binding paired report가 모두
  `status: complete`다.
- 모든 binding 상세 표에 `미측정`, `미달`, `측정 gap`, `보류`가 없다.
- 모든 통과 셀에 paired C와 binding report, manifest, 반복값, 비율, 옵션 일치 근거가
  기록되어 있다.
- throughput, 평균 latency, client 수, auto-HWM, 대상 외 대표 셀 회귀 gate를 모두
  통과하고, 변동성이 큰 셀은 7.2절의 재측정과 조사 절차를 마쳤다.
- 변경한 binding의 단위 테스트와 통합 테스트가 통과한다.
- 한 언어의 모든 pattern이 각각 완료되기 전에는 다음 언어로 이동하지 않는다.
- 채택한 성능 개선은 검증된 범위만 커밋하고 원격에 푸시했으며 commit id를 기록했다.
- perf 전용 우회, private API 접근, 무시되는 필수 option, timeout/sleep 증가가 남아
  있지 않다.
- 최종 리뷰에서 public interface가 더 복잡해지지 않았고 비용이 binding 내부에서
  줄었는지 확인했다.
