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
- Single 상태: `진행 중` — `PAIR / inproc`, `PAIR / tcp`, `PAIR / ws`의 6개 size
  paired 측정을 현재 소스 기준으로 완료했지만, 전체 Single transport·pattern 완료
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
- Multi 상태: `미측정`
- 다음 작업: 현재 대상의 large-message 병목은 Release profiler와 단일 진단에서
  copy/allocation, Core send/receive, poller 비용을 확인했지만, public contract와
  ownership을 유지하면서 제거할 수 있는 C++ binding 전용 hot path는 확인하지 못했다.
  gprof로 확인한 raw send builder 호출 비용 후보는 64B 재측정에서 재현되지 않아
  제거했고, private constructor inline과 `operation_builder_base_t` 변경은 근거 부족과
  pooling·rollback 수명 위험 때문에 실행하지 않았다. `socket_access_t` 후보는
  profile에서 기존 out-of-line wrapper symbol이 사라지고 같은 시점 baseline보다
  64B ratio가 87.62%에서 94.88~95.05%로 올라갔지만, 변동성 gate 미통과 상태다.
  persistent poller 구현은 유지했지만 65536B 반복 median 개선은 121~124K에 그쳤고
  최신 inproc full sweep ratio는 24.77%다. `message.cpp`의 128KiB pool 하한을
  64KiB로 낮추는 후보도 `sol` 리뷰에서 검토했지만, allocation만 줄여 이론상 ratio가 약
  29%에 그치고 global mutex·release callback·linear search 비용을 추가하므로 실행하지
  않고 폐기했다. 추가 allocator, buffer reuse, receive object reuse 또는 정식 public
  contract가 아닌 direct send 경로는 만들지 않으며, 새 per-message 차이를 분리할 profiler
  근거가 생길 때까지 추가 source 후보를 보류한다. `PAIR / tcp`는 throughput 목표를
  충족했지만 64B 변동성만 반복되어 이동했고, `PAIR / ws`도 모든 개별 셀이 85% 이상,
  size 중앙값 97.10%로 목표를 충족했다. WS framing과 Core I/O가 남은 차이의 주된
  영역이며 binding 전용 hot path 근거가 없어 source 변경 없이 `wss / PAIR`를
  진행했다. WSS도 모든 개별 셀이 85% 이상이고 size 중앙값 98.23%로 목표를
  충족했지만, encryption·WebSocket framing·Core I/O가 전체 비용을 지배해 추가
  source 후보를 no-go로 판정했다. 다음 선택 대상은 `tls / PAIR`다.

#### 9.1.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미달 (94.82%) | 통과 (95.78%) | 통과 (98.91%) | 통과 (87.80%) | 통과 (91.09%) | 통과 (97.02%) | full sweep size 중앙값 95.30%다. 256B·1024B·262144B는 boundary revalidation에서 C/C++ 변동 폭 8.44%/4.38%, 6.04%/3.89%, 1.48%/4.90%로 안정됐고, 64B는 세 번째 재검증에서도 11.66%/11.32%로 gate를 넘었다. |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 통과 (96.22%) | 통과 (97.97%) | 통과 (99.80%) | 통과 (95.27%) | 통과 (94.38%) | 통과 (98.33%) | full sweep size 중앙값 97.10%다. C/C++ 변동 폭은 64B 11.04%/10.00%, 256B 6.41%/7.74%, 1024B 4.73%/5.06%, 65536B 3.78%/5.62%, 131072B 3.02%/5.01%, 262144B 3.77%/2.32%다. 64B C 변동 폭은 10%를 조금 넘었지만 C++ 전용 병목 근거가 없고 `sol` 리뷰에서 source 변경 후보를 no-go로 판정했다. |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 통과 (94.89%) | 통과 (97.93%) | 통과 (99.24%) | 통과 (98.76%) | 통과 (97.99%) | 통과 (98.46%) | full sweep size 중앙값 98.23%다. C/C++ 변동 폭은 64B 8.10%/2.35%, 256B 2.83%/3.17%, 1024B 6.14%/3.02%, 65536B 4.84%/3.01%, 131072B 3.68%/2.06%, 262144B 2.67%/13.90%다. 262144B boundary revalidation은 Round 1 ratio 100.01%, C/C++ 변동 폭 6.76%/10.47%, Round 2 ratio 90.22%, 변동 폭 2.63%/14.55%였다. 반복 변동은 기록했지만 throughput 목표는 충족했고 `sol` 리뷰에서 WSS source 후보를 no-go로 판정했다. |
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
| `inproc` | `PAIR` | 미달 (89.97%) | 통과 (99.60%) | 미달 (93.02%) | 미달 (24.77%) | 미달 (67.02%) | 미달 (73.84%) | 최신 contract-clean size 중앙값은 81.91%다. 256B C/C++ throughput 변동 폭은 5.64%/8.60%로 gate를 통과했지만, 64B는 20.14%/12.25%, 1024B는 4.06%/13.23%, 65536B는 9.21%/28.28%로 변동성 gate를 넘었고 128KiB 이상은 최소 기준에 미달했다. 두 report는 라운드 기록 참고 |
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
| runner inventory | 선택 대상 확인 | `PAIR / inproc`, `PAIR / tcp`, `PAIR / ws`, `PAIR / wss`의 C·C++ runner 및 실제 `cpp_perf_pair` binary mapping을 확인했다. 전체 inventory는 미완료다. |
| Multi size 정책 | 미확인 |  |
| 무시되는 runner option | 선택 대상 확인 | Effective Options에서 pattern, transport, size, duration, runs, I/O thread, HWM, timeout을 C·C++ 모두 확인했다. |
| memory guard | Single 해당 없음 | 이번 선택 범위는 Single이며 multi memory guard는 실행하지 않았다. |
| 재현 환경 manifest | 부분 기록 | release provenance, 명령, 조건, report 경로와 session tag를 라운드 기록에 남겼다. |

### 10.2 Pattern별 paired 기준 측정

| 구분 | 상태 | 결과 파일 / 메모 |
|------|------|------------------|
| 현재 언어 | C++ |  |
| 현재 pattern | 진행 중 | `PAIR / wss`를 최신 선택 대상으로 측정했고, `PAIR / inproc`, `PAIR / tcp`, `PAIR / ws` 결과는 라운드 기록에 보존했다. 전체 pattern·transport 완료는 아니다. 다음 선택 대상은 `tls / PAIR`다. |
| paired C | 완료 | `status: complete`, 최신 6개 size × 5회 paired report를 생성했다. C와 C++은 Core v0.10.1 release, auto-HWM, I/O thread 1, timeout 200ms 조건이 일치한다. `PAIR / wss` C median은 1,674,462.6 / 690,639.4 / 225,953.0 / 9,093.2 / 5,434.6 / 2,996.6 msg/s다. |
| binding paired 결과 | 유효 결과·통과 | `status: complete`. `PAIR / wss` C++ median은 1,588,878.8 / 676,332.0 / 224,226.8 / 8,980.2 / 5,325.4 / 2,950.6 msg/s다. ratio는 94.89%, 97.93%, 99.24%, 98.76%, 97.99%, 98.46%이고 size 중앙값은 98.23%다. C/C++ 변동 폭은 64B 8.10%/2.35%, 256B 2.83%/3.17%, 1024B 6.14%/3.02%, 65536B 4.84%/3.01%, 131072B 3.68%/2.06%, 262144B 2.67%/13.90%다. 262144B 두 차례 boundary revalidation의 C++ 변동 폭은 10.47%와 14.55%였지만 반복 조사와 `sol` no-go review를 기록했으며 source 변경은 없다. |
| 개선 반복 | 진행 중 | C++ active blocking send와 1ms retry, PAIR stop-token retry, C reference 수신 poller parity, raw single-part send state와 runtime binary mapping을 수정했다. C++ Single latency sampler를 C reference와 동일한 bounded reservoir 방식으로 맞췄다. `message_t` accessor inline 후보는 contract tree의 `<zlink.h>` 금지 규칙을 위반해 원래 out-of-line public contract로 복구했다. 이전 C++ `DONTWAIT` active send 결과는 C의 blocking 의미와 달라 공식 비교에서 제외했다. native poller, large-message native allocation, receiver object reuse, constructor storage 초기화 제거, private raw/slow 경로 분리 후보는 각각 측정 후 제거했다. gprof 근거로 raw builder의 `message`·`flags` fast branch를 header inline하는 후보도 첫 측정 92.35%가 반복 측정 87.31%로 재현되지 않아 제거했다. `socket_access_t::native_handle()` private header inline과 persistent poller 구현은 public API·ownership·ABI를 바꾸지 않아 유지했지만, `PAIR / inproc` 최신 결과는 성능 통과가 아니다. Release profile에서 C++ large-message sender/receiver 비용이 C와 공통 Core 경로에 집중되고, 정식 public contract가 아닌 direct send나 buffer/receiver reuse 없이는 제거할 binding 전용 hot path를 확인하지 못해 추가 source 후보를 보류했다. WS/WSS는 개별 셀과 size median 목표를 충족했지만 framing·Core I/O·encryption이 비용을 지배해 `sol` no-go review 후 source 변경 없이 기록한다. raw mode 분기 복원이 누락된 ASAN multi smoke 문제는 복구했고, Release 재빌드 후 C++ ctest가 통과했다. |
| 커밋과 푸시 | 진행 중 | `PAIR / wss` 결과 문서와 라운드 기록을 checkpoint commit/push한 뒤 commit id를 기록한다. |

### 10.3 언어 진행 상태

| 순서 | 언어 | Single 상태 | Multi 상태 | 다음 작업 |
|------|------|-------------|------------|-----------|
| 1 | C++ | 진행 중 | 미측정 | `PAIR / wss` 결과를 기록하고 `tls / PAIR`를 C → C++ 순서로 선택한다. 전체 pattern·transport를 완료하기 전에는 다음 언어로 이동하지 않는다. |
| 2 | .NET | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 3 | Java | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 4 | Node | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 5 | Go | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 6 | Rust | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 7 | Python | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |

## 11. 라운드 기록

측정 또는 구현 변경을 수행할 때마다 아래 표에 한 행을 추가하고, 상세 설명이 길면
`doc/perf/perf/log/` 아래에 별도 라운드 문서를 작성해 연결한다.

| 날짜 | 언어 | suite / 범위 | pair tag | 변경 또는 측정 | 결과 | report / 로그 |
|------|------|---------------|----------|----------------|------|---------------|
| 2026-08-07 | 전체 | 계획 초기화 | - | core 0.10.1 기준으로 성능 측정표와 진행 상태를 초기화했다. | 계획 작성 | 이 문서 |
| 2026-08-09 | C++ | Single / PAIR / inproc / 64B | `pair-inproc-64-policy-doc-5x5` | C reference와 C++ runner의 active 송수신 의미를 `PERF_SINGLE_TEST_POLICY` 기준으로 맞추고, raw single-part send state와 `message_t` accessor 비용을 개선한 뒤 C → C++ 순서로 5회 paired 측정했다. Core는 GitHub `core/v0.10.1` release runtime을 사용했다. 정책 확정 전 full-matrix 탐색 report는 공식 판정에서 제외했다. | 두 report 모두 `status: complete`. C median 2,458,383 msg/s, C++ median 2,226,716 msg/s, ratio 90.58%. 평균 latency는 C 0.013 ms, C++ 0.019 ms(1.46배)다. C throughput 변동 폭 10.66%, C++ 18.84%로 7.2절 조사 기준을 넘으므로 전체 완료나 최종 통과로 판정하지 않는다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_095529_pair-inproc-64-policy-doc-5x5.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_095605_pair-inproc-64-policy-doc-5x5.txt`; Core provenance: `/home/hep7hep7/.cache/zlink/core/0.10.1/linux-x64/share/zlink/core-package-provenance.json` |
| 2026-08-09 | C++ | Single / PAIR / inproc / 64·256·1024·65536·131072·262144B | `pair-inproc-all-sizes-policy-doc-5x5` | 정책 parity 수정과 raw single-part send state 후보를 포함한 선택 transport의 size sweep를 C → C++ 순서로 수행했다. 전체 matrix는 실행하지 않았다. | 두 report 모두 `status: complete`. C++ throughput ratio는 순서대로 83.50%, 94.44%, 90.45%, 67.00%, 65.85%, 73.84%이며 size 중앙값은 78.67%다. 64B와 128KiB 이상 일부 셀은 C++ 단순 one-way 최소 기준 85%보다 낮아 transport 완료로 판정하지 않는다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_100212_pair-inproc-all-sizes-policy-doc-5x5.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_100453_pair-inproc-all-sizes-policy-doc-5x5.txt` |
| 2026-08-09 | C++ | 후보 검증 / PAIR / inproc / 65536B | `pair-inproc-65536-recv-close-5x5` | C reference와 수신 message 수명을 맞추기 위해 C++ perf에서 수신 전 `message_t::close()`를 호출하는 후보를 검토했다. | C median 415,791.4 msg/s, C++ median 291,685 msg/s(70.14%)로 채택하지 않고 후보를 제거했다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_101323_pair-inproc-65536-recv-close-5x5.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_101354_pair-inproc-65536-recv-close-5x5.txt` |
| 2026-08-09 | C++ | 후보 검증 / PAIR / inproc / 262144B | `pair-inproc-262144-native-alloc-candidate` | C++ `message_t` large-message pool을 사용하지 않는 native allocation 후보를 검토했다. | C median 79,914.4 msg/s, C++ median 65,443 msg/s(81.89%)로 최소 기준에 미달했고, 다른 pattern 회귀를 확인하지 않았으므로 후보를 제거했다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_101206_pair-inproc-262144-native-alloc-candidate.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_101228_pair-inproc-262144-native-alloc-candidate.txt` |
| 2026-08-09 | C++ | 기준 재측정 / PAIR / inproc / 65536B | `pair-inproc-65536-policy-retry-baseline` | C와 C++의 active blocking send, transient retry 대기, stop-token bounded retry를 같은 의미로 맞춘 뒤 후보 비교 전 기준을 C → C++ 순서로 재측정했다. | 두 report 모두 `status: complete`. C median 440,432.8 msg/s, C++ median 361,464.2 msg/s, ratio 82.07%다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_103515_pair-inproc-65536-policy-retry-baseline-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_103610_pair-inproc-65536-policy-retry-baseline-cpp.txt` |
| 2026-08-09 | C++ | 후보 검증 / PAIR / inproc / 65536B | `pair-inproc-65536-policy-retry-native-poller` | C++ `poller_t`의 socket-only `zlink_poll()` fast path를 끄고 C reference와 같은 `zlink_poller_wait()` primitive를 사용하는 후보를 검토했다. | C median 430,427.2 msg/s, C++ median 301,141.6 msg/s, ratio 70.00%로 악화되어 후보를 제거했다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_103703_pair-inproc-65536-policy-retry-poller-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_103735_pair-inproc-65536-policy-retry-native-poller-cpp.txt` |
| 2026-08-09 | C++ | 후보 검증 / PAIR / inproc / 262144B | `pair-inproc-262144-policy-retry-native-alloc` | C++ large-message pool을 우회하고 Core native allocation을 사용하는 후보를 retry parity 이후 다시 검토했다. | C median 78,391.2 msg/s, C++ median 64,906.6 msg/s, ratio 82.80%로 최소 기준에 미달해 후보를 제거했다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_103848_pair-inproc-262144-policy-retry-pool-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_103919_pair-inproc-262144-policy-retry-native-alloc-cpp.txt` |
| 2026-08-09 | C++ | 후보 검증 / PAIR / inproc / 65536B | `pair-inproc-65536-policy-retry-reuse` | C++ receiver에서 `message_t` 하나를 drain loop에 재사용하는 후보를 검토했다. C canonical helper의 호출별 message 초기화·정리 의미와 달라 공식 최적화로 채택하지 않았다. | C median 429,563.4 msg/s, C++ median 386,147.6 msg/s, ratio 89.89%였지만 object lifetime 의미가 달라졌고 throughput 변동도 컸으며 95% 목표에 미달했다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_104141_pair-inproc-65536-policy-retry-reuse-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_104211_pair-inproc-65536-policy-retry-reuse-cpp.txt` |
| 2026-08-09 | C++ | 후보 검증 / PAIR / inproc / 65536B | `pair-inproc-65536-policy-retry-msginit` | `message_t` 생성자의 opaque storage zero-initialization을 제거하는 binding-level 후보를 검토했다. | C median 430,063.0 msg/s, C++ median 309,337.4 msg/s, ratio 71.93%로 악화되어 후보를 제거했다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_104439_pair-inproc-65536-policy-retry-msginit-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_104513_pair-inproc-65536-policy-retry-msginit-cpp.txt` |
| 2026-08-09 | C++ | 이전 최종 재검증 / Single / PAIR / inproc / 64·256·1024·65536·131072·262144B | `pair-inproc-all-sizes-policy-retry-final` | retry parity와 기존에 유지한 raw single-part send state, `message_t` accessor inline, runtime mapping 수정만 적용한 상태에서 선택한 하나의 `PAIR/inproc` transport를 C → C++ 순서로 5회씩 측정했다. 이후 C++ Single sampler가 C reference와 다른 것을 확인했으므로 이 report는 공식 비교에서 제외한다. | 두 report 모두 `status: complete`. size별 ratio는 86.96%, 98.01%, 93.75%, 77.95%, 67.51%, 77.13%이며 size 중앙값은 82.45%다. sampler parity audit 후 공식 기준에서 제외했다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_104638_pair-inproc-all-sizes-policy-retry-final-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_104916_pair-inproc-all-sizes-policy-retry-final-cpp.txt`; Core provenance: `/home/hep7hep7/.cache/zlink/core/0.10.1/linux-x64/share/zlink/core-package-provenance.json` |
| 2026-08-09 | C++ | 후보 검증 / PAIR / inproc / 64B | `pair-inproc-64-policy-retry-slowpath` | `sol` 에이전트 리뷰에서 제안한 private raw/slow 경로 분리를 적용해 느린 multipart 경로를 별도 helper로 분리하는 후보를 검토했다. 공개 API와 소유권 의미는 변경하지 않았고, C → C++ 순서로 단일 셀만 측정했다. | 두 report 모두 `status: complete`. C median 2,392,684.2 msg/s, C++ median 2,048,407.2 msg/s, ratio 85.61%다. 기존 최종 64B 기준 86.96%보다 낮고, 후보 채택 기준인 91.96% 이상 및 C++ throughput 변동 폭 10% 이하를 만족하지 못해 후보를 제거하고 source/header를 복구했다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_110852_pair-inproc-64-policy-retry-slowpath-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_110923_pair-inproc-64-policy-retry-slowpath-cpp.txt` |
| 2026-08-09 | C++ | sampler parity smoke / Single / PAIR / inproc / 64B | `pair-inproc-64-policy-retry-sampler-parity` | `sol` 에이전트가 발견한 C++ Single latency sampler 불일치를 수정했다. 기본 cap 1,000,000, `0` sample 미보관, 전체 count·sum 집계와 C와 같은 bounded reservoir 교체·percentile 보간을 적용한 뒤 C → C++ 순서로 5회 paired 측정했다. | 두 report 모두 `status: complete`. C median 2,619,479.0 msg/s, C++ median 2,106,005.0 msg/s, ratio 80.40%다. 성능 목표는 미달했지만 하네스 의미를 C 기준과 맞추는 필수 parity 수정이므로 sampler 변경은 유지한다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_111733_pair-inproc-64-policy-retry-sampler-parity-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_111809_pair-inproc-64-policy-retry-sampler-parity-cpp.txt` |
| 2026-08-09 | C++ | 최종 paired / Single / PAIR / inproc / 64·256·1024·65536·131072·262144B | `pair-inproc-all-sizes-policy-retry-sampler-parity-final` | C++ Single sampler parity 수정과 기존 retained parity·binding 변경을 적용한 선택 `PAIR/inproc` transport를 C → C++ 순서로 5회씩 측정했다. 전체 matrix는 실행하지 않았다. | 두 report 모두 `status: complete`. size별 ratio는 87.27%, 96.06%, 87.08%, 25.79%, 68.26%, 71.76%이며 size 중앙값은 79.42%다. 65536B 이상은 95% 목표에 미달하고, 64·1024B도 목표에 미달한다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_111937_pair-inproc-all-sizes-policy-retry-sampler-parity-final-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_112214_pair-inproc-all-sizes-policy-retry-sampler-parity-final-cpp.txt`; Core provenance: `/home/hep7hep7/.cache/zlink/core/0.10.1/linux-x64/share/zlink/core-package-provenance.json` |
| 2026-08-09 | C++ | 후보 검증 / PAIR / inproc / 64B | `pair-inproc-64-inline-fastpath` | gprof에서 확인한 raw send builder 호출 비용을 줄이기 위해 raw mode의 `message`·`flags`만 header inline하고 기존 multipart slow path를 유지하는 후보를 검토했다. 공개 API와 소유권 의미는 변경하지 않았으며 C → C++ 순서로 5회 paired 측정했다. | 두 report 모두 `status: complete`. 첫 측정은 C median 2,364,778.0 msg/s, C++ median 2,183,949.2 msg/s, ratio 92.35%였지만 C/C++ 변동성이 17.46%/20.44%로 gate를 넘었다. CPU 고정 진단은 공식 결과에서 제외했다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_113936_pair-inproc-64-inline-fastpath-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_114008_pair-inproc-64-inline-fastpath-cpp.txt`; pinned 진단 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_114217_pair-inproc-64-inline-fastpath-pinned-c.txt`; pinned 진단 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_114253_pair-inproc-64-inline-fastpath-pinned-cpp.txt` |
| 2026-08-09 | C++ | 후보 재검증 / PAIR / inproc / 64B | `pair-inproc-64-inline-fastpath-repeat` | 같은 Core release, Effective Options와 C → C++ 실행 순서를 유지한 상태에서 inline 후보를 5회 재측정했다. | 두 report 모두 `status: complete`. C median 2,506,509.6 msg/s, C++ median 2,187,938.6 msg/s, ratio 87.31%다. C/C++ 변동성은 14.15%/12.64%로 다시 10% gate를 넘었고 첫 측정의 개선이 재현되지 않아 inline 후보를 제거했다. 변동성이 안정될 때까지 추가 source 후보는 보류한다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_114610_pair-inproc-64-inline-fastpath-repeat-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_114642_pair-inproc-64-inline-fastpath-repeat-cpp.txt`; sol review submission: `019fe466-0d47-75b1-8119-5bfe018b8a68` |
| 2026-08-09 | C++ | 회귀 검증 / Multi / DEALER_DEALER / tcp / 64B | `ctest-cpp-multi-dealer-dealer-runtime-smoke` | inline 후보 제거 중 raw mode 분기 복원이 누락되어 임시 ASAN build에서 client `send_operation_t::message()`의 null state write를 확인하고 raw lvalue/rvalue/flags 분기를 복구했다. 공식 Release build를 다시 생성한 뒤 smoke test를 실행했다. | ASAN 원인은 수정되었고 공식 C++ ctest가 1/1 passed, 100% passed로 복구됐다. ASAN binary와 C++ Core release runtime은 공식 결과로 사용하지 않았다. | ASAN stack: `/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Messaging/send_operations.cpp:84`; 공식 ctest: `/home/hep7hep7/project/zlink/bindings/cpp/build/Testing/Temporary/LastTest.log` |
| 2026-08-09 | C++ | 후보 채택 재검증 / Single / PAIR / inproc / 64·256·1024·65536·131072·262144B | `pair-inproc-all-sizes-socket-access-inline-final` | Release profile에서 기존 `socket_access_t::native_handle()` out-of-line wrapper가 2.71% sampled self로 나타난 근거를 확인한 뒤, private header에서 native handle 접근을 inline하는 후보를 적용했다. `sol` 에이전트가 private implementation 최적화이며 public API, object layout, ownership, ABI와 ODR 위험이 없다고 검토했다. 후보 반영 후 C → C++ 순서로 같은 Core v0.10.1 release, auto-HWM, I/O thread 1, timeout 200ms 조건에서 6개 size를 5회씩 다시 측정했다. perf는 한 번에 하나만 실행했다. | 두 report 모두 `status: complete`. C median은 2,472,519.2 / 1,825,118.8 / 1,763,376.2 / 405,734.4 / 167,956.8 / 78,150.0 msg/s이고 C++ median은 2,200,741.0 / 1,826,795.2 / 1,636,738.6 / 109,446.2 / 117,017.6 / 57,890.6 msg/s다. ratio는 89.01%, 100.09%, 92.82%, 26.97%, 69.67%, 74.08%, size 중앙값은 81.54%다. 후보 구현은 유지하지만 95% 목표와 변동성 gate를 모두 충족한 transport는 아니며, C++ 65536B 변동성은 176.28%로 outlier 조사가 필요하다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_121935_pair-inproc-all-sizes-socket-access-inline-final.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_122216_pair-inproc-all-sizes-socket-access-inline-final.txt`; baseline C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_121259_pair-inproc-64-socket-access-baseline-cpp.txt`; sol review submission: `019fe484-37cd-7102-b92a-d4583931bad0`; Core provenance: `/home/hep7hep7/.cache/zlink/core/0.10.1/linux-x64/share/zlink/core-package-provenance.json` |

| 2026-08-09 | C++ | 후보 제거 / PAIR / inproc / 65536B | `pair-inproc-65536-message-construction-candidate` | `message_from_payload()`를 `message_t(size)`와 `memcpy`로 직접 구성하는 후보를 검토했다. C와 C++의 payload allocation/copy 의미는 유지했지만, 현재 C++ `message_t::from()` 경로를 대체할 강한 근거가 있는지 확인하는 단일 셀 A/B였다. | 두 report 모두 `status: complete`. C median 432,191.8 msg/s, C++ median 115,112.4 msg/s, ratio 26.64%였고 C++ 변동성은 203.5%였다. 개선을 재현하지 못해 후보를 제거했다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_123437_pair-inproc-65536-message-construction-candidate.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_123508_pair-inproc-65536-message-construction-candidate.txt`; sol review submission: `019fe497-40bd-7d62-9ad0-d6cb797572f6` |
| 2026-08-09 | C++ | 후보 재검증 / PAIR / inproc / 65536B | `pair-inproc-65536-poller-revalidate-1-2` | C++ socket-only wait가 임시 `zlink_poll()`을 매번 만들지 않고 등록된 `zlink_poller_wait()`를 사용하도록 한 persistent poller 구현을 C → C++ 순서로 두 번 5회 재검증했다. perf는 각 paired 순서에서 한 번에 하나만 실행했다. | 두 paired round 모두 `status: complete`. Round 1은 C median 424,327.2, C++ median 124,387.8 msg/s, ratio 29.31%; Round 2는 C median 429,636.0, C++ median 121,082.0 msg/s, ratio 28.18%였다. 기존 C++ 약 96~115K 대비 median 개선 신호는 반복됐지만 C++ 변동성은 Round 1 약 199%, Round 2 28.94%로 gate를 넘었고 95% 목표도 미달했다. 구현은 public API·ownership을 바꾸지 않아 유지하되 성능 통과로 기록하지 않는다. | Round 1 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_123939_pair-inproc-65536-poller-revalidate-1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_124018_pair-inproc-65536-poller-revalidate-1.txt`; Round 2 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_124055_pair-inproc-65536-poller-revalidate-2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_124128_pair-inproc-65536-poller-revalidate-2.txt`; sol review: `019fe4ab-5d82-7fb2-8848-6de8c8860ddd` |
| 2026-08-09 | C++ | 회귀 검증 / PAIR / inproc / 64B | `pair-inproc-64-poller-regression` | persistent poller 구현을 유지한 상태에서 대상이 아닌 64B 경계를 C → C++ 순서로 5회 재측정했다. | 두 report 모두 `status: complete`. C median 2,504,259.8, C++ median 2,257,869.8 msg/s, ratio 90.15%다. 이전 socket-access 후보의 64B ratio 89.01%보다 낮아지지 않았지만 C++ 변동성 27.89%로 최종 회귀 gate는 보류한다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_124209_pair-inproc-64-poller-regression.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_124240_pair-inproc-64-poller-regression.txt` |
| 2026-08-09 | C++ | 최종 paired / Single / PAIR / inproc / 64·256·1024·65536·131072·262144B | `pair-inproc-all-sizes-poller-alignment-final` | persistent poller 구현과 retained binding 변경을 적용한 선택 `PAIR / inproc` transport를 C → C++ 순서로 6개 size, 5회씩 측정했다. Core는 GitHub `core/v0.10.1` release runtime이고 auto-HWM, I/O thread 1, timeout 200ms를 맞췄다. 전체 matrix는 실행하지 않았으며 perf process는 직렬 실행했다. | 두 report 모두 `status: complete`. C median은 2,295,412.8 / 1,848,899.6 / 1,738,949.4 / 420,942.0 / 164,456.4 / 76,604.0 msg/s이고 C++ median은 2,122,254.8 / 1,776,893.2 / 1,573,472.8 / 98,009.2 / 112,671.0 / 56,162.6 msg/s다. ratio는 92.46%, 96.11%, 90.48%, 23.28%, 68.51%, 73.32%, size 중앙값은 81.90%다. C++ 변동성은 256B 16.56%, 1024B 14.92%, 65536B 24.59%로 gate를 넘었다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_124353_pair-inproc-all-sizes-poller-alignment-final.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_124918_pair-inproc-all-sizes-poller-alignment-final.txt`; Core provenance: `/home/hep7hep7/.cache/zlink/core/0.10.1/linux-x64/share/zlink/core-package-provenance.json` |
| 2026-08-09 | C++ | 진단 / PAIR / inproc / 65536B | `pair-inproc-65536-release-profile-diagnostic` | 공식 비교와 분리해 C와 C++을 각각 단독으로 Release `cpu-clock` profile과 `perf stat`으로 확인했다. `perf stat`은 C → C++ 순서로 한 번에 하나씩 실행했으며 공식 ratio에는 사용하지 않았다. | `perf stat`에서 task-clock은 C 11,542.29ms, C++ 11,563.45ms, context-switches와 CPU migrations는 둘 다 0, page faults는 2,973,587 대 2,984,840으로 유사했다. 계측 중 처리량은 C 112,670.8, C++ 110,167.8 msg/s로 계측 오버헤드가 커 공식 판정에서 제외한다. Release profile은 sender의 `message_t::from()`/Core copy, Core send·receive, poller·close 비용이 중심이며 C++ 전용으로 제거할 비용을 분리하지 못했다. | C stat: `/tmp/zlink-perf-tools-0070B1/profile-latest/c-pair-65536.stat`; C++ stat: `/tmp/zlink-perf-tools-0070B1/profile-latest/cpp-pair-65536.stat`; C profile: `/tmp/zlink-perf-tools-0070B1/profile-latest/c-pair-65536.data`; C++ profile: `/tmp/zlink-perf-tools-0070B1/profile-latest/cpp-pair-65536.data`; sol review: `019fe4ab-5d82-7fb2-8848-6de8c8860ddd` |
| 2026-08-09 | C++ | 계약 회귀 확인 / poller | `cpp-contract-poller-release-0.10.1` | 기존 poller add/modify/remove/close 계약 테스트를 Core v0.10.1 release prefix로 별도 Release test build에서 실행했다. 첫 configure는 release prefix에 Boost headers가 없어 중단됐지만, Core source의 기존 Boost header만 test build dependency로 지정하고 Core runtime은 release prefix로 유지해 재구성했다. | 첫 suite에서 `message.hpp`의 inline accessor가 contract tree의 `<zlink.h>` 금지 규칙을 위반하는 것을 확인했다. accessor를 원래 out-of-line public contract로 복구하고, runtime `socket_handle.hpp`에 필요한 `<zlink.h>`를 명시해 의존성 경계를 고쳤다. 이후 contract suite 11/11 passed, 기본 Release ctest의 C++ runtime smoke도 1/1 passed다. | contract ctest: `/tmp/zlink-cpp-contract-build-0.10.1/Testing/Temporary/LastTest.log`; 기본 ctest: `/home/hep7hep7/project/zlink/bindings/cpp/build/Testing/Temporary/LastTest.log` |
| 2026-08-09 | C++ | 후보 review / PAIR / inproc / 65536B | `pair-inproc-65536-pool-threshold-review` | C++ `message.cpp`의 large-message pool 하한을 128KiB에서 64KiB로 낮추는 추가 후보를 별도 구현 없이 `sol` 에이전트에 검토 요청했다. 공개 ownership·ABI·contract 보존 여부와 현재 65536B 병목을 설명할 수 있는지를 확인했다. | private allocator 변경 자체는 계약상 안전하지만 allocation 비용만 제거하며, 기존 profile과 pool 사용 중인 128KiB·256KiB 결과를 고려하면 현재 24.77% ratio gap을 해소할 근거가 없다. 64KiB 경로에 global mutex, release callback, linear exact-size search와 8MiB 혼합 allocation을 추가할 회귀 위험이 있어 후보를 실행하지 않고 폐기했다. 추가 allocator·buffer reuse 후보는 profiler가 binding 전용 비용을 분리할 때까지 보류한다. | sol review submission: `019fe4c9-332c-7591-9bda-f499bfdd0e22`; 관련 source: `/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Messaging/message.cpp` |
| 2026-08-09 | C++ | 최신 최종 paired / Single / PAIR / inproc / 64·256·1024·65536·131072·262144B | `pair-inproc-all-sizes-poller-contract-clean-final` | contract source-layout 검증에서 발견한 `message_t` accessor inline 후보를 제거하고 out-of-line public contract로 복구한 현재 소스에서, retained persistent poller와 `socket_access_t::native_handle()` inline을 적용한 선택 transport를 C → C++ 순서로 6개 size, 5회씩 재측정했다. Core는 GitHub `core/v0.10.1` release runtime이고 auto-HWM, I/O thread 1, timeout 200ms를 맞췄다. 전체 matrix는 실행하지 않았으며 perf process는 직렬 실행했다. | 두 report 모두 `status: complete`. C median은 2,368,128.0 / 1,779,729.0 / 1,705,951.4 / 409,183.2 / 166,334.0 / 75,524.6 msg/s이고 C++ median은 2,130,678.2 / 1,772,567.6 / 1,586,870.8 / 101,346.6 / 111,481.0 / 55,770.8 msg/s다. ratio는 89.97%, 99.60%, 93.02%, 24.77%, 67.02%, 73.84%, size 중앙값은 81.91%다. 256B만 ratio와 C/C++ throughput 변동 폭 5.64%/8.60%를 함께 통과했다. 64B·1024B·65536B의 C/C++ 변동 폭은 각각 20.14%/12.25%, 4.06%/13.23%, 9.21%/28.28%이고 128KiB 이상은 최소 기준에 미달했다. 이전 `pair-inproc-all-sizes-poller-alignment-final`은 inline accessor 후보가 포함된 소스의 역사 기록이며, 이 contract-clean report를 현재 공식 기준으로 사용한다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_131550_pair-inproc-all-sizes-poller-contract-clean-final.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_131831_pair-inproc-all-sizes-poller-contract-clean-final.txt`; Core provenance: `/home/hep7hep7/.cache/zlink/core/0.10.1/linux-x64/share/zlink/core-package-provenance.json`; contract ctest: `/tmp/zlink-cpp-contract-build-0.10.1/Testing/Temporary/LastTest.log` |
| 2026-08-09 | C++ | 최종 paired / Single / PAIR / tcp / 64·256·1024·65536·131072·262144B | `pair-tcp-all-sizes-contract-clean-final` | `PAIR / tcp`를 새 transport 대상으로 선택해 C → C++ 순서로 6개 size, 5회씩 측정했다. Core는 GitHub `core/v0.10.1` release runtime이고 auto-HWM, I/O thread 1, timeout 200ms를 맞췄다. 전체 matrix는 실행하지 않았으며 perf process는 직렬 실행했다. | 두 report 모두 `status: complete`. C median은 2,295,656.6 / 1,143,764.0 / 595,416.0 / 38,134.2 / 24,951.6 / 15,003.2 msg/s이고 C++ median은 2,176,794.2 / 1,095,440.4 / 588,905.4 / 33,480.8 / 22,728.6 / 14,555.6 msg/s다. full sweep ratio는 94.82%, 95.78%, 98.91%, 87.80%, 91.09%, 97.02%, size 중앙값은 95.30%다. 64B·256B·1024B·262144B는 변동성 조사 대상이었고, 64B는 boundary revalidation 세 차례에서도 gate를 넘지 않아 transport를 완료로 판정하지 않는다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_133558_pair-tcp-all-sizes-contract-clean-final.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_133838_pair-tcp-all-sizes-contract-clean-final.txt`; Core provenance: `/home/hep7hep7/.cache/zlink/core/0.10.1/linux-x64/share/zlink/core-package-provenance.json` |
| 2026-08-09 | C++ | 변동성 재검증 / Single / PAIR / tcp / 64·256·1024·262144B | `pair-tcp-boundary-revalidate-1-3` | full sweep에서 10% throughput variation을 넘은 네 셀만 같은 조건으로 C → C++ 순서로 재검증했다. 64B는 세 차례 paired revalidation을 수행했고, 나머지 세 셀은 한 차례 재검증했다. | Round 1의 C/C++ median과 ratio는 2,525,139.6/2,161,442.0 msg/s(85.60%), 1,149,337.8/1,079,913.8(93.96%), 601,622.0/587,371.8(97.63%), 14,901.6/14,481.2(97.18%)다. Round 1 variation은 C/C++ 각각 10.79%/7.78%, 8.44%/4.38%, 6.04%/3.89%, 1.48%/4.90%로 세 셀은 안정됐다. 64B Round 2는 C/C++ 2,523,376.8/2,261,225.6 msg/s(89.61%), variation 10.89%/4.87%; Round 3은 2,418,238.0/2,280,926.2 msg/s(94.32%), variation 11.66%/11.32%로 반복 gate를 넘지 못했다. source 변경은 없었고 `PAIR / tcp`의 다음 transport로 이동한다. | Round 1 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_134220_pair-tcp-boundary-revalidate.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_134409_pair-tcp-boundary-revalidate.txt`; Round 2 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_134615_pair-tcp-64-boundary-revalidate-2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_134646_pair-tcp-64-boundary-revalidate-2.txt`; Round 3 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_134732_pair-tcp-64-boundary-revalidate-3.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_134804_pair-tcp-64-boundary-revalidate-3.txt` |
| 2026-08-09 | C++ | 최종 paired / Single / PAIR / ws / 64·256·1024·65536·131072·262144B | `pair-ws-all-sizes-contract-clean-final` | `PAIR / ws`를 다음 단일 transport 대상으로 선택해 같은 Core v0.10.1 release, auto-HWM, I/O thread 1, timeout 200ms 조건에서 C → C++ 순서로 6개 size를 5회씩 측정했다. 전체 matrix는 실행하지 않았고 perf process는 직렬 실행했다. | 두 report 모두 `status: complete`. C median은 1,778,658.0 / 995,329.2 / 404,398.2 / 21,675.4 / 14,587.0 / 8,752.0 msg/s이고 C++ median은 1,711,428.0 / 975,155.0 / 403,586.6 / 20,650.0 / 13,767.2 / 8,606.0 msg/s다. ratio는 96.22%, 97.97%, 99.80%, 95.27%, 94.38%, 98.33%이고 size 중앙값은 97.10%다. C/C++ throughput 변동 폭은 각각 64B 11.04%/10.00%, 256B 6.41%/7.74%, 1024B 4.73%/5.06%, 65536B 3.78%/5.62%, 131072B 3.02%/5.01%, 262144B 3.77%/2.32%다. WS 64B의 C 변동 폭은 10%를 조금 넘었지만 source 병목 근거는 없고, paired median과 나머지 조건은 기준을 충족해 source 변경 없이 다음 transport로 이동한다. | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_135227_pair-ws-all-sizes-contract-clean-final.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_135640_pair-ws-all-sizes-contract-clean-final.txt`; Core provenance: `/home/hep7hep7/.cache/zlink/core/0.10.1/linux-x64/share/zlink/core-package-provenance.json` |
| 2026-08-09 | C++ | `sol` review / Single / PAIR / ws | `pair-ws-hotpath-review` | WS/PAIR 결과에서 source 변경 후보가 없어 `sol` 에이전트에 public contract·ownership·harness policy를 유지하는 안전한 개선 방향이 있는지 검토를 요청했다. | 모든 개별 셀이 85% 이상이고 size median이 97.10%라 성능 목표를 충족했다. WS framing과 Core I/O 및 정상적인 wrapper 비용 외에 binding 전용 hot path 근거가 없으며, direct send·external buffer·message reuse·perf 전용 fast path는 계약과 lifetime을 훼손할 위험이 있어 no-go로 판정했다. source 변경 없이 `wss / PAIR`로 이동한다. | sol review submission: `019fe4e4-d125-7891-a328-76f172fa80ff`; 관련 source: `/home/hep7hep7/project/zlink/bindings/cpp/perf/single/src/perf_pair.cpp`, `/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Messaging/send_operations.cpp`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/single/common/perf_single_common.cpp` |

| 2026-08-09 | C++ | 최종 paired / Single / PAIR / wss / 64·256·1024·65536·131072·262144B | `pair-wss-all-sizes-contract-clean-final` | `PAIR / wss`를 다음 단일 transport 대상으로 선택해 같은 Core v0.10.1 release, auto-HWM, I/O thread 1, timeout 200ms 조건에서 C → C++ 순서로 smoke와 6개 size full sweep을 수행했다. 전체 matrix는 실행하지 않았고 perf process는 직렬 실행했다. | smoke와 full sweep 두 report 모두 `status: complete`다. C median은 1,674,462.6 / 690,639.4 / 225,953.0 / 9,093.2 / 5,434.6 / 2,996.6 msg/s이고 C++ median은 1,588,878.8 / 676,332.0 / 224,226.8 / 8,980.2 / 5,325.4 / 2,950.6 msg/s다. full sweep ratio는 94.89%, 97.93%, 99.24%, 98.76%, 97.99%, 98.46%이고 size 중앙값은 98.23%다. C/C++ 변동 폭은 64B 8.10%/2.35%, 256B 2.83%/3.17%, 1024B 6.14%/3.02%, 65536B 4.84%/3.01%, 131072B 3.68%/2.06%, 262144B 2.67%/13.90%다. 262144B 변동성은 boundary revalidation으로 추가 조사했다. | C smoke: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_140447_pair-wss-smoke-contract-clean.txt`; C++ smoke: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_140455_pair-wss-smoke-contract-clean.txt`; C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_140503_pair-wss-all-sizes-contract-clean-final.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_140745_pair-wss-all-sizes-contract-clean-final.txt`; Core provenance: `/home/hep7hep7/.cache/zlink/core/0.10.1/linux-x64/share/zlink/core-package-provenance.json` |
| 2026-08-09 | C++ | 변동성 재검증 / Single / PAIR / wss / 262144B | `pair-wss-262144-boundary-revalidate-1-2` | full sweep에서 C++ 262144B throughput variation이 10%를 넘어서 같은 조건으로 C → C++ 순서의 5회 boundary revalidation을 두 차례 수행했다. | Round 1은 C/C++ median 2,960.4/2,960.8 msg/s, ratio 100.01%, variation 6.76%/10.47%다. Round 2는 C/C++ median 3,045.0/2,747.2 msg/s, ratio 90.22%, variation 2.63%/14.55%다. C는 두 round에서 안정됐고 C++은 변동이 반복됐지만 두 결과 모두 최소 throughput 기준 85% 이상이며, source 병목을 입증하는 패턴이 아니므로 환경 변동으로 기록하고 source 변경 없이 진행한다. | Round 1 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_141112_pair-wss-262144-boundary-revalidate.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_141144_pair-wss-262144-boundary-revalidate.txt`; Round 2 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_141225_pair-wss-262144-boundary-revalidate-2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_141258_pair-wss-262144-boundary-revalidate-2.txt` |
| 2026-08-09 | C++ | `sol` review / Single / PAIR / wss | `pair-wss-hotpath-review` | WSS/PAIR 결과에서 source 변경 후보가 없어 `sol` 에이전트에 public contract·ownership·harness policy를 유지하는 안전한 개선 방향이 있는지 검토를 요청했다. | encryption, WebSocket framing, Core I/O가 전체 비용을 지배하고 현재 raw single-part send state와 persistent poller 뒤 DONTWAIT drain 외에 제거할 binding 전용 hot path가 없다. direct send, pool threshold 변경, external buffer, message reuse, perf 전용 fast path는 public contract와 object lifetime을 회귀시킬 위험이 있으므로 no-go로 판정했다. | sol review submission: `019fe4ee-8a8d-7602-977f-abfce647b7a8`; 관련 source: `/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Messaging/operation_submit.hpp`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/single/src/perf_pair.cpp`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/single/common/perf_single_common.cpp` |

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
