# core 0.10 bindings 라이브러리 성능 개선 계획

> 시작일: 2026-08-07
>
> 작업 브랜치: `core-0.10.0-bindings-performance`
>
> 이 브랜치에서 작업하도록 승인되었으며, 측정과 문서 변경은 WSL Ubuntu-24.04 작업영역에서 진행한다.
>
> 이 문서는 core 0.10.1을 기준으로 bindings 라이브러리 성능 개선을 처음부터
> 진행하기 위한 실행 문서다. 이전 계획 문서의 측정값과 완료 판정은 가져오지 않는다.
> 새 C 기준 결과와 각 binding의 새 결과만 이 문서에 기록한다.

## 1. 기준 버전과 시작 상태

이번 작업의 core 기준 버전은 0.10.1이다. 측정 전에 다음 세 파일의 버전이 모두 같은지
확인한다.

- `VERSION`: `LIBZLINK_VERSION=0.10.1`
- `core/CMakeLists.txt`: `project(zlink VERSION 0.10.1 ...)`
- `core/include/zlink.h`: major 0, minor 10, patch 1

`bindings/tools/local_core_runtime.sh`는 `VERSION`의 값을 이용해 GitHub의
`core/v0.10.1` release asset을 기존 release 절차로 가져오고 versioned runtime 경로를
선택한다. 따라서 파일 이름이나 `Perf runtime libzlink: ...` 경로만 보고 판정하지 않는다.
runner 또는 binding의 public version API가 보고한 실제 runtime 버전도 0.10.1인지 확인한다.

측정을 시작할 때는 Core source를 다시 build하지 않는다. `ZLINK_CORE_SOURCE=release`
(기본값) 상태에서 release prefix를 준비하고, `share/zlink/core-package-provenance.json`의
version이 0.10.1인지 확인한다. `core/build`와 현재 source 변경은 이 측정 runtime을
구성하지 않는다. 다른 버전의 local package나 오래된 runtime을 사용한 결과도 이 문서의
기준값으로 사용하지 않는다.

모든 성능 셀은 `미측정`에서 시작한다. 상세 표에는 현재 binding runner에 실제로 등록된
pattern만 포함한다. 공식 C runner에만 있고 binding runner에 없는 pattern은 이 계획의
측정 대상에서 제외한다. 이전 문서와 이전 report는 병목 후보를 찾는 참고 자료로만 사용하며,
core 0.10.1의 통과 비율이나 완료 근거로 사용하지 않는다.

언어별 pattern 목록이 다른 것은 Core C API 또는 binding public contract가 언어별로 다르다는
뜻이 아니다. 모든 binding은 같은 Core C API를 감싸지만, 각 언어의 perf runner가 현재
구현하고 등록한 측정 scenario가 다를 수 있다. 따라서 이 문서의 언어별 차이는 public API
차이가 아니라 perf runner 구현 범위의 차이로 해석한다.

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

비교 기준은 같은 core 0.10.1 runtime으로 실행한 `bindings/c/perf` 결과다. 같은 suite,
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

`doc/perf/perf/bindings-0.10.0/log/`의 과거 측정값은 달성 가능한 범위를 판단하는 참고 자료다. 과거 결과가
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
| .NET / Java | 단순 one-way | 74.9% | 87.6% |
| .NET / Java | routed one-way | 78.1% | 83.6% |
| .NET / Java | multi routed echo | 54.8% | 57.0% |
| Go | 단순 one-way | 59.4% | 68.2% |
| Go | routed one-way | 50.0% | 55.8% |
| Go | multi routed echo | 41.3% | 42.4% |
| Node | 단순 one-way | 33.6% | 36.1% |
| Node | routed one-way | 33.0% | 39.4% |
| Node | multi routed echo | 29.9% | 31.1% |

Python의 과거 full matrix는 이후 공개 계약 복구 전 구현으로 측정한 값이므로 달성 가능성
판단에서도 제외한다. C++ socket request/reply의 완료 셀은 p10 88.9%, 중앙값
96.3%였고 routed one-way와 multi routed echo도 비슷했다. 다만 현재 core 0.10.1의
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

아래 값은 `최소 기준 / 중앙값 목표`다. 언어 runtime과 binding 경계를 따로 반영하기 위해
언어를 묶지 않는다.

| 언어 | 단순 one-way | routed one-way | socket request/reply | multi routed echo |
| ------ | --------------- | ---------------- | ---------------------- | ------------------- |
| C++ | 85% / 95% | 80% / 85% | 75% / 85% | 80% / 85% |
| .NET | 64% / 85% | 75% / 80% | 50% / 70% | 50% / 70% |
| Java | 70% / 90% | 75% / 85% | 50% / 70% | 50% / 70% |
| Node | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |
| Go | 55% / 65% | 50% / 57% | 40% / 53% | 40% / 53% |
| Rust | 85% / 95% | 70% / 85% | 70% / 85% | 70% / 85% |
| Python | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |

Node의 과거 size 중앙값은 pattern 그룹에 따라 55.3~91.8%였다. 아직 최적화가 끝난
결과가 아니므로 가장 낮은 값에 맞춰 목표를 낮추지 않고 모든 pattern 그룹의 중앙값 목표를
60%로 둔다. Python의 과거 full matrix는 공개 계약 복구 전 결과이므로 목표를 낮추는
근거로 쓰지 않으며 Node와 같은 60% 중앙값 목표에서 시작한다. 이후 현재 core 0.10.1의
paired 측정과 binding 개선으로 달성 가능성을 검증한다.

.NET의 과거 size 중앙값은 multi routed echo 66.1%였고 Java는 69.8%였다. 이 값도
최적화 한계가 아니므로 request/reply와 multi routed echo의 중앙값 목표를 70%로 둔다.
Java의 단순 one-way와 routed one-way는 과거 중앙값도 각각 98.7%, 112.6%였으므로
90%, 85%를 달성 가능한 중앙값 목표로 사용한다.

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
paired 측정과 저부하 대형 셀 재측정의 중앙값은 약 54.5%와 50.10%였다. pooled snapshot과
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

core 0.10.1의 현재 multi runner 기본값을 따른다. 이전 표의 256 KiB는 제거하고
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

하나라도 다르면 해당 pattern의 paired 측정을 시작하지 않는다. 각 binding의 공식 runner에
실제로 등록된 pattern만 이 문서의 상세 표와 paired 측정 대상에 포함한다. 공식 C에만 있고
binding runner에 없는 pattern은 이 계획의 측정 대상에서 제외한다. 새 pattern이나 public API가
필요하면 별도 설계·계약 검토 후 runner와 문서를 함께 갱신한다.

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
- core 버그이면 binding에서 우회하지 않고 별도 Core release 작업으로 분리한다. 새 Core
  수정 결과를 이 계획의 측정에 반영하려면 새로운 GitHub release와 버전을 먼저 확정하고,
  모든 paired 기준을 해당 release runtime으로 다시 만든다.
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
| core | release 버전과 tag, runtime 절대 경로, package provenance |
| binding | package 버전, compiler 또는 runtime 버전 |
| host | OS, kernel, CPU model, 논리 CPU 수, memory |
| CPU 상태 | governor, CPU pinning, 측정 중 다른 고부하 작업 유무 |
| 명령 | C와 binding에 사용한 전체 명령과 성능 관련 환경 변수 |
| 조건 | suite, pattern, transport, size, duration, runs, client 수, I/O thread 수 |
| 결과 | report 경로, `status`, Effective Options, auto-HWM detail |
| pair | C와 binding에 공통으로 부여한 session tag |

Core release version/tag, package provenance, runtime, host boot, CPU governor, client 수 또는
성능 관련 환경 변수가 바뀌면 이전 C 결과와 새 binding 결과를 짝지어 판정하지 않는다.
binding before와 after 사이에는 검토 중인 변경만 있어야 하며 변경 파일을 manifest에
기록한다. 그 밖의 조건이 바뀌면 같은 manifest 조건으로 C를 다시 제한 측정한다.

## 7. 실행 절차

공식 entrypoint만 사용한다.

- C single: `bindings/c/perf/run_benchmarks.sh`
- C multi: `bindings/c/perf/run_benchmarks_multi.sh`
- binding single: `bindings/<lang>/perf/run_benchmarks.sh`
- binding multi: `bindings/<lang>/perf/run_benchmarks_multi.sh`

### 7.0 측정 단위와 순차 실행

전체 matrix를 먼저 실행해 기준값을 만드는 방식은 사용하지 않는다. 한 번에 하나의
`binding + suite + pattern + transport` 조합만 비교 대상으로 선택한다. 먼저
`bindings/c/perf`에서 같은 pattern과 transport를 같은 message size, duration, runs,
client 수, I/O thread 수, Core release runtime으로 측정하고, C report가
`status: complete`이면 같은 session tag와 조건으로 해당 binding runner를 바로 측정한다.

선택한 조합의 C와 binding 결과를 비교해 병목과 개선 대상을 정한 뒤, 구현 변경 후에도
같은 조합을 C와 binding 순서로 다시 paired 측정한다. 다른 pattern이나 transport의
결과를 미리 측정하거나, 전체 matrix 결과를 현재 조합의 C 기준으로 재사용하지 않는다.
다른 조합을 확인할 필요가 생기면 기존 측정을 확장하지 않고 새 paired 대상으로 별도로
선택해 같은 절차를 반복한다.

perf 실행은 항상 직렬화한다. C runner, binding runner, 후보 after 측정 중 어느 것도
동시에 실행하지 않으며, 한 runner process의 report가 종료되고 `status: complete`인지
확인한 뒤 다음 하나의 측정을 시작한다. 백그라운드에서 다른 perf process를 함께 실행해
host CPU·memory·I/O 부하를 섞지 않는다.

### 7.0.1 `PERF_SINGLE_TEST_POLICY` parity gate

비교 기준은 binding runner의 현재 구현이 아니라
`doc/perf/PERF_SINGLE_TEST_POLICY.md`와 그 문서를 반영한 `bindings/c/perf`
canonical reference runner다. 선택한 하나의 비교 대상은 다음 의미가 C와 binding에서
동일해야 유효한 paired 결과로 인정한다.

- `ready -> active(duration)` 순서와 active payload header의 수집 범위
- active 송신의 blocking 의미, transient 오류 후 새 timestamp·1ms retry, stop token의
  wire-level 종료와 bounded retry
- receiver의 `POLLIN` readiness 대기(`-1` timeout)와 `DONTWAIT` drain
- throughput, 평균 latency, p95/p99의 산출 방식과 runs 중앙값 집계
- latency sample cap 기본값 1,000,000, `0`일 때 percentile sample 미보관, 전체 count·sum
  집계, 동일한 bounded reservoir 교체 알고리즘과 percentile 보간
- Core release runtime, auto-HWM message unit, I/O thread 수, client 수와 timeout

위 항목 중 하나라도 C와 binding에서 다르면 수치는 비교 자료로만 남기고 기준값이나
통과 판정에 사용하지 않는다. 정책과 reference runner를 수정한 경우에는 같은
`binding + suite + pattern + transport` 대상의 C와 binding을 다시 순서대로 측정한다.
poller API의 내부 primitive가 다르더라도 readiness, drain, 종료와 metric 의미가 같으면
비교할 수 있다. 반대로 retry 대기나 stop-token 종료 조건이 다르면 수치를 공식 비교에
사용하지 않는다.

이번 `PAIR / inproc / 64B` parity audit에서 C++ active 송신이
`send_flags_t::dontwait`로 실행되어 C reference의 blocking 송신과 달랐던 문제를
확인했다. C++ 송신을 `send_flags_t::none`으로 맞추고, C reference의 수신도 정책에
맞춰 public poller의 `POLLIN` 대기와 `DONTWAIT` drain을 사용하도록 고쳤다. C++의
runtime binary mapping이 실제 `cpp_perf_*` 산출물과 어긋나던 문제도 수정했다. 이후
C++ active 송신의 transient 오류 처리에 C와 같은 1ms 대기와 재-stamp를 적용하고,
PAIR stop token에도 같은 환경 변수(`PERF_SINGLE_STOP_RETRY_TIMEOUT_MS`) 기반의
bounded retry를 적용했다. 이 수정 전 report는 정책 parity를 충족하지 않으므로 공식
비교 결과에서 제외한다. 특히 parity 수정 전 C++ active 송신은 `DONTWAIT`였고 C
reference는 blocking 송신이었으므로, 당시 C++가 C 대비 90% 이상으로 보인 결과는
같은 의미의 비교가 아니다. C++의 blocking active 송신으로 다시 만든 결과만 공식
비교에 사용한다.

이전 `zlink_poll()` fast-path A/B는 65536B에서 70.00%로 악화되어 해당 라운드에서는
제거했다. 이후 C++ `poller_t`의 socket-only wait가 등록된 `zlink_poller_wait()`를
사용하도록 유지하는 구현을 다시 검증했다. 65536B 반복 median은 약 96~115K에서
121~124K로 개선되는 신호가 있었지만, 최신 5회 결과의 변동성 gate와 95% 목표를
충족하지 못했으므로 성능 통과로 기록하지 않는다. 정책에서 요구하는 readiness와
drain 의미는 유지하되, 구현 primitive가 같다는 이유만으로 성능 후보를 채택하지 않는다.

추가 audit에서 C++ Single latency sampler가 C reference와 달리 모든 sample을 무제한으로
보관하고 기본 cap을 200,000으로 사용하며, `0`을 허용하지 않고 p95/p99를 mean 이상으로
보정하는 문제를 확인했다. C와 같은 bounded reservoir sampler, 기본 cap 1,000,000,
`0` sample 미보관, count·sum 전체 집계와 percentile 계산으로 수정했다. 이 sampler
parity 수정 전 C++ report는 공식 비교에서 제외하고, 수정 후 같은 선택 대상의 C report와
binding report를 다시 순서대로 측정한다. 최신 large-message profile에서는 C++ sender
63.7%, C sender 65.5%, poller와 close 비용도 양쪽에서 비슷한 비중으로 나타났다.
`message_t::from()`의 payload copy와 Core send/receive는 공통 경로이므로, public
contract와 ownership을 유지하면서 제거할 수 있는 C++ 전용 hot path는 확인하지 못했다.
`direct socket.send(message_t&, flags)`는 정식 public contract가 아니고 raw builder
고유 비용도 약 1.66%에 그쳐 후보로 실행하지 않았다. 추가 allocator, buffer reuse 또는
receive object reuse는 측정 의미나 소유권을 바꾸므로 보류한다.

### 7.1 Pattern별 smoke와 제한 사전 점검

전체 pattern이나 전체 matrix를 한 번에 실행해 기준값을 만들지 않는다. 현재 언어에서
진행할 pattern과 transport 하나를 선택한 뒤 C와 binding의 같은 조합만 smoke한다. 이렇게
하면 서로 다른 조합을 측정하는 동안 생기는 host 부하와 시간 차이가 현재 비교값에 섞이지
않는다.

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
- binding before와 after는 같은 Core release runtime과 host session을 사용하고,
  검토 중인 binding 변경만 다르게 유지한다.
- Core release version/tag, package provenance, runtime, host boot 또는 성능 환경이 달라지면
  C를 다시 측정한다.
- 개선 작업이 길어졌거나 host 부하가 달라졌으면 후보 최종 판정 직전에 같은 C pattern을 다시
  측정한다.
- 목표 기준 ±5%p 셀은 이전 측정값 하나만으로 판정하지 않는다.
- paired report 중 하나라도 `status: complete`가 아니면 표를 갱신하지 않는다.

### 7.4 작업 순서

1. inventory gate를 통과시키고 정책, runner, 상세 표의 측정 범위를 일치시킨다.
2. Core 0.10.1 release asset을 확보하고 release provenance와 재현 환경 manifest를 기록한다.
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
    다시 대조한다. 미측정, 미달, 보류가 하나라도 있으면 다음 언어로 이동하지 않는다.
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
경로는 측정 기록과 결과 표에 남긴다. 후보를 적용·기각한 과정은 이 문서와 같은 폴더의
`log/`에 남긴다. 다음 상태는 커밋 대상으로 인정하지 않는다.

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

- `미측정`: 같은 조건의 core 0.10.1 C 결과와 binding 결과를 아직 비교하지 않았다.
- `통과(비율%)`: throughput, latency, 변동성, 회귀, Effective Options, auto-HWM,
  client 수 조건을 모두 만족한다.
- `미달(비율%)`: 유효한 결과가 있지만 목표에 도달하지 못했고 내부 개선이 필요하다.
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
우선한다. 상세 표에 `미측정`, `미달`, `보류`가 하나라도 남아 있으면
해당 언어는 완료가 아니다.

### 9.1 C++

- perf 경로: `bindings/cpp/perf`
- Single 상태: `진행 중` — `PAIR / inproc`, `PAIR / tcp`, `PAIR / ws`, `PUBSUB / tcp`,
  `PUBSUB / ws`, `PUBSUB / wss`, `PUBSUB / tls`, `PUBSUB / inproc`, `PUBSUB / ipc`,
  `DEALER_DEALER / tcp`, `DEALER_DEALER / ws`, `DEALER_DEALER / wss`,
  `DEALER_DEALER / tls`, `DEALER_DEALER / inproc`, `DEALER_DEALER / ipc`,
  `DEALER_ROUTER / tcp`의 선택된 size paired 측정을 현재 소스
  기준으로 완료했지만, 전체
  Single transport·pattern 완료
  판정은 아직 하지 않았다. `socket_access_t::native_handle()` inline과 등록된
  `zlink_poller_wait()` 사용은 private implementation 최적화로 유지했다. contract
  source-layout 검증에서 `message_t` accessor inline 후보는 제거하고 원래 out-of-line
  public contract로 복구했다. `PAIR / inproc`의 최신 contract-clean size별 ratio는
  89.97%, 99.60%, 93.02%, 24.77%, 67.02%, 73.84%이고 size 중앙값은 81.91%다.
  `PAIR / tcp` full sweep의 ratio는 94.82%, 95.78%, 98.91%, 87.80%, 91.09%,
  97.02%이고 size 중앙값은 95.30%다. `PAIR / tcp` 64B는 세 번의 paired 재검증에서도
  C/C++ 변동 폭이 11.66%/11.32%로 남아 transport 완료를 보류한다. `PAIR / ws`의
  ratio는 96.22%, 97.97%, 99.80%, 95.27%, 94.38%, 98.33%이고 size 중앙값은
  97.10%다. WS 64B의 C/C++ 변동 폭은 11.04%/10.00%였지만 C++ 전용 병목 근거는
  없었고 `sol` 리뷰에서 source 변경을 진행하지 않는 결론을 확인했다. 이전 C++ 90%
  이상 결과는 C의 blocking active 송신과 달리 `DONTWAIT`를 사용한 정책 불일치
  결과이므로 공식 근거에서 제외한다. `PAIR / wss`의 ratio는 94.89%, 97.93%,
  99.24%, 98.76%, 97.99%, 98.46%이고 size 중앙값은 98.23%다. 262144B는 두 번의
  boundary revalidation에서 C++ 변동 폭이 10.47%와 14.55%로 남았지만 ratio는
  각각 100.01%와 90.22%였고, 다른 셀은 안정적이었다. WSS의 encryption, WebSocket
  framing, Core I/O가 비용을 지배하며 `sol` 리뷰에서도 source 변경 no-go를 확인했다.
  `PAIR / tls` full sweep의 ratio는 93.79%, 96.17%, 99.00%, 91.19%, 92.72%,
  86.04%이고 size 중앙값은 93.25%다. 모든 개별 셀은 85% 이상이지만 size 중앙값
  목표 95%에는 미달하므로 transport 완료로 판정하지 않는다. 64B와 262144B
  boundary revalidation의 ratio는 각각 Round 1에서 88.89%와 82.36%, Round 2에서
  92.25%와 86.88%였다. TLS/262144B Release profile에서 page fault가 C 19,816 대
  C++ 270,185로 관측되어 pooled range 전체를 Core native allocation으로 바꾸는
  일반 A/B 후보를 검증했지만, C++ throughput 개선은 기존 기준 대비 3.33%에 그쳤고
  page fault도 275,132로 줄지 않아 후보를 제거했다. TLS의 SSL read/write,
  encryption, Core I/O가 비용을 지배하며 `message_t::from()` 자체를 우회하는
  size-specific 경로는 만들지 않는다.
  `PAIR / ipc` full sweep의 ratio는 86.62%, 96.75%, 94.41%, 87.76%, 91.42%,
  97.95%이고 size 중앙값은 92.91%다. 모든 개별 셀은 최소 기준을 넘었지만 중앙값
  목표 95%에는 미달한다. 65536B Release profile에서 C++ `message_t::from(span)`에
  귀속된 32.11%는 native message allocation과 payload copy를 포함하며 C reference의
  같은 작업은 inline 또는 anonymous libc stack으로 보인다. raw builder 3.68%를 모두
  제거해도 중앙값 목표를 해결하지 못하고, public ownership·builder 계약을 유지하면서
  제거할 일반 binding 전용 병목을 확인하지 못했다. `sol` follow-up에서 IPC source
  optimization은 no-go로 판정했다.
  기존 `PUBSUB` C++ paired 결과는 subscriber를 200ms blocking receive로 읽어 C 기준의
  public poller 의미와 달랐으므로 최종 비교에서 제외한다. C++ harness를 persistent
  public `poller_t`의 `wait(-1)` 뒤 `recv_flags_t::dontwait` drain으로 수정하고,
  `topic_message_t`를 drain loop 밖에서 재사용했다. 이 수정은 public API, wire payload,
  ownership과 집계 의미를 바꾸지 않으며, 이후 수치는 모두 수정된 harness의 결과다.
  `subscription_reader.hpp`의 single-part 수신 성공 경로는 기존
  `lazy_message_parts_t::replace(message_t)`를 사용하는 private friend helper를
  유지한다. `PUBSUB / tcp` 재측정 ratio는 90.38%, 99.55%, 103.99%, 89.70%, 93.15%,
  95.96%이고 size 중앙값은 94.56%로 미달이다. 모든 개별 셀은 85% 이상이며 추가
  binding hot path 근거가 없어 source 변경은 하지 않는다.
  `PUBSUB / ws` 재측정 ratio는 84.17%, 93.47%, 102.77%, 94.58%, 94.85%, 96.92%이고
  size 중앙값은 94.71%다. 64B 개별 기준과 중앙값 목표가 미달이며, WebSocket framing,
  Core poller와 공통 publisher 비용이 중심이므로 기존 replacement 후보 외 추가 변경은
  하지 않는다.
  `PUBSUB / wss` 재측정 ratio는 92.00%, 100.79%, 99.83%, 98.08%, 98.78%, 97.22%이고
  size 중앙값은 98.43%다. 모든 개별 셀이 85% 이상이고 transport 목표를 통과했다.
  WSS encryption·WebSocket framing·Core I/O가 비용을 지배하므로 추가 source 변경은
  하지 않는다.
  `PUBSUB / tls` 재측정 ratio는 89.83%, 101.92%, 97.85%, 95.67%, 92.12%, 88.19%이고
  size 중앙값은 93.89%다. 모든 개별 셀은 85% 이상이지만 중앙값 목표에는 미달하며,
  TLS encryption과 Core I/O가 비용을 지배하므로 추가 source 변경은 하지 않는다.
  `PUBSUB / inproc` 재측정 ratio는 93.17%, 93.27%, 98.13%, 31.59%, 61.80%, 70.10%이고
  size 중앙값은 81.64%다. 64B·256B·1024B만 개별 기준을 통과하고 65536B 이상은
  크게 미달했다. 정책 정렬 Release profile에서 C++ `read_subscription_message`는
  2.10%, `zlink_subscribe_part`는 2.94%, `malloc`은 3.36%였고 C/C++ 공통의
  anonymous libc·Core mutex·allocation·poller 비용이 중심이었다. Sol review는
  direct receive, allocator와 builder 변경을 no-go로 판정했으므로 추가 source 변경은
  하지 않는다.
- Multi 상태: `미측정`
- 다음 작업: C++ PUBSUB harness는 C 기준과 같은 persistent public poller
  `wait(-1) → DONTWAIT drain` 의미로 정렬했고 `topic_message_t`도 drain loop 밖에서
  재사용한다. DEALER_DEALER receiver도 같은 poller parity 의미로 정렬했다. 이 변경은
  public API·wire payload·ownership을 바꾸지 않으므로 유지한다. `DEALER_DEALER / ws`
  공식 결과는 size 중앙값 92.08%로 미달했고 typed socket A/B도 전체 sweep에서
  개선을 재현하지 못해 제거했다. `DEALER_DEALER` transport sweep는 완료했다.
  `PUBSUB / tcp`는 ratio 90.38%, 99.55%, 103.99%, 89.70%, 93.15%, 95.96%와
  size 중앙값 94.56%로 미달했고, `PUBSUB / ws`는 84.17%, 93.47%, 102.77%,
  94.58%, 94.85%, 96.92%와 size 중앙값 94.71%로 미달했다. `PUBSUB / wss`는
  92.00%, 100.79%, 99.83%, 98.08%, 98.78%, 97.22%와 size 중앙값 98.43%로
  통과했고, `PUBSUB / tls`는 89.83%, 101.92%, 97.85%, 95.67%, 92.12%, 88.19%와
  size 중앙값 93.89%로 미달했다. 이전 poller 불일치 결과는 모두 공식 비교에서
  제외한다. WSS/TLS의 encryption·WebSocket·Core I/O와 TCP/WS의 공통 poller·socket
  비용이 중심이라 추가 source 변경은 하지 않는다.
  `PUBSUB / inproc`은 정책 정렬 후 ratio 93.17%, 93.27%, 98.13%, 31.59%, 61.80%,
  70.10%와 size 중앙값 81.64%로 미달했다. C++ `read_subscription_message` 2.10%,
  `zlink_subscribe_part` 2.94%, `malloc` 3.36% 외에는 C/C++ 공통 비용이 profile을
  지배했고, `sol` review는 direct receive·allocator·builder 변경을 no-go로 판정했다.
  `PUBSUB / ipc`는 ratio 87.53%, 98.12%, 100.53%, 90.90%, 95.94%, 95.92%와
  size 중앙값 95.93%로 통과했다. `ws` 64B boundary 재검증은 88.74%로 개별 기준을
  통과했지만 전체 size 중앙값은 94.71%로 미달했고, `tcp` boundary 재검증도 93.93%로
  미달을 재현했다. WS profile과 TCP 65536B profile에서 mutex·clock·Core pipe/poller가
  공통으로 지배했으며, `sol` review는 `replace_single()`과 poller parity를 유지하고
  raw publish state·direct publish overload·allocator 변경을 no-go로 판정했다.
  `DEALER_DEALER / tcp`는 C++ receiver를 C 기준의 public poller `wait(-1)` 뒤
  `DONTWAIT` drain으로 보정한 다음 6개 size를 paired 측정했다. ratio는 90.88%,
  93.58%, 93.96%, 84.05%, 85.44%, 90.42%이고 size 중앙값은 90.65%다. 65536B
  Release profile에서 C 447 samples, C++ 419 samples를 비교한 결과 양쪽 모두
  payload allocation/copy와 Core TCP I/O가 지배했다. `sol`은 typed `dealer_socket_t`
  전환을 안전한 선택적 harness A/B로 인정했지만 variant 비용이 독립 hotspot으로
  확인되지 않았고, 목표까지 필요한 13.03% 개선을 설명하지 못하므로 binding source
  개선 후보로는 no-go 판정했다.
  `DEALER_DEALER / ws`도 같은 public poller parity harness를 사용해 C → C++ 순서로
  6개 size를 paired 측정했다. 공식 adapter 결과의 ratio는 87.31%, 88.77%, 94.07%,
  91.85%, 92.32%, 95.31%이고 size 중앙값은 92.08%다. C++ typed `dealer_socket_t`
  경로는 선택적 harness A/B로 64B 92.93%, 65536B 96.28%, 131072B 94.74%를
  기록했지만, 같은 6개 size 전체 sweep에서는 88.07%, 91.51%, 91.27%, 94.80%,
  88.58%, 93.72%, 중앙값 91.39%로 개선이 재현되지 않았다. typed 경로는 제거하고
  기존 adapter harness로 복구했다. WS WebSocket framing, Core I/O와 공통 poller 비용이
  지배하며 추가 binding source 변경은 no-go로 판정했다.
  `DEALER_DEALER / wss`는 같은 harness로 6개 size를 paired 측정했다. ratio는 89.02%,
  86.92%, 87.13%, 93.92%, 95.70%, 86.19%이고 size 중앙값은 88.07%다. 65536B
  Release profile은 C 769 samples, C++ 774 samples를 기록했고 WSS encryption,
  WebSocket framing, Core I/O가 비용을 지배하는지 확인하는 결과로 사용한다. 추가
  WSS profile은 C 769 samples, C++ 774 samples를 기록했다. C++/C profile 처리량은
  98.76%였고, WSS framing·OpenSSL·Asio·Core I/O와 양쪽에 공통인 payload allocation/copy가
  비용을 지배했다. 목표까지 필요한 7.87% 개선을 설명할 binding 전용 hotspot은 확인되지
  않았으므로 source optimization은 no-go로 판정한다. `DEALER_DEALER / tls`의 ratio는
  94.19%, 95.85%, 98.68%, 93.59%, 90.37%, 85.33%이고 size 중앙값은 93.89%다.
  모든 개별 셀은 85% 이상이지만 transport 목표 95%에는 미달한다. TLS encryption,
  Core I/O와 공통 payload allocation/copy가 주요 비용이므로 추가 binding source 변경은
  하지 않는다.
  `DEALER_DEALER / inproc`의 ratio는 89.15%, 102.63%, 100.59%, 23.90%, 64.94%,
  68.80%이고 size 중앙값은 78.97%다. 65536B 재검증도 ratio 21.03%와 C++ 변동 폭
  317.8%로 안정성 gate를 통과하지 못했다. WSL에 `perf` 실행 파일이 없어 profile은
  수집하지 못했으며, Sol은 payload copy·message lifetime을 우회하는 변경과 typed
  dispatch 제거를 binding-only 개선 근거로 인정하지 않았다. 추가 source 변경은 하지 않고
  다음 선택 대상은 `DEALER_DEALER / ipc`로 둔다.
  `DEALER_DEALER / ipc`는 ratio 93.81%, 93.87%, 92.19%, 83.87%, 87.83%, 92.72%와
  size 중앙값 92.45%로 미달한다. 1024B·256KiB·64KiB의 안정적인 셀도 목표에 미달했고,
  이전 IPC profile에서 확인한 encoder·decoder·queue·poller 공통 비용 외에 안전한
  binding-only hotspot은 확인되지 않았다. Sol review는 source optimization을 no-go로
  판정했다.
  `DEALER_ROUTER / tcp`는 C와 C++ 모두 public poller `wait(-1)`, routed
  `DONTWAIT` drain, wire-level stop token과 recv별 새 message 수명을 사용한 정책 정렬
  결과다. ratio는 82.99%, 98.00%, 98.29%, 85.24%, 87.99%, 94.93%이고 size 중앙값은
  91.46%다. C++ 변동 폭은 64B 20.2%, 256B 17.1%, 1024B 21.2%, 65536B 19.4%,
  131072B 14.6%, 262144B 15.3%로 안정성 gate를 초과했다. C++ ROUTER의 public
  single-part receive와 Core TCP I/O가 비용을 지배하며 binding-only source 후보는
  확인되지 않아 no-go로 판정했다.
  `DEALER_ROUTER / ws`도 같은 정책 정렬 상태에서 ratio 87.85%, 89.52%, 99.61%,
  93.71%, 92.05%, 97.13%, size 중앙값 92.88%로 미달했다. 평균 latency 비율은
  1.067배, 1.042배, 1.009배, 1.075배, 1.081배, 1.027배로 C++ 목표 2.0배 이내이며,
  C++ throughput 변동 폭은 9.1%, 14.0%, 12.7%, 13.9%, 16.7%, 4.6%다. WebSocket
  framing, Core I/O와 public routed receive 비용이 중심이고 binding-only source 후보는
  확인되지 않아 no-go로 판정했다.
  `DEALER_ROUTER / wss` full sweep ratio는 84.52%, 97.28%, 98.28%, 99.43%, 97.65%,
  87.18%, size 중앙값은 97.46%다. 64B full sweep은 84.52%였지만 boundary 재검증에서
  C/C++ median 1,530,693.8/1,390,285.6 msg/s, ratio 90.83%, 변동 폭 4.5%/5.7%로
  안정성과 개별 기준을 충족했다. 평균 latency 비율은 full sweep에서
  1.174배/0.983배/1.068배/1.008배/1.023배/1.165배로 C++ 목표 2.0배 이내다.
  WSS encryption, WebSocket framing, Core I/O와 public routed receive 비용이 중심이고
  binding-only source 후보는 확인되지 않아 no-go로 판정했다. 다음 선택 대상은
  `DEALER_ROUTER / tls`다.
  `DEALER_ROUTER / tls` 최초 paired 결과는 ratio 77.47%, 95.96%, 96.43%, 92.65%,
  93.75%, 88.58%, size 중앙값 93.20%였고 64B throughput, 256B 평균 latency와 작은
  셀의 변동성이 gate를 넘었다. 전체 size 재검증 후 ratio는 73.67%, 93.26%, 91.18%,
  91.09%, 89.59%, 81.33%, size 중앙값 90.34%로 미달을 재현했다. 재검증 평균 latency
  비율은 1.708배/1.813배/0.607배/1.098배/1.120배/1.229배였지만 C++ throughput
  변동 폭은 47.5%, 31.7%, 31.0%, 24.3%, 16.2%, 20.5%로 모든 size에서 안정성 gate를
  초과했다. TLS encryption, Core I/O와 queue latency가 비용을 지배하며 binding-only
  source 후보는 확인되지 않아 no-go로 판정했다. 다음 선택 대상은
  `DEALER_ROUTER / inproc`다.
  `DEALER_ROUTER / inproc` full sweep ratio는 82.15%, 87.87%, 87.46%, 33.80%,
  63.11%, 68.82%, size 중앙값 75.49%다. 64B·256B·1024B·65536B boundary 재검증은
  각각 85.68%, 93.39%, 92.89%, 26.72%였고, 65536B 평균 latency 비율은 2.375배,
  C++ 변동 폭은 2.2%, 14.0%, 14.0%, 130.3%였다. 128KiB·256KiB의 full sweep도
  63.11%·68.82%로 안정적인 미달이다. inproc allocation/copy, mutex, pipe와 Core
  polling 비용이 공통으로 지배하며 binding-only source 후보는 확인되지 않아 no-go로
  판정했다. 다음 선택 대상은 `DEALER_ROUTER / ipc`다.
  `DEALER_ROUTER / ipc` full sweep ratio는 76.98%, 92.83%, 98.78%, 82.71%,
  84.56%, 89.69%, size 중앙값 87.13%다. Sol 권고에 따른 64B·256B·1024B·65536B·
  131072B boundary 재검증은 81.99%, 93.76%, 96.51%, 85.35%, 85.86%였고, C++
  변동 폭은 10.64%, 7.68%, 10.14%, 11.35%, 5.32%였다. 256KiB full sweep ratio는
  89.69%, 변동 폭은 4.56%로 안정적이지만, 64B와 64KiB 개별 기준 및 size 중앙값이
  미달한다. IPC encoder/decoder, queue, Core polling·I/O 비용이 공통으로 지배하며
 binding-only source 후보는 확인되지 않아 no-go로 판정했다. 다음 선택 대상은
 `DEALER_ROUTER_REQREP / tcp`다.
  `DEALER_ROUTER_REQREP / tcp` 최초 C++ report는 generic Router monitor wait 때문에
  30개 셀이 모두 실패해 공식 비교에서 제외했다. C++ setup을 C canonical과 같은 Router
  activity-driven monitor wait로 수정한 뒤 paired ratio는 95.10%, 93.51%, 96.72%,
  94.99%, 95.76%, 93.89%, size 중앙값 95.05%였다. Sol 권고에 따른 1024B·65536B
  boundary 재검증은 각각 94.64%, 95.73%였고, 6-size 재계산 중앙값은 94.87%로
  transport 목표 95%에 미달한다. latency 비율은 모두 1.057배 이내이고 boundary
  C/C++ 변동 폭도 7.38%/4.69%, 2.35%/5.09%로 안정됐지만, binding-only hot-path
  후보는 확인되지 않았다. 수정은 측정 전 setup parity correction이며 performance
  optimization으로 계산하지 않고, 다음 선택 대상은 `DEALER_ROUTER_REQREP / ws`다.

#### 9.1.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미달 (94.82%) | 통과 (95.78%) | 통과 (98.91%) | 통과 (87.80%) | 통과 (91.09%) | 통과 (97.02%) | full sweep size 중앙값 95.30%다. 256B·1024B·262144B는 boundary revalidation에서 C/C++ 변동 폭 8.44%/4.38%, 6.04%/3.89%, 1.48%/4.90%로 안정됐고, 64B는 세 번째 재검증에서도 11.66%/11.32%로 gate를 넘었다. |
| `tcp` | `PUBSUB` | 통과 (90.38%) | 통과 (99.55%) | 통과 (103.99%) | 통과 (89.70%) | 통과 (93.15%) | 통과 (95.96%) | full sweep 중앙값 94.56%에 이어 boundary 재검증도 94.56%, 100.50%, 105.08%, 91.59%, 93.28%, 93.29%, 중앙값 93.93%로 미달을 재현했다. 65536B profile에서 C++ `read_subscription_message` 1.68%, `pub_socket_t::publish()` 1.68%였지만 공통 libc·Core I/O 비용이 중심이고, `sol`은 raw publish state를 ABI·topic lifetime·효과 근거 부족으로 reject했다. C boundary: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_173337_pubsub-tcp-poller-parity-boundary-revalidate-c1.txt`; C++ boundary: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_173646_pubsub-tcp-poller-parity-boundary-revalidate-c1.txt`; profile C/C++: `/tmp/zlink-perf-tools-0070B1/profile-latest/pubsub-tcp-c-65536-poller-parity.data`, `/tmp/zlink-perf-tools-0070B1/profile-latest/pubsub-tcp-cpp-65536-poller-parity.data` |
| `tcp` | `DEALER_DEALER` | 통과 (90.88%) | 통과 (93.58%) | 통과 (93.96%) | 미달 (84.05%) | 통과 (85.44%) | 통과 (90.42%) | size 중앙값 90.65%로 transport 목표 95%에 미달한다. C++ receiver poller parity 후의 공식 결과다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_175728_dealer-dealer-tcp-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_180006_dealer-dealer-tcp-poller-parity-c1.txt`; profile C/C++: `/tmp/zlink-perf-tools-0070B1/profile-latest/dealer-dealer-tcp-c-65536-poller-parity.data`, `/tmp/zlink-perf-tools-0070B1/profile-latest/dealer-dealer-tcp-cpp-65536-poller-parity.data`; sol review: `4918ccdb-54ec-4449-b93f-6873941bacde` |
| `tcp` | `DEALER_ROUTER` | 미달 (82.99%) | 통과 (98.00%) | 통과 (98.29%) | 통과 (85.24%) | 통과 (87.99%) | 통과 (94.93%) | 정책 parity 후 size 중앙값 91.46%로 transport 목표 95%에 미달하고 C++ 변동 폭도 모든 size에서 10% gate를 초과했다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_195450_dealer-router-tcp-policy-c3.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_195728_dealer-router-tcp-policy-c3.txt`; Sol review: `33c013c9-a49f-4934-8311-4dd6ac3b8d62` |
| `tcp` | `DEALER_ROUTER_REQREP` | 통과 (95.10%) | 통과 (93.51%) | 통과 (94.64%, 재검증) | 통과 (95.73%, 재검증) | 통과 (95.76%) | 통과 (93.89%) | 구형 C++ report는 Router monitor wait 불일치로 30개 셀이 실패해 제외했다. setup parity fix 후 full sweep size 중앙값은 95.05%였지만 1024B·65536B boundary 재검증을 반영한 공식 중앙값은 94.87%로 transport 목표 95%에 미달한다. latency ratio는 1.022배/1.057배/0.942배/1.049배/1.036배/1.047배이고 boundary C/C++ throughput 변동 폭은 7.38%/4.69%, 2.35%/5.09%다. setup fix는 측정 전 Router activity-driven monitor wait를 C 기준에 맞춘 parity correction이며 binding-only hot-path optimization으로 계산하지 않는다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_211628_dealer-router-reqrep-tcp-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_212518_dealer-router-reqrep-tcp-policy-c2.txt`; 1024B boundary C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_213036_dealer-router-reqrep-tcp-boundary-1024-c1.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_213108_dealer-router-reqrep-tcp-boundary-1024-c1.txt`; 65536B boundary C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_213143_dealer-router-reqrep-tcp-boundary-65536-c1.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_213219_dealer-router-reqrep-tcp-boundary-65536-c1.txt`; Sol review: `4ee8f728-da3d-4d98-a8ec-b9c0892557f1` |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 통과 (96.22%) | 통과 (97.97%) | 통과 (99.80%) | 통과 (95.27%) | 통과 (94.38%) | 통과 (98.33%) | full sweep size 중앙값 97.10%다. C/C++ 변동 폭은 64B 11.04%/10.00%, 256B 6.41%/7.74%, 1024B 4.73%/5.06%, 65536B 3.78%/5.62%, 131072B 3.02%/5.01%, 262144B 3.77%/2.32%다. 64B C 변동 폭은 10%를 조금 넘었지만 C++ 전용 병목 근거가 없고 `sol` 리뷰에서 source 변경 후보를 no-go로 판정했다. |
| `ws` | `PUBSUB` | 미달 (84.17%) | 통과 (93.47%) | 통과 (102.77%) | 통과 (94.58%) | 통과 (94.85%) | 통과 (96.92%) | full sweep 중앙값은 94.71%로 미달이고 64B 개별 기준도 미달했지만, boundary 재검증에서 C/C++ 1,030,946.8/914,824.8 msg/s, ratio 88.74%로 64B 기준을 통과했다. 중앙값을 구성한 65536B·131072B도 C/C++ 24,084.4/22,329.4 및 15,829.4/14,936.6 msg/s, ratio 92.71%/94.36%로 미달을 재현했다. profile은 C/C++ 모두 mutex·clock·Core pipe/poller가 중심이고 C++ `read_subscription_message`는 1.59%뿐이었다. `sol`은 추가 source optimization을 no-go로 판정했다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_165343_pubsub-ws-poller-parity-c.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_165651_pubsub-ws-poller-parity-cpp.txt`; C/C++ 64B boundary: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_172915_pubsub-ws-64-poller-parity-boundary-revalidate-c1.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_172951_pubsub-ws-64-poller-parity-boundary-revalidate-c1.txt`; C/C++ median cells: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_174812_pubsub-ws-median-cells-poller-parity-revalidate-c1.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_174919_pubsub-ws-median-cells-poller-parity-revalidate-c1.txt`; profile C/C++: `/tmp/zlink-perf-tools-0070B1/profile-latest/pubsub-ws-c-64-poller-parity.data`, `/tmp/zlink-perf-tools-0070B1/profile-latest/pubsub-ws-cpp-64-poller-parity.data` |
| `ws` | `DEALER_DEALER` | 통과 (87.31%) | 통과 (88.77%) | 통과 (94.07%) | 통과 (91.85%) | 통과 (92.32%) | 통과 (95.31%) | full sweep size 중앙값 92.08%로 transport 목표 95%에 미달한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_181249_dealer-dealer-ws-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_181529_dealer-dealer-ws-poller-parity-c1.txt`; 64B profile C/C++: `/tmp/zlink-perf-tools-0070B1/profile-latest/dealer-dealer-ws-c-64-poller-parity.data`, `/tmp/zlink-perf-tools-0070B1/profile-latest/dealer-dealer-ws-cpp-64-poller-parity.data`; typed A/B full sweep C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_182802_dealer-dealer-ws-typed-c1.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_183040_dealer-dealer-ws-typed-c1.txt`; typed full sweep size 중앙값 91.39%로 개선을 재현하지 못해 typed 경로는 제거했다. Sol review: `51bb1842-4485-4bd0-91e2-8b9e22998d37` |
| `ws` | `DEALER_ROUTER` | 미달 (87.85%) | 미달 (89.52%) | 통과 (99.61%) | 통과 (93.71%) | 통과 (92.05%) | 통과 (97.13%) | size 중앙값 92.88%, 평균 latency 비율 1.067배/1.042배/1.009배/1.075배/1.081배/1.027배다. C 변동 폭은 7.6%/12.5%/7.6%/2.9%/3.0%/3.3%, C++ 변동 폭은 9.1%/14.0%/12.7%/13.9%/16.7%/4.6%다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_200556_dealer-router-ws-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_200838_dealer-router-ws-policy-c1.txt`; Sol review: `4b2cf27a-87c1-4143-8aaf-117b3831f102` |
| `ws` | `DEALER_ROUTER_REQREP` | 통과 (92.00%) | 통과 (88.68%) | 통과 (89.73%) | 통과 (88.55%) | 통과 (98.67%) | 통과 (93.61%) | full sweep size 중앙값은 90.86%로 transport 목표 95%에 미달한다. C/C++ throughput 변동 폭은 9.27%/11.08%, 10.27%/9.47%, 11.65%/10.51%, 6.96%/7.54%, 4.64%/10.64%, 1.67%/8.69%다. latency ratio는 1.072배/1.110배/1.120배/1.127배/1.010배/1.058배다. Sol review와 boundary 재검증 여부를 확인 중이다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_214653_dealer-router-reqrep-ws-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_214934_dealer-router-reqrep-ws-policy-c1.txt` |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 통과 (94.89%) | 통과 (97.93%) | 통과 (99.24%) | 통과 (98.76%) | 통과 (97.99%) | 통과 (98.46%) | full sweep size 중앙값 98.23%다. C/C++ 변동 폭은 64B 8.10%/2.35%, 256B 2.83%/3.17%, 1024B 6.14%/3.02%, 65536B 4.84%/3.01%, 131072B 3.68%/2.06%, 262144B 2.67%/13.90%다. 262144B boundary revalidation은 Round 1 ratio 100.01%, C/C++ 변동 폭 6.76%/10.47%, Round 2 ratio 90.22%, 변동 폭 2.63%/14.55%였다. 반복 변동은 기록했지만 throughput 목표는 충족했고 `sol` 리뷰에서 WSS source 후보를 no-go로 판정했다. |
| `wss` | `PUBSUB` | 통과 (92.00%) | 통과 (100.79%) | 통과 (99.83%) | 통과 (98.08%) | 통과 (98.78%) | 통과 (97.22%) | C++ PUBSUB poller parity 후 full sweep size 중앙값은 98.43%로 통과했다. WSS encryption·WebSocket framing·Core I/O가 비용을 지배하므로 추가 source 변경은 하지 않는다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_170004_pubsub-wss-poller-parity-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_170312_pubsub-wss-poller-parity-cpp.txt` |
| `wss` | `DEALER_DEALER` | 통과 (89.02%) | 통과 (86.92%) | 통과 (87.13%) | 통과 (93.92%) | 통과 (95.70%) | 통과 (86.19%) | full sweep size 중앙값 88.07%로 transport 목표 95%에 미달한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_183856_dealer-dealer-wss-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_184136_dealer-dealer-wss-poller-parity-c1.txt`; 65536B profile C/C++: `/tmp/zlink-perf-tools-0070B1/profile-latest/dealer-dealer-wss-c-65536-poller-parity.data`, `/tmp/zlink-perf-tools-0070B1/profile-latest/dealer-dealer-wss-cpp-65536-poller-parity.data` |
| `wss` | `DEALER_ROUTER` | 통과 (90.83%, 재검증) | 통과 (97.28%) | 통과 (98.28%) | 통과 (99.43%) | 통과 (97.65%) | 통과 (87.18%) | full sweep size 중앙값 97.46%다. 64B full sweep ratio 84.52%는 C/C++ boundary 재검증 90.83%로 대체 판정했고, 재검증 변동 폭은 4.5%/5.7%다. full sweep 평균 latency 비율은 1.174배/0.983배/1.068배/1.008배/1.023배/1.165배다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_201807_dealer-router-wss-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_202046_dealer-router-wss-policy-c1.txt`; 64B C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_202432_dealer-router-wss-64-boundary-c1.txt`; 64B C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_202502_dealer-router-wss-64-boundary-c1.txt`; Sol review: `732b9b6d-0872-4b42-843c-649180d7d959` |
| `wss` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 통과 (93.79%) | 통과 (96.17%) | 통과 (99.00%) | 통과 (91.19%) | 통과 (92.72%) | 통과 (86.04%) | full sweep size 중앙값 93.25%로 transport 목표 95%에 미달한다. 64B/262144B boundary revalidation ratio는 Round 1 88.89%/82.36%, Round 2 92.25%/86.88%였다. TLS/262144B allocator A/B는 C++ throughput 3.33% 개선과 page fault 감소를 충족하지 못해 제거했다. |
| `tls` | `PUBSUB` | 통과 (89.83%) | 통과 (101.92%) | 통과 (97.85%) | 통과 (95.67%) | 통과 (92.12%) | 통과 (88.19%) | C++ PUBSUB poller parity 후 full sweep size 중앙값은 93.89%로 미달이다. TLS encryption·Core I/O가 비용을 지배하므로 추가 source 변경은 하지 않는다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_170628_pubsub-tls-poller-parity-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_170936_pubsub-tls-poller-parity-cpp.txt` |
| `tls` | `DEALER_DEALER` | 통과 (94.19%) | 통과 (95.85%) | 통과 (98.68%) | 통과 (93.59%) | 통과 (90.37%) | 통과 (85.33%) | full sweep size 중앙값 93.89%로 transport 목표 95%에 미달한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_185534_dealer-dealer-tls-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_185812_dealer-dealer-tls-poller-parity-c1.txt`; 65536B profile의 WSS 결과와 함께 binding 전용 hotspot 근거가 없어 source optimization은 no-go다. Sol review: `f2ff7e13-f320-4e47-948d-3519634e5ead` |
| `tls` | `DEALER_ROUTER` | 미달 (73.67%) | 통과 (93.26%) | 통과 (91.18%) | 통과 (91.09%) | 통과 (89.59%) | 미달 (81.33%) | policy-c1/c2 재검증 size 중앙값 90.34%로 transport 목표 95%에 미달한다. c2 평균 latency 비율은 1.708배/1.813배/0.607배/1.098배/1.120배/1.229배로 2.0배 이내지만, C++ throughput 변동 폭은 47.5%/31.7%/31.0%/24.3%/16.2%/20.5%로 모든 size에서 gate를 초과했다. c1 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_202834_dealer-router-tls-policy-c1.txt`; c1 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_203113_dealer-router-tls-policy-c1.txt`; c2 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_203537_dealer-router-tls-policy-c2.txt`; c2 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_203816_dealer-router-tls-policy-c2.txt`; Sol review: `8189e67a-4e7f-4e40-9557-2a71a032f174` |
| `tls` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미달 (89.97%) | 통과 (99.60%) | 미달 (93.02%) | 미달 (24.77%) | 미달 (67.02%) | 미달 (73.84%) | 최신 contract-clean size 중앙값은 81.91%다. 256B C/C++ throughput 변동 폭은 5.64%/8.60%로 gate를 통과했지만, 64B는 20.14%/12.25%, 1024B는 4.06%/13.23%, 65536B는 9.21%/28.28%로 변동성 gate를 넘었고 128KiB 이상은 최소 기준에 미달했다. 공식 판정은 미달이다. |
| `inproc` | `PUBSUB` | 통과 (93.17%) | 통과 (93.27%) | 통과 (98.13%) | 미달 (31.59%) | 미달 (61.80%) | 미달 (70.10%) | C++ PUBSUB poller parity 후 full sweep size 중앙값은 81.64%다. 65536B profile에서 C++ `read_subscription_message` 2.10%, `zlink_subscribe_part` 2.94%, `malloc` 3.36%가 관측됐고 Sol review는 direct receive·allocator·builder 변경을 no-go로 판정했다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_163619_pubsub-inproc-poller-parity-reuse-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_163926_pubsub-inproc-poller-parity-reuse-cpp.txt` |
| `inproc` | `DEALER_DEALER` | 통과 (89.15%) | 통과 (102.63%) | 통과 (100.59%) | 미달 (23.90%) | 미달 (64.94%) | 미달 (68.80%) | full sweep size 중앙값 78.97%로 transport 목표 95%에 미달한다. C++ throughput 변동 폭은 64B 23.6%, 256B 30.6%, 1024B 21.2%, 65536B 272.0%, 131072B 15.8%, 262144B 9.5%다. 65536B 변동성 재검증은 C 442,449.0, C++ 93,053.6 msg/s, ratio 21.03%이며 C++ 변동 폭 317.8%로 안정성 gate를 충족하지 못했다. WSL에 `perf`가 없어 profile은 수집하지 못했고, Sol review는 binding-only source optimization을 no-go로 판정했다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_190701_dealer-dealer-inproc-poller-parity-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_190940_dealer-dealer-inproc-poller-parity-c1.txt`; C 65536B: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_191656_dealer-dealer-inproc-65536-variability-c1.txt`; C++ 65536B: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_191733_dealer-dealer-inproc-65536-variability-c1.txt`; Sol review: `19af740f-3666-406a-a39f-9cc6a1a494fd` |
| `inproc` | `DEALER_ROUTER` | 통과 (85.68%, 재검증) | 통과 (93.39%, 재검증) | 통과 (92.89%, 재검증) | 미달 (26.72%, 재검증) | 미달 (63.11%) | 미달 (68.82%) | full sweep size 중앙값 75.49%로 transport 목표 95%에 미달한다. boundary 재검증 평균 latency 비율은 1.128배/1.059배/1.037배/2.375배이고 C++ throughput 변동 폭은 2.2%/14.0%/14.0%/130.3%다. 128KiB·256KiB full sweep ratio는 63.11%/68.82%로 안정적인 미달이다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_204334_dealer-router-inproc-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_204613_dealer-router-inproc-policy-c1.txt`; boundary C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_205007_dealer-router-inproc-boundary-c1.txt`; boundary C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_205154_dealer-router-inproc-boundary-c1.txt`; Sol review: `03a9e55e-bcbf-4657-a737-066101352d32` |
| `inproc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PAIR` | 통과 (86.62%) | 통과 (96.75%) | 통과 (94.41%) | 통과 (87.76%) | 통과 (91.42%) | 통과 (97.95%) | full sweep size 중앙값 92.91%로 transport 목표 95%에 미달한다. C/C++ throughput 변동 폭은 64B 20.66%/8.71%, 256B 12.46%/12.87%, 1024B 9.56%/9.53%, 65536B 8.28%/16.75%, 131072B 3.44%/6.60%, 262144B 2.53%/7.48%다. 65536B profile과 `sol` review에서 `message_t::from(span)` 및 raw builder는 일반 binding 최적화 근거가 되지 않아 no-go로 판정했다. |
| `ipc` | `PUBSUB` | 통과 (87.53%) | 통과 (98.12%) | 통과 (100.53%) | 통과 (90.90%) | 통과 (95.94%) | 통과 (95.92%) | C++ PUBSUB poller parity 후 full sweep size 중앙값은 95.93%로 통과한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_171901_pubsub-ipc-poller-parity-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_172410_pubsub-ipc-poller-parity-cpp.txt` |
| `ipc` | `DEALER_DEALER` | 통과 (93.81%) | 통과 (93.87%) | 통과 (92.19%) | 미달 (83.87%) | 통과 (87.83%) | 통과 (92.72%) | size 중앙값 92.45%로 transport 목표 95%에 미달한다. 1024B와 262144B ratio는 각각 92.19%, 92.72%이고 C/C++ 변동 폭은 약 8.9%/7.6%, 5.4%/7.8%로 안정적이다. 65536B ratio 83.87%도 C/C++ 변동 폭 약 5.5%/4.2%로 안정적이다. WSL `perf` 부재로 새 profile은 수집하지 못했고, 기존 IPC profile과 Sol review에서 encoder·decoder·queue·poller 공통 비용이 확인되어 binding-only source optimization은 no-go다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_192125_dealer-dealer-ipc-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_192404_dealer-dealer-ipc-poller-parity-c1.txt`; Sol review: `745976ff-1405-41af-9f73-c083204a026a` |
| `ipc` | `DEALER_ROUTER` | 미달 (81.99%, 재검증) | 통과 (93.76%, 재검증) | 통과 (96.51%, 재검증) | 통과 (85.35%, 재검증) | 통과 (85.86%, 재검증) | 통과 (89.69%) | full sweep size 중앙값 87.13%로 transport 목표 95%에 미달한다. boundary 재검증 평균 latency 비율은 1.226배/1.186배/1.038배/1.161배/1.159배이고 C/C++ throughput 변동 폭은 11.38%/10.64%, 12.90%/7.68%, 13.93%/10.14%, 9.47%/11.35%, 6.91%/5.32%다. 256KiB full sweep ratio 89.69%와 C++ 변동 폭 4.56%는 안정적이지만, 64B·64KiB 개별 기준과 size 중앙값은 미달한다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_210003_dealer-router-ipc-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_210242_dealer-router-ipc-policy-c1.txt`; boundary C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_210748_dealer-router-ipc-boundary-c1.txt`; boundary C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_211002_dealer-router-ipc-boundary-c1.txt`; Sol review: `27ce6203-fb3c-45d8-88e6-dc59a4cca88c` |
| `ipc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |

#### 9.1.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.2 .NET

- perf 경로: `bindings/dotnet/perf`
- Single 상태: `미측정`
- Multi 상태: `미측정`
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

#### 9.2.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |

#### 9.2.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.3 Java

- perf 경로: `bindings/java/perf`
- Single 상태: `미측정`
- Multi 상태: `미측정`
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

#### 9.3.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |

#### 9.3.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.4 Node

- perf 경로: `bindings/node/perf`
- Single 상태: `미측정`
- Multi 상태: `미측정`
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

#### 9.4.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |

#### 9.4.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.5 Go

- perf 경로: `bindings/go/perf`
- Single 상태: `미측정`
- Multi 상태: `미측정`
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

#### 9.5.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |

#### 9.5.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.6 Rust

- perf 경로: `bindings/rust/perf`
- Single 상태: `미측정`
- Multi 상태: `미측정`
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

#### 9.6.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |

#### 9.6.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.7 Python

- perf 경로: `bindings/python/perf`
- Single 상태: `미측정`
- Multi 상태: `미측정`
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

#### 9.7.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |

#### 9.7.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |


## 10. 전체 진행 상태

### 10.1 사전 조건

| 구분 | 상태 | 결과 파일 / 메모 |
|------|------|------------------|
| 버전 3곳 일치 | 확인 | `VERSION`, `core/CMakeLists.txt`, `core/include/zlink.h`가 모두 0.10.1이다. |
| 실제 runtime 버전 | 확인 | GitHub `core/v0.10.1` release prefix와 package provenance를 사용했다. |
| runner inventory | 선택 대상 확인 | `PAIR / inproc`, `PAIR / tcp`, `PAIR / ws`, `PAIR / wss`, `PAIR / tls`, `PAIR / ipc`, `PUBSUB / tcp`, `PUBSUB / ws`, `PUBSUB / wss`, `PUBSUB / tls`, `PUBSUB / inproc`, `DEALER_ROUTER / tcp`의 C·C++ runner 및 실제 binary mapping을 확인했다. 전체 inventory는 미완료다. |
| Multi size 정책 | 미확인 |  |
| 무시되는 runner option | 선택 대상 확인 | Effective Options에서 pattern, transport, size, duration, runs, I/O thread, HWM, timeout을 C·C++ 모두 확인했다. |
| memory guard | Single 해당 없음 | 이번 선택 범위는 Single이며 multi memory guard는 실행하지 않았다. |
| 재현 환경 manifest | 부분 기록 | release provenance, 측정 조건, report 경로와 session tag를 측정 기록과 결과 표에 남겼다. |

### 10.2 Pattern별 paired 기준 측정

| 구분 | 상태 | 결과 파일 / 메모 |
|------|------|------------------|
| 현재 언어 | C++ |  |
| 현재 pattern | 진행 중 | `DEALER_ROUTER_REQREP / tcp`는 boundary 재검증 반영 중앙값 94.87%로 no-go 처리했다. 현재 `DEALER_ROUTER_REQREP / ws` full sweep ratio는 92.00%, 88.68%, 89.73%, 88.55%, 98.67%, 93.61%, 중앙값 90.86%이며 Sol review 및 boundary 재검증을 진행한다. |
| paired C | 완료 | `status: complete`. 정책 정렬 `PUBSUB`, `DEALER_DEALER`의 paired 결과와 `DEALER_ROUTER / tcp`, `/ ws`, `/ wss`, `/ tls`, `/ inproc`, `/ ipc`를 C → C++ 순서로 측정했다. `DEALER_ROUTER / inproc` C full median은 2,247,755.0 / 1,605,539.6 / 1,598,773.6 / 439,708.2 / 173,491.8 / 79,539.8 msg/s이고, `/ ipc` full median은 2,185,331.8 / 1,205,057.4 / 750,265.2 / 38,045.2 / 25,042.0 / 14,905.8 msg/s다. `/ ipc` boundary C median은 2,089,866.2 / 1,194,847.8 / 751,513.0 / 37,585.4 / 25,169.8 msg/s다. 모든 report는 Core v0.10.1 release, auto-HWM, I/O thread 1, timeout 200ms 조건을 사용했다. |
| binding paired 결과 | 유효 결과·미달 포함 | `status: complete`. 정책 정렬 `PUBSUB`와 `DEALER_DEALER` 결과에 이어 `DEALER_ROUTER / tcp` ratio는 82.99%, 98.00%, 98.29%, 85.24%, 87.99%, 94.93%, `/ ws` ratio는 87.85%, 89.52%, 99.61%, 93.71%, 92.05%, 97.13%, `/ wss` full sweep ratio는 84.52%, 97.28%, 98.28%, 99.43%, 97.65%, 87.18%, `/ tls` c2 ratio는 73.67%, 93.26%, 91.18%, 91.09%, 89.59%, 81.33%, `/ inproc` full sweep ratio는 82.15%, 87.87%, 87.46%, 33.80%, 63.11%, 68.82%, `/ ipc` full sweep ratio는 76.98%, 92.83%, 98.78%, 82.71%, 84.56%, 89.69%다. size 중앙값은 각각 91.46%, 92.88%, 97.46%, 90.34%, 75.49%, 87.13%이며, `/ inproc` boundary ratio는 85.68%, 93.39%, 92.89%, 26.72%, `/ ipc` boundary ratio는 81.99%, 93.76%, 96.51%, 85.35%, 85.86%다. |
| 개선 결과 | 기록 | C 기준과 의미가 다른 harness 결과는 공식 판정에서 제외하고, 정책 정렬 후의 C → C++ 결과만 9.1.1과 11절에 기록했다. `DEALER_ROUTER / tcp`, `/ ws`, `/ tls`, `/ inproc`, `/ ipc`는 목표 미달이고 `/ wss`는 64B boundary 재검증 후 통과했으며, 다섯 transport 모두 binding-only source optimization은 no-go다. 과정 로그는 `log/`에 기록한다. |

| request/reply paired C | 완료 | `DEALER_ROUTER_REQREP / tcp` C report median: 212,245.6 / 192,437.8 / 176,009.8 / 17,174.8 / 11,981.0 / 7,339.6 msg/s. C++ 구형 실행은 공식 비교에서 제외했으며, parity 보정 후 C++을 다시 측정했다. |
| request/reply binding paired 결과 | 완료·미달 | C++ full sweep ratio: 95.10%, 93.51%, 96.72%, 94.99%, 95.76%, 93.89%, 중앙값 95.05%. 1024B·65536B boundary 재검증 ratio: 94.64%, 95.73%. 공식 재계산 중앙값 94.87%, latency ratio 모두 1.057배 이내. |
| request/reply 개선 결과 | no-go | 모든 개별 ratio는 85% 이상이지만 공식 중앙값이 95%에 미달한다. boundary 변동 폭은 C/C++ 7.38%/4.69%, 2.35%/5.09%로 안정됐고 binding-only hot-path 근거가 없어 source optimization을 수행하지 않는다. |

| request/reply ws 결과 | 진행 중 | `DEALER_ROUTER_REQREP / ws` C → C++ full sweep은 complete다. 개별 ratio는 92.00%, 88.68%, 89.73%, 88.55%, 98.67%, 93.61%, 중앙값 90.86%다. C++ 변동성이 64B·1024B·131072B에서 10%를 넘었고, Sol review 후 boundary만 재검증한다. |

| request/reply ws parity boundary | 완료 | C++ measured-loop parity 수정 후 64B ratio 96.25%, 65536B ratio 97.01%를 재현했다. latency ratio는 각각 1.004배/1.030배이고 C/C++ 변동 폭은 6.96%/7.28%, 3.27%/1.82%다. boundary는 안정됐으며 6-size full sweep을 다시 수행한다. |

### 10.3 언어 진행 상태

| 순서 | 언어 | Single 상태 | Multi 상태 | 다음 작업 |
|------|------|-------------|------------|-----------|
| 1 | C++ | 진행 중 | 미측정 | `PUBSUB`, `DEALER_DEALER`, `DEALER_ROUTER`의 선택 transport 측정을 완료했다. `DEALER_ROUTER_REQREP / tcp`는 boundary 재검증 반영 중앙값 94.87%로 미달했으며 binding-only source optimization은 no-go다. 다음 선택 대상은 `DEALER_ROUTER_REQREP / ws`다. |
| 2 | .NET | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 3 | Java | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 4 | Node | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 5 | Go | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 6 | Rust | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 7 | Python | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |

## 11. 측정 기록과 결과

이 표에는 현재 C++ Single에서 공식 판정에 사용한 최신 paired 측정만 남긴다. 각 결과는 같은 Core v0.10.1 release runtime과 동일한 조건에서 C를 먼저 실행한 뒤 C++을 실행했으며, 상세한 구현·후보·프로파일 과정은 같은 디렉터리의 `log/`에 기록한다. 전체 측정표와 과거 공식 결과는 9.1.1을 기준으로 한다.

| 날짜 | 언어 | 대상 | pair tag | size별 throughput ratio | size 중앙값 | 판정 | report |
|------|------|------|----------|-------------------------|------------|------|--------|
| 2026-08-09 | C++ | Single / PUBSUB / tcp | `pubsub-tcp-poller-parity` | 90.38%, 99.55%, 103.99%, 89.70%, 93.15%, 95.96% | 94.56% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_164718_pubsub-tcp-poller-parity-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_165025_pubsub-tcp-poller-parity-cpp.txt` |
| 2026-08-09 | C++ | Single / PUBSUB / ws | `pubsub-ws-poller-parity` | 84.17%, 93.47%, 102.77%, 94.58%, 94.85%, 96.92% | 94.71% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_165343_pubsub-ws-poller-parity-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_165651_pubsub-ws-poller-parity-cpp.txt` |
| 2026-08-09 | C++ | Single / PUBSUB / wss | `pubsub-wss-poller-parity` | 92.00%, 100.79%, 99.83%, 98.08%, 98.78%, 97.22% | 98.43% | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_170004_pubsub-wss-poller-parity-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_170312_pubsub-wss-poller-parity-cpp.txt` |
| 2026-08-09 | C++ | Single / PUBSUB / tls | `pubsub-tls-poller-parity` | 89.83%, 101.92%, 97.85%, 95.67%, 92.12%, 88.19% | 93.89% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_170628_pubsub-tls-poller-parity-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_170936_pubsub-tls-poller-parity-cpp.txt` |
| 2026-08-09 | C++ | Single / PUBSUB / inproc | `pubsub-inproc-poller-parity-reuse` | 93.17%, 93.27%, 98.13%, 31.59%, 61.80%, 70.10% | 81.64% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_163619_pubsub-inproc-poller-parity-reuse-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_163926_pubsub-inproc-poller-parity-reuse-cpp.txt` |
| 2026-08-09 | C++ | Single / PUBSUB / ipc | `pubsub-ipc-poller-parity` | 87.53%, 98.12%, 100.53%, 90.90%, 95.94%, 95.92% | 95.93% | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_171901_pubsub-ipc-poller-parity-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_172410_pubsub-ipc-poller-parity-cpp.txt` |
| 2026-08-09 | C++ | Single / DEALER_DEALER / tcp | `dealer-dealer-tcp-poller-parity-c1` | 90.88%, 93.58%, 93.96%, 84.05%, 85.44%, 90.42% | 90.65% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_175728_dealer-dealer-tcp-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_180006_dealer-dealer-tcp-poller-parity-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_DEALER / ws | `dealer-dealer-ws-poller-parity-c1` | 87.31%, 88.77%, 94.07%, 91.85%, 92.32%, 95.31% | 92.08% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_181249_dealer-dealer-ws-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_181529_dealer-dealer-ws-poller-parity-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_DEALER / wss | `dealer-dealer-wss-poller-parity-c1` | 89.02%, 86.92%, 87.13%, 93.92%, 95.70%, 86.19% | 88.07% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_183856_dealer-dealer-wss-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_184136_dealer-dealer-wss-poller-parity-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_DEALER / tls | `dealer-dealer-tls-poller-parity-c1` | 94.19%, 95.85%, 98.68%, 93.59%, 90.37%, 85.33% | 93.89% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_185534_dealer-dealer-tls-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_185812_dealer-dealer-tls-poller-parity-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_DEALER / inproc | `dealer-dealer-inproc-poller-parity-c1` | 89.15%, 102.63%, 100.59%, 23.90%, 64.94%, 68.80% | 78.97% | 미달·변동성 확인 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_190701_dealer-dealer-inproc-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_190940_dealer-dealer-inproc-poller-parity-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_DEALER / inproc / 65536B 재검증 | `dealer-dealer-inproc-65536-variability-c1` | 21.03% | 21.03% | 변동성 gate 초과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_191656_dealer-dealer-inproc-65536-variability-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_191733_dealer-dealer-inproc-65536-variability-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_DEALER / ipc | `dealer-dealer-ipc-poller-parity-c1` | 93.81%, 93.87%, 92.19%, 83.87%, 87.83%, 92.72% | 92.45% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_192125_dealer-dealer-ipc-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_192404_dealer-dealer-ipc-poller-parity-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / tcp | `dealer-router-tcp-policy-c3` | 82.99%, 98.00%, 98.29%, 85.24%, 87.99%, 94.93% | 91.46% | 미달·고변동 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_195450_dealer-router-tcp-policy-c3.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_195728_dealer-router-tcp-policy-c3.txt` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / ws | `dealer-router-ws-policy-c1` | 87.85%, 89.52%, 99.61%, 93.71%, 92.05%, 97.13% | 92.88% | 미달·변동성 확인 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_200556_dealer-router-ws-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_200838_dealer-router-ws-policy-c1.txt`; 평균 latency ratio: 1.067배/1.042배/1.009배/1.075배/1.081배/1.027배; C++ throughput 변동 폭: 9.1%/14.0%/12.7%/13.9%/16.7%/4.6%; Sol review: `4b2cf27a-87c1-4143-8aaf-117b3831f102` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / wss | `dealer-router-wss-policy-c1` | 84.52%, 97.28%, 98.28%, 99.43%, 97.65%, 87.18% | 97.46% | 통과·64B 재검증 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_201807_dealer-router-wss-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_202046_dealer-router-wss-policy-c1.txt`; 평균 latency ratio: 1.174배/0.983배/1.068배/1.008배/1.023배/1.165배 |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / wss / 64B 재검증 | `dealer-router-wss-64-boundary-c1` | 90.83% | 90.83% | 통과·안정 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_202432_dealer-router-wss-64-boundary-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_202502_dealer-router-wss-64-boundary-c1.txt`; C/C++ throughput 변동 폭: 4.5%/5.7%; Sol review: `732b9b6d-0872-4b42-843c-649180d7d959` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / tls | `dealer-router-tls-policy-c1` | 77.47%, 95.96%, 96.43%, 92.65%, 93.75%, 88.58% | 93.20% | 미달·고변동 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_202834_dealer-router-tls-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_203113_dealer-router-tls-policy-c1.txt`; 평균 latency ratio: 1.376배/2.301배/1.201배/1.077배/1.070배/1.132배 |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / tls 재검증 | `dealer-router-tls-policy-c2` | 73.67%, 93.26%, 91.18%, 91.09%, 89.59%, 81.33% | 90.34% | 미달·고변동 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_203537_dealer-router-tls-policy-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_203816_dealer-router-tls-policy-c2.txt`; 평균 latency ratio: 1.708배/1.813배/0.607배/1.098배/1.120배/1.229배; C++ throughput 변동 폭: 47.5%/31.7%/31.0%/24.3%/16.2%/20.5%; Sol review: `8189e67a-4e7f-4e40-9557-2a71a032f174` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / inproc | `dealer-router-inproc-policy-c1` | 82.15%, 87.87%, 87.46%, 33.80%, 63.11%, 68.82% | 75.49% | 미달·변동성 확인 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_204334_dealer-router-inproc-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_204613_dealer-router-inproc-policy-c1.txt`; 평균 latency ratio: 1.229배/1.107배/1.126배/2.286배/1.538배/1.571배; C++ throughput 변동 폭: 9.7%/31.1%/18.9%/168.9%/7.4%/5.3%; Sol review: `03a9e55e-bcbf-4657-a737-066101352d32` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / inproc boundary 재검증 | `dealer-router-inproc-boundary-c1` | 85.68%, 93.39%, 92.89%, 26.72% | - | 미달·고변동 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_205007_dealer-router-inproc-boundary-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_205154_dealer-router-inproc-boundary-c1.txt`; 평균 latency ratio: 1.128배/1.059배/1.037배/2.375배; C++ throughput 변동 폭: 2.2%/14.0%/14.0%/130.3% |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / ipc | `dealer-router-ipc-policy-c1` | 76.98%, 92.83%, 98.78%, 82.71%, 84.56%, 89.69% | 87.13% | 미달·변동성 확인 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_210003_dealer-router-ipc-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_210242_dealer-router-ipc-policy-c1.txt`; 평균 latency ratio: 1.583배/1.127배/1.007배/1.198배/1.178배/1.109배; C++ throughput 변동 폭: 14.55%/11.35%/11.75%/11.22%/10.91%/4.56%; Sol review: `27ce6203-fb3c-45d8-88e6-dc59a4cca88c` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / ipc boundary 재검증 | `dealer-router-ipc-boundary-c1` | 81.99%, 93.76%, 96.51%, 85.35%, 85.86% | 85.86% | 미달·고변동 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_210748_dealer-router-ipc-boundary-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_211002_dealer-router-ipc-boundary-c1.txt`; 평균 latency ratio: 1.226배/1.186배/1.038배/1.161배/1.159배; C/C++ throughput 변동 폭: 11.38%/10.64%/12.90%/7.68%/13.93%/10.14%/9.47%/11.35%/6.91%/5.32%; Sol review: `27ce6203-fb3c-45d8-88e6-dc59a4cca88c` |

| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / tcp full sweep | `dealer-router-reqrep-tcp-policy-c2` | 95.10%, 93.51%, 96.72%, 94.99%, 95.76%, 93.89% | 95.05% | boundary 재검증 전 임시 결과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_211628_dealer-router-reqrep-tcp-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_212518_dealer-router-reqrep-tcp-policy-c2.txt` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / tcp / 1024B boundary | `dealer-router-reqrep-tcp-boundary-1024-c1` | 94.64% | 94.64% | 미달·재계산 대상 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_213036_dealer-router-reqrep-tcp-boundary-1024-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_213108_dealer-router-reqrep-tcp-boundary-1024-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / tcp / 65536B boundary | `dealer-router-reqrep-tcp-boundary-65536-c1` | 95.73% | 95.73% | 통과·재계산 대상 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_213143_dealer-router-reqrep-tcp-boundary-65536-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_213219_dealer-router-reqrep-tcp-boundary-65536-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / tcp 최종 | `dealer-router-reqrep-tcp-policy-final` | 95.10%, 93.51%, 94.64%, 95.73%, 95.76%, 93.89% | 94.87% | 미달·no-go | latency ratio 1.022배/1.057배/0.964배/1.041배/1.036배/1.047배. boundary 재검증 후 공식 중앙값이 95%에 미달하고 binding-only hot-path 근거가 없어 개선하지 않는다. |

| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / ws full sweep | `dealer-router-reqrep-ws-policy-c1` | 92.00%, 88.68%, 89.73%, 88.55%, 98.67%, 93.61% | 90.86% | 미달·review 진행 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_214653_dealer-router-reqrep-ws-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_214934_dealer-router-reqrep-ws-policy-c1.txt` |

| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / ws / 64B parity boundary | dealer-router-reqrep-ws-parity-64-c2 | 96.25% | 96.25% | 통과·안정 | C: /home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_220013_dealer-router-reqrep-ws-parity-64-c2.txt; C++: /home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_220043_dealer-router-reqrep-ws-parity-64-c2.txt |
| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / ws / 65536B parity boundary | dealer-router-reqrep-ws-parity-65536-c2 | 97.01% | 97.01% | 통과·안정 | C: /home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_220115_dealer-router-reqrep-ws-parity-65536-c2.txt; C++: /home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_220146_dealer-router-reqrep-ws-parity-65536-c2.txt |

## 12. 완료 기준

다음 조건을 모두 만족해야 작업을 완료한다.

- runner, 정책, 상세 표의 pattern, transport, size inventory가 일치한다.
- 각 pattern의 최종 판정에 사용한 core 0.10.1 C와 binding paired report가 모두
  `status: complete`다.
- 모든 binding 상세 표에 `미측정`, `미달`, `보류`가 없다.
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
