# core 0.10 bindings 라이브러리 성능 개선 계획

> 시작일: 2026-08-07
>
> 작업 브랜치: `core-0.10.0-bindings-performance`
>
> 이 브랜치에서 작업하도록 승인되었으며, 측정과 문서 변경은 WSL Ubuntu-24.04 작업영역에서 진행한다.
>
> 이 문서는 core 0.10.1을 기준으로 bindings 라이브러리 성능 개선을 처음부터
> 진행하기 위한 실행 문서다. 이전 계획 문서의 측정값과 완료 판정은 가져오지 않는다.
> 새 C 기준 결과와 각 binding의 새 결과만 이 문서에 기록한다. 이 문서에는 측정 대상,
> 측정 조건, report 경로, 비교값과 판정만 남긴다. 실행 명령, 후보 검토, 프로파일과
> 같은 과정 설명은 이 문서가 있는 폴더의 `log/`에 기록한다.

## 1. 기준 버전과 시작 상태

이번 작업의 core 기준 버전은 0.10.1이다. 측정 전에 다음 세 파일의 버전이 모두 같은지
확인한다.

public/runtime 경계, ownership, callback 수명, allocator, pool, queue 또는 thread model을
바꾸는 구조 변경은 구현·측정 전에 Sol 에이전트에 read-only review를 요청한다. 리뷰 결과와
before/after 측정으로 이득이 분리되지 않거나 cleanup·thread 이동·예외 경로 위험이 남으면
후보를 제거하고, C harness parity 수정과 binding source 개선은 별도 결과로 기록한다.

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
**목표**를 둔다. transport의 throughput 판정은 모든 측정 size ratio의 **산술평균
(aggregate mean)**을 gate로 사용한다. 개별 size가 최소 기준보다 낮아도 종합 평균이 목표를
충족하면 그 값만으로 전체를 미달로 바꾸지 않는다. 개별 값은 병목 위치와 결과를 확인하기
위한 측정 기록으로 남긴다. 중앙값과 반복값은 보조 비교 자료로 기록한다. 비율 자체에는
상한을 두지 않는다.

C++의 단순 one-way 중앙값 목표는 기본 95%다. 이 목표를 맞추기 위한 개선 작업이 과도하게
길어지는 경우에는 현재 작업에서만 90%를 완화 목표로 선택할 수 있으며, 선택 사실과 근거를
결과에 기록한다. 완화 목표를 선택해도 size ratio 산술평균 90%를 달성해야 한다.

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
| multi routed echo | `MULTI_DEALER_ROUTER_SENDSEND`, `MULTI_ROUTER_ROUTER_SENDSEND` |

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

throughput과 같은 방식으로 size별 평균 latency ratio의 중앙값을 aggregate 값으로 계산한다.
이 aggregate latency가 아래 상한을 넘으면 `미달`로 판정한다. 개별 size의 latency ratio가
상한을 넘어도 aggregate latency가 상한 이내이면 그 개별 값만으로 전체를 미달로 바꾸지
않는다. 해당 값은 latency outlier로 기록한다. p95와 p99는 진단 자료로만 기록하고 목표
통과 여부에는 사용하지 않는다.
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
- contract의 public interface(공개 함수·메서드 signature, 공개 type·enum 값,
  ownership·error 동작)는 변경하지 않는다. 기존 public interface의 변경도 허용하지
  않으며, 성능 개선은 현재 interface를 호출하는 binding 내부 구현과 perf harness의
  의미 정렬 범위에서만 수행한다.
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
121~124K로 개선되는 신호가 있었지만, 최신 5회 결과의 측정값과 95% 목표를
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
receive object reuse는 측정 의미나 소유권을 바꾸므로 채택하지 않는다.

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

### 7.2 반복 횟수와 측정값 기록

| 단계 | 기본 조건 | 용도 |
|------|-----------|------|
| smoke | 1초, 1회 | 실행 경로와 종료 상태 확인 |
| 탐색 | 기본 duration, 1회 | 병목 후보 선별 |
| 후보 판정 | 기본 duration, 3회 | before/after와 C 대비 비율을 더 자세히 비교할 때 사용 |
| 최종·경계 판정 | 기본 duration, 5회, CPU pin 없음 | 필요할 때 반복값을 추가 기록하는 진단용 측정 |

반복 횟수는 perf 정책의 실행 조건을 따른다. `runs=1`이면 해당 측정값을 사용하고,
`runs>1`이면 metric별 median을 대표값으로 사용한다. 원시 반복값과 변동 폭은 측정 기록에
함께 남길 수 있지만 판정 입력은 throughput ratio와 평균 latency ratio다. 노트북 부하와
측정 오차가 있더라도 측정값이 생성된 셀은 즉시 기준과 비교하고 다음 셀로 진행한다. 추가
반복은 before/after 확인이나 원인 진단이 필요할 때만 수행하며, 기존 결과를 무효화하거나
판정을 미루는 조건으로 사용하지 않는다. 유리한 실행 결과만 선택하지 않으며,
CPU pin·timeout·sleep 증가로 수치를 조정하지 않는다.

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
- 목표 기준 ±5%p 셀은 필요하면 추가 반복값을 참고로 기록하지만, 단일 측정값을 무효화하지
  않는다.
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
8. C 대비 throughput과 평균 latency를 비교하고 현재 transport의 목표 미달 셀을 확인한다.
9. 미달 셀은 profiler, allocation 자료, copy 수, callback/dispatch 및 native 경계
   자료로 비용 위치를 확인한다.
10. aggregate 평균이 미달한 대상은 먼저 자체 hot-path 개선 pass를 수행한다. 후보는
    public interface, ownership, error contract와 측정 의미를 유지해야 하며, 후보 after를
    한 번 측정해 자체 pass의 효과를 기록한다.
11. 자체 pass 뒤에도 대상의 최종 판단을 닫지 않는다. Sol에 read-only review를 요청하고,
    리뷰에서 선택한 계약 보존 후보로 두 번째 개선 pass를 수행해 after를 한 번 측정한다.
    Sol이 안전한 후보를 제시하지 않으면 그 no-go 판단을 두 번째 pass의 결과로 기록한다.
12. 두 개선 pass의 before/after와 aggregate 결과를 비교한다. 추가 반복은 원인 진단이나
    before/after 확인에 꼭 필요할 때만 수행하며, 변동값을 이유로 반복하지 않는다.
13. 기능 테스트와 같은 pattern 안의 대상이 아닌 대표 셀에 대한 회귀 gate를 통과시킨다.
14. 현재 transport의 모든 message size report가 complete이고 throughput·latency aggregate
    평균이 목표를 만족하면 transport 완료를 기록한다. 개별 size 미달은 결과에 기록한다.
    성능 개선 코드를 채택했다면 검증된 변경과 측정 근거만 커밋하고 원격에 푸시한 뒤 다음
    transport로 이동한다.
15. aggregate 평균이 미달한 대상은 자체 pass와 Sol pass가 모두 끝난 뒤에도 공개 contract를
    유지한 효과 있는 후보가 없을 때만 `보류`로 기록하고 다음 transport로 이동한다.
16. 선택한 pattern의 모든 공식 transport report가 complete이고 각 transport의
    throughput·latency aggregate 평균이 통과 또는 보류로 확정되면 pattern 완료를 기록하고
    관련 문서를 커밋해 원격에 푸시한다.
17. pattern 커밋과 푸시가 끝난 뒤에만 같은 언어의 다음 pattern을 선택한다.
18. 현재 언어의 Single과 Multi 모든 pattern이 완료된 뒤 pattern별 최종 report와 표를
    다시 대조한다. 미측정 또는 유효한 report가 없는 셀이 남아 있으면 다음 언어로
    이동하지 않는다. aggregate 평균 미달이지만 hot path 검토와 후보 A/B, 필요한 Sol
    리뷰를 끝낸 대상은 `보류`로 기록하고 다음 선택 대상에 진행할 수 있다.
19. 현재 언어가 모두 완료된 뒤에만 다음 언어로 이동한다.

한 번에 하나의 언어만 측정한다. C와 binding을 paired 제한 측정할 때도 공식 perf
프로세스는 순차 실행해 서로 CPU와 memory에 영향을 주지 않게 한다.
모든 최종 측정은 `--pin-cpu`를 사용하지 않는다. 한 번에 perf process 하나만 실행한다.

### 7.5 Pattern 완료와 언어 전환 gate

pattern 완료는 수치를 한 번 얻었다는 뜻이 아니다. 다음 조건을 모두 만족해야 완료로
기록한다.

- 해당 pattern의 모든 공식 transport와 message size에서 C와 binding report가
  `status: complete`다.
- 모든 size의 paired report가 있고, throughput ratio 산술평균과 평균 latency ratio의
  산술평균, client 수, auto-HWM 기준을 측정값으로 판정한다. 개별 size의 최소 기준·latency
  상한 미달은 결과에 기록하되 aggregate gate를 별도로 낮추지 않는다.
- 개선 전후 기능 테스트와 같은 pattern의 대표 회귀 셀이 통과한다.
- 최종 판정에 사용한 C와 binding이 가까운 시점의 같은 manifest와 session tag로 측정됐다.
- 상세 표에 C report, binding report, 반복값, 비율과 판정 근거를 기록했다.
- POSD 위험 신호를 변경 전후로 다시 확인했고 새 복잡성을 만들지 않았다.

목표에 미달하면 자체 개선 pass와 Sol 리뷰 기반 개선 pass를 각각 한 번씩 수행한다. 각
pass는 before/after 또는 후보 no-go 결과를 남긴다. 두 pass가 끝난 뒤에도 공개 contract를
유지한 효과 있는 후보가 없으면 `보류`로 확정한다. 변동값과 안정성을 이유로 같은 셀을
반복하지 않는다. public contract 변경이 필요하면 우회 구현으로 통과시키지 않고 `보류`로
기록한다.

### 7.6 개선 코드 커밋과 푸시

성능 개선 후보가 pattern 목표와 회귀 gate를 통과해 최종 코드로 채택되면 다음 pattern을
시작하기 전에 커밋하고 원격 저장소에 푸시한다. 커밋에는 현재 개선과 직접 관련된 binding,
테스트, runner, 계획 문서와 측정 로그만 포함한다. 작업 트리의 다른 변경을 함께 넣지 않는다.

커밋 전에는 변경 파일 목록과 staged diff를 확인하고 `git diff --cached --check`를 통과시킨다.
커밋 메시지에는 언어와 pattern, 제거한 병목을 드러낸다. 푸시한 commit id와 paired report
경로는 측정 기록과 결과 표에 남긴다. 후보를 적용·기각한 과정은 이 문서와 같은 폴더의
`log/`에 남긴다. 다음 상태는 커밋 대상으로 인정하지 않는다.

- C 또는 binding report가 partial인 후보
- 목표나 latency, 회귀 gate를 통과하지 못한 후보
- perf 전용 우회나 public contract 위반이 남은 후보
- 기능 테스트를 통과하지 못한 후보

C++ binding 경로에서는 large-message buffer pool을 사용하지 않는 것으로 확정한다. C++의
pool 재도입이나 pool A/B는 후보로 취급하지 않는다. 반면 .NET과 같이 VM 또는 managed
runtime 위에서 동작하는 binding에 이미 존재하는 `Message`·byte storage pool은 기존 내부
구현으로 유지할 수 있다. managed binding에서 pool을 새로 도입하거나 정책을 바꿀 때도
public ownership과 재사용 경계를 바꾸지 않고, 별도 후보로 before/after 측정을 남긴다.
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
- `통과(비율%)`: 모든 size의 paired report가 complete이고, throughput ratio 산술평균과
  평균 latency ratio의 산술평균, 회귀, Effective Options, auto-HWM, client 수 조건을 만족한다.
  개별 size의 최소 기준·latency 상한 미달은 outlier로 함께 기록할 수 있다.
- `미달(비율%)`: 최종 판정 전의 임시 상태다. 유효한 paired 결과가 있지만 throughput 또는
  latency의 aggregate 평균 목표에 도달하지 않았고 hot path 검토 또는 후보 비교가 남아 있다.
- `보류`: paired 측정, 자체 개선 pass, Sol 리뷰 기반 두 번째 개선 pass를 완료했지만
  public contract를 유지한 추가 개선 요소가 없어 현재 aggregate 목표를 달성하지 못한 채
  다음 대상으로 이동한다. 변동 폭이나 안정성을 이유로 `보류`하지 않는다.
- 상세 표의 최종 상태는 `통과(비율%)`, `보류(비율%)`, `미측정`을 사용한다. C와 binding의
  paired report가 있고 ratio와 latency가 기록된 pattern·transport이면 size ratio와 평균
  latency ratio의 aggregate 평균으로 상태를 판정한다. paired report가 없는 셀만
  `미측정`으로 남긴다.
- 측정 여부는 두 report 경로가 있고 두 report가 모두 `status: complete`이며 ratio가
  기록되어 있는지로 확인한다. 이 조건이면 `측정 완료`이고, 셀 값이 `미측정`이면
  아직 측정하지 않은 것이다. `측정값으로 판정` 같은 문구는 상태 값으로 사용하지 않는다.
- `해당 없음`: 공식 C runner와 binding 정책 모두 측정하지 않는 조합이다.

반복값의 변동이나 하향 drift는 필요할 때 참고 기록으로만 남긴다. 이것만으로 `미측정`
또는 별도 판정 상태를 만들지 않는다. 측정값이 있으면 throughput ratio와 latency ratio의
aggregate 평균으로 판정하고, aggregate 목표가 미달이면 hot path 검토와 후보 A/B, 필요한
Sol 리뷰 후 즉시 `보류` 여부를 결정한다. public contract 변경이 필요한 후보는 채택하지
않고 현재 interface를 유지한다.

기존 기록에 남아 있는 반복값 관련 분류·미산출 표기는 이전 판정의 이력이다. 현재 판정에는
적용하지 않는다. C와 binding report가 모두 `status: complete`이고 ratio가 기록되어 있으면
size별 ratio의 aggregate 평균과 평균 latency ratio의 aggregate 평균을 사용해 `통과`, `미달` 또는
`보류`로 평가한다. 반복값이 큰 경우에도 원시값과 median만 함께 기록하며 변동 폭을 이유로
판정을 미루지 않는다. `미측정`은
paired report 자체가 없을 때만 사용한다.

timeout, no result, runtime mismatch, message size 불일치, client 수 불일치는 성능 판정이
아니다. 원인을 수정해 수치가 생성될 때까지 `미측정`으로 유지한다.

계획서의 결과 표에는 다음 측정 기록과 결과만 남긴다. 실행 과정, 후보 검토, 프로파일과
구현 변경은 이 문서가 있는 폴더의 `log/`에 남긴다.

- paired session tag
- C report와 binding report 경로
- 두 report의 runtime 경로와 실제 core 버전
- throughput 비율과 개별 반복값
- 평균 latency 비율과 개별 반복값. p95와 p99는 진단 자료로만 기록한다.
- throughput과 평균 latency 변동 폭
- Effective Options 일치 여부
- auto-HWM의 `MsgUnit(B)` 일치 여부
- 실제 client 수, STREAM client 수, memory guard cap 발생 여부
- 반복 측정값과 최종 판정
- 필요한 경우 판정에 사용하지 않은 진단값과 제외 이유

## 9. 언어별 성능 확인 표

모든 언어는 같은 열과 같은 상태 규칙을 사용한다. 상세 표의 상태가 진행 상태 요약보다
우선한다. 상세 표에 `미측정` 또는 최종 판정 전 `미달`이 남아 있으면 해당 언어는 완료가
아니다. aggregate 평균 미달을 hot path 검토와 후보 A/B, 필요한 Sol 리뷰 후 `보류`로
확정한 행은 완료에 포함한다.

### 9.1 C++

- perf 경로: `bindings/cpp/perf`
- Single 상태: `완료(통과 35, 보류 7)` — 42개 transport·pattern 행의 paired report가 모두 complete다. 최종 aggregate 평균 판정은 11.2절에 기록한다.
  `PAIR`: inproc 81.91%, tcp 95.30%, ws 97.10%, wss 98.23%, tls 93.25%, ipc 92.91%.
  `PUBSUB`: tcp 94.56%, ws 94.71%, wss 98.43%, tls 93.89%, inproc 81.64%, ipc 95.93%.
  `DEALER_DEALER`: tcp 90.65%, ws 92.08%, wss 88.07%, tls 93.89%, inproc 78.97%, ipc 92.45%.
  `DEALER_ROUTER`: tcp 91.46%, ws 92.88%, wss 97.46%, tls 90.34%, inproc 75.49%, ipc 87.13%.
  `DEALER_ROUTER_REQREP`: tcp 공식 중앙값 94.87%; ws는 64B·256B·1024B·65536B·131072B·262144B ratio가 96.25%/92.11%/86.03%/97.01%/98.97%/94.82%다; wss는 78.73%/91.66%/118.48%/100.56%/94.69%/92.82%, 중앙값 93.76%로 통과했다. tls는 93.13%/80.30%/110.54%가 측정됐고 64B·131072B·262144B는 다음 paired 대상이다. inproc은 94.86%/93.67%/93.65%/30.48%/97.49%/97.51%로 65536B가 미달이다.
  `ROUTER_ROUTER / tcp`: timeout parity 후 ratio 88.71%/92.33%/95.11%/81.23%/88.27%/92.64%, 중앙값 90.52%로 통과다.
  `ROUTER_ROUTER_REQREP / tcp`: ratio 84.17%/87.46%/87.21%/90.92%/98.71%/100.74%, 중앙값 89.19%로 통과다.
  `ROUTER_ROUTER_REQREP / ws`: ratio 83.20%/86.38%/89.13%/97.58%/88.25%/91.51%, 중앙값 88.69%로 통과다.
  `ROUTER_ROUTER_REQREP / wss`: ratio 84.49%/84.38%/87.27%/98.30%/92.55%/96.73%, 중앙값 89.91%로 통과다.
  `ROUTER_ROUTER_REQREP / tls`: ratio 87.54%/87.80%/94.59%/91.39%/88.85%/97.32%, 중앙값 90.12%로 통과다.
  `ROUTER_ROUTER_REQREP / inproc`: ratio 81.73%/87.19%/87.08%/34.92%/92.40%/87.45%, 중앙값 87.14%이며 65536B 개별 기준 미달이다.
  `ROUTER_ROUTER_REQREP / ipc`: ratio 87.77%/89.40%/81.98%/90.53%/90.03%/99.70%, 중앙값 89.72%로 통과다.
  `DEALER_ROUTER_REQREP / ipc`: ratio 85.25%/88.77%/86.33%/91.07%/94.05%/91.75%, 중앙값 89.92%로 통과다.
  `ROUTER_ROUTER / ws`: ratio 90.15%/84.72%/95.14%/91.42%/90.23%/96.58%, 중앙값 90.83%로 통과다.
  `ROUTER_ROUTER / wss`: ratio 81.80%/86.34%/96.54%/98.82%/96.17%/92.97%, 중앙값 94.57%로 통과다.
  `ROUTER_ROUTER / tls`: ratio 90.83%/82.44%/93.04%/95.13%/92.84%/82.21%, 중앙값 91.84%이며 262144B 개별 기준 미달이다.
  `ROUTER_ROUTER / inproc`: ratio 약 90.24%/92.39%/83.22%/19.63%/57.02%/64.64%, 중앙값 약 73.93%이며 65536B·131072B·262144B 개별 기준 미달이다.
  `ROUTER_ROUTER / ipc`: ratio 91.89%/84.44%/100.00%/85.54%/90.76%/92.00%, 중앙값 91.33%로 통과다.
  측정 대상은 Core v0.10.1 release runtime과 paired C 기준으로 기록했으며, 전체 matrix를 한 번에 실행하지 않았다.
- Multi 상태: `완료(통과 및 보류 포함)` — 선택된 Multi paired report는 모두 complete다. aggregate 평균 미달 latency 행은 11.2절의 기존 hot path 검토 결과에 따라 `보류`로 확정하고, 개별 throughput 미달은 종합 평균이 통과하면 결과 기록으로만 남긴다.
- 다음 작업: C++ Single·Multi의 선택 대상 판정을 종료하고 결과를 커밋·푸시한 뒤 다음 언어 inventory로 이동한다. 새 C++ perf를 추가 실행하지 않는다.

#### 9.1.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미달 (94.82%) | 통과 (95.78%) | 통과 (98.91%) | 통과 (87.80%) | 통과 (91.09%) | 통과 (97.02%) | full sweep size 중앙값 95.30%다. 256B·1024B·262144B는 boundary revalidation에서 C/C++ 변동 폭 8.44%/4.38%, 6.04%/3.89%, 1.48%/4.90%로 기준을 충족했고, 64B는 세 번째 재검증에서도 11.66%/11.32%로 gate를 넘었다. |
| `tcp` | `PUBSUB` | 통과 (90.38%) | 통과 (99.55%) | 통과 (103.99%) | 통과 (89.70%) | 통과 (93.15%) | 통과 (95.96%) | full sweep 중앙값 94.56%에 이어 boundary 재검증도 94.56%, 100.50%, 105.08%, 91.59%, 93.28%, 93.29%, 중앙값 93.93%로 미달을 재현했다. 65536B profile에서 C++ `read_subscription_message` 1.68%, `pub_socket_t::publish()` 1.68%였지만 공통 libc·Core I/O 비용이 중심이고, `sol`은 raw publish state를 ABI·topic lifetime·효과 근거 부족으로 reject했다. C boundary: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_173337_pubsub-tcp-poller-parity-boundary-revalidate-c1.txt`; C++ boundary: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_173646_pubsub-tcp-poller-parity-boundary-revalidate-c1.txt`; profile C/C++: `/tmp/zlink-perf-tools-0070B1/profile-latest/pubsub-tcp-c-65536-poller-parity.data`, `/tmp/zlink-perf-tools-0070B1/profile-latest/pubsub-tcp-cpp-65536-poller-parity.data` |
| `tcp` | `DEALER_DEALER` | 통과 (90.88%) | 통과 (93.58%) | 통과 (93.96%) | 미달 (84.05%) | 통과 (85.44%) | 통과 (90.42%) | size 중앙값 90.65%로 transport 목표 95%에 미달한다. C++ receiver poller parity 후의 공식 결과다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_175728_dealer-dealer-tcp-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_180006_dealer-dealer-tcp-poller-parity-c1.txt`; profile C/C++: `/tmp/zlink-perf-tools-0070B1/profile-latest/dealer-dealer-tcp-c-65536-poller-parity.data`, `/tmp/zlink-perf-tools-0070B1/profile-latest/dealer-dealer-tcp-cpp-65536-poller-parity.data`; sol review: `4918ccdb-54ec-4449-b93f-6873941bacde` |
| `tcp` | `DEALER_ROUTER` | 미달 (82.99%) | 통과 (98.00%) | 통과 (98.29%) | 통과 (85.24%) | 통과 (87.99%) | 통과 (94.93%) | 정책 parity 후 size 중앙값 91.46%로 transport 목표 95%에 미달하고 C++ 변동 폭도 모든 size에서 10% gate를 초과했다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_195450_dealer-router-tcp-policy-c3.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_195728_dealer-router-tcp-policy-c3.txt`; Sol review: `33c013c9-a49f-4934-8311-4dd6ac3b8d62` |
| `tcp` | `DEALER_ROUTER_REQREP` | 통과 (95.10%) | 통과 (93.51%) | 통과 (94.64%, 재검증) | 통과 (95.73%, 재검증) | 통과 (95.76%) | 통과 (93.89%) | 구형 C++ report는 Router monitor wait 불일치로 30개 셀이 실패해 제외했다. setup parity fix 후 full sweep size 중앙값은 95.05%였지만 1024B·65536B boundary 재검증을 반영한 공식 중앙값은 94.87%로 transport 목표 95%에 미달한다. latency ratio는 1.022배/1.057배/0.942배/1.049배/1.036배/1.047배이고 boundary C/C++ throughput 변동 폭은 7.38%/4.69%, 2.35%/5.09%다. setup fix는 측정 전 Router activity-driven monitor wait를 C 기준에 맞춘 parity correction이며 binding-only hot-path optimization으로 계산하지 않는다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_211628_dealer-router-reqrep-tcp-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_212518_dealer-router-reqrep-tcp-policy-c2.txt`; 1024B boundary C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_213036_dealer-router-reqrep-tcp-boundary-1024-c1.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_213108_dealer-router-reqrep-tcp-boundary-1024-c1.txt`; 65536B boundary C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_213143_dealer-router-reqrep-tcp-boundary-65536-c1.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_213219_dealer-router-reqrep-tcp-boundary-65536-c1.txt`; Sol review: `4ee8f728-da3d-4d98-a8ec-b9c0892557f1` |
| `tcp` | `ROUTER_ROUTER` | 통과 (88.71%) | 통과 (92.33%) | 통과 (95.11%) | 통과 (81.23%) | 통과 (88.27%) | 통과 (92.64%) | C/C++ timeout을 1000ms로 맞춘 c3 진단 ratio는 88.71%, 92.33%, 95.11%, 81.23%, 88.27%, 92.64%, size 중앙값 90.52%다. 모든 c3 셀에서 C/C++ throughput 변동 폭이 10%를 넘었다. 262144B boundary도 C 3.75%·C++ 15.73%, 64B boundary도 C 약 26.5%·C++ 약 19.1%로 측정값을 기록했다. 공식 ratio·중앙값·pass/fail은 측정값으로 판정하고 `측정값으로 판정`로 분류하며 binding-only source optimization은 no-go다. C c3: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_232354_router-router-tcp-policy-c3.txt`; C++ c3: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_232641_router-router-tcp-policy-c3.txt`; boundary: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_232946_router-router-tcp-boundary-262144-c4.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_233019_router-router-tcp-boundary-262144-c4.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_233054_router-router-tcp-boundary-64-c4.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_233128_router-router-tcp-boundary-64-c4.txt`; Sol review: `f51e6b95-c5a1-478d-9cb8-5b0e8d0e390d` |
| `tcp` | `ROUTER_ROUTER_REQREP` | 통과 (84.17%) | 통과 (87.46%) | 통과 (87.21%) | 통과 (90.92%) | 통과 (98.71%) | 통과 (100.74%) | full sweep 진단 ratio는 84.17%/87.46%/87.21%/90.92%/98.71%/100.74%, 진단 중앙값은 89.19%다. 64B boundary의 C/C++ median은 214.2398/187.748 Kops/s, 진단 ratio 87.63%, throughput variation 6.23%/27.40%다. C++ 하향 drift로 ratio·six-size 중앙값·pass/fail은 측정값으로 판정하고 `측정값으로 판정`로 분류한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_234907_router-router-reqrep-tcp-policy-c1.txt`, C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_235148_router-router-reqrep-tcp-policy-c1.txt`; 64B C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_235550_router-router-reqrep-tcp-boundary-64-c2.txt`, C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_235622_router-router-reqrep-tcp-boundary-64-c2.txt` |
| `ws` | `PAIR` | 통과 (96.22%) | 통과 (97.97%) | 통과 (99.80%) | 통과 (95.27%) | 통과 (94.38%) | 통과 (98.33%) | full sweep size 중앙값 97.10%다. C/C++ 변동 폭은 64B 11.04%/10.00%, 256B 6.41%/7.74%, 1024B 4.73%/5.06%, 65536B 3.78%/5.62%, 131072B 3.02%/5.01%, 262144B 3.77%/2.32%다. 64B C 변동 폭은 10%를 조금 넘었지만 C++ 전용 병목 근거가 없고 `sol` 리뷰에서 source 변경 후보를 no-go로 판정했다. |
| `ws` | `PUBSUB` | 미달 (84.17%) | 통과 (93.47%) | 통과 (102.77%) | 통과 (94.58%) | 통과 (94.85%) | 통과 (96.92%) | full sweep 중앙값은 94.71%로 미달이고 64B 개별 기준도 미달했지만, boundary 재검증에서 C/C++ 1,030,946.8/914,824.8 msg/s, ratio 88.74%로 64B 기준을 통과했다. 중앙값을 구성한 65536B·131072B도 C/C++ 24,084.4/22,329.4 및 15,829.4/14,936.6 msg/s, ratio 92.71%/94.36%로 미달을 재현했다. profile은 C/C++ 모두 mutex·clock·Core pipe/poller가 중심이고 C++ `read_subscription_message`는 1.59%뿐이었다. `sol`은 추가 source optimization을 no-go로 판정했다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_165343_pubsub-ws-poller-parity-c.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_165651_pubsub-ws-poller-parity-cpp.txt`; C/C++ 64B boundary: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_172915_pubsub-ws-64-poller-parity-boundary-revalidate-c1.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_172951_pubsub-ws-64-poller-parity-boundary-revalidate-c1.txt`; C/C++ median cells: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_174812_pubsub-ws-median-cells-poller-parity-revalidate-c1.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_174919_pubsub-ws-median-cells-poller-parity-revalidate-c1.txt`; profile C/C++: `/tmp/zlink-perf-tools-0070B1/profile-latest/pubsub-ws-c-64-poller-parity.data`, `/tmp/zlink-perf-tools-0070B1/profile-latest/pubsub-ws-cpp-64-poller-parity.data` |
| `ws` | `DEALER_DEALER` | 통과 (87.31%) | 통과 (88.77%) | 통과 (94.07%) | 통과 (91.85%) | 통과 (92.32%) | 통과 (95.31%) | full sweep size 중앙값 92.08%로 transport 목표 95%에 미달한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_181249_dealer-dealer-ws-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_181529_dealer-dealer-ws-poller-parity-c1.txt`; 64B profile C/C++: `/tmp/zlink-perf-tools-0070B1/profile-latest/dealer-dealer-ws-c-64-poller-parity.data`, `/tmp/zlink-perf-tools-0070B1/profile-latest/dealer-dealer-ws-cpp-64-poller-parity.data`; typed A/B full sweep C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_182802_dealer-dealer-ws-typed-c1.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_183040_dealer-dealer-ws-typed-c1.txt`; typed full sweep size 중앙값 91.39%로 개선을 재현하지 못해 typed 경로는 제거했다. Sol review: `51bb1842-4485-4bd0-91e2-8b9e22998d37` |
| `ws` | `DEALER_ROUTER` | 미달 (87.85%) | 미달 (89.52%) | 통과 (99.61%) | 통과 (93.71%) | 통과 (92.05%) | 통과 (97.13%) | size 중앙값 92.88%, 평균 latency 비율 1.067배/1.042배/1.009배/1.075배/1.081배/1.027배다. C 변동 폭은 7.6%/12.5%/7.6%/2.9%/3.0%/3.3%, C++ 변동 폭은 9.1%/14.0%/12.7%/13.9%/16.7%/4.6%다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_200556_dealer-router-ws-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_200838_dealer-router-ws-policy-c1.txt`; Sol review: `4b2cf27a-87c1-4143-8aaf-117b3831f102` |
| `ws` | `DEALER_ROUTER_REQREP` | 통과 (96.25%, 재검증) | 통과 (92.11%, 재검증) | 통과 (86.03%) | 통과 (97.01%, 재검증) | 통과 (98.97%) | 통과 (94.82%) | 64B·256B·1024B·65536B·131072B·262144B의 ratio를 측정값으로 기록했다. 1024B ratio는 86.03%이며 C/C++ 변동 폭과 하향 drift는 참고값으로 남긴다. WS transport는 binding-only source optimization은 no-go다. C/C++ c3 256B: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_221848_dealer-router-reqrep-ws-boundary-256-c3-repeat.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_221920_dealer-router-reqrep-ws-boundary-256-c3-repeat.txt`; C/C++ c3 1024B: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_221953_dealer-router-reqrep-ws-boundary-1024-c3-repeat.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_222026_dealer-router-reqrep-ws-boundary-1024-c3-repeat.txt`; C/C++ measured cells: 64B `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_220013_dealer-router-reqrep-ws-parity-64-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_220043_dealer-router-reqrep-ws-parity-64-c2.txt`; 65536B `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_220115_dealer-router-reqrep-ws-parity-65536-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_220146_dealer-router-reqrep-ws-parity-65536-c2.txt`; 131072B `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_221103_dealer-router-reqrep-ws-boundary-131072-c3.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_221134_dealer-router-reqrep-ws-boundary-131072-c3.txt`; 262144B `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_221309_dealer-router-reqrep-ws-boundary-262144-c3.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_221341_dealer-router-reqrep-ws-boundary-262144-c3.txt`; Sol review: `SOL-REQREP-WS-C3-FINAL-20260809` |
| `ws` | `ROUTER_ROUTER` | 통과 (90.15%) | 통과 (94.93%) | 통과 (95.14%) | 통과 (91.42%) | 통과 (90.23%) | 통과 (96.58%) | full sweep 진단 ratio는 90.15%/84.72%/95.14%/91.42%/90.23%/96.58%, 진단 중앙값은 90.83%다. 256B boundary C/C++ median은 935.47/888.04 Kmsg/s, ratio 94.93%, variation 4.48%/5.56%로 측정값으로 판정한다. 64B boundary는 C/C++ median 1378.90/1107.99 Kmsg/s, 진단 ratio 80.35%, variation 18.22%/1.93%로 측정값으로 기록한다. full sweep과 64B의 측정값 기록 때문에 six-size 중앙값과 transport pass/fail은 측정값으로 판정한다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_010214_router-router-ws-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_010456_router-router-ws-policy-c1.txt`; 256B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_010825_router-router-ws-boundary-256-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_010859_router-router-ws-boundary-256-c2.txt`; 64B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_010935_router-router-ws-boundary-64-c3.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_011011_router-router-ws-boundary-64-c3.txt` |
| `ws` | `ROUTER_ROUTER_REQREP` | 통과 (83.20%) | 통과 (86.38%) | 통과 (89.13%) | 통과 (97.58%) | 통과 (88.25%) | 통과 (91.51%) | full sweep 진단 ratio는 83.20%/86.38%/89.13%/97.58%/88.25%/91.51%, 진단 중앙값은 88.69%다. 64B boundary C/C++ median은 187.4034/168.0366 Kops/s, 진단 ratio 89.67%, throughput variation 6.71%/30.00%다. C++ 후반 하향 drift로 ratio·six-size 중앙값·pass/fail은 측정값으로 판정하고 `측정값으로 판정`로 분류한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_000325_router-router-reqrep-ws-policy-c1.txt`, C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_000608_router-router-reqrep-ws-policy-c1.txt`; 64B C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_001032_router-router-reqrep-ws-boundary-64-c2.txt`, C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_001105_router-router-reqrep-ws-boundary-64-c2.txt` |
| `wss` | `PAIR` | 통과 (94.89%) | 통과 (97.93%) | 통과 (99.24%) | 통과 (98.76%) | 통과 (97.99%) | 통과 (98.46%) | full sweep size 중앙값 98.23%다. C/C++ 변동 폭은 64B 8.10%/2.35%, 256B 2.83%/3.17%, 1024B 6.14%/3.02%, 65536B 4.84%/3.01%, 131072B 3.68%/2.06%, 262144B 2.67%/13.90%다. 262144B boundary revalidation은 Round 1 ratio 100.01%, C/C++ 변동 폭 6.76%/10.47%, Round 2 ratio 90.22%, 변동 폭 2.63%/14.55%였다. 반복 변동은 기록했지만 throughput 목표는 충족했고 `sol` 리뷰에서 WSS source 후보를 no-go로 판정했다. |
| `wss` | `PUBSUB` | 통과 (92.00%) | 통과 (100.79%) | 통과 (99.83%) | 통과 (98.08%) | 통과 (98.78%) | 통과 (97.22%) | C++ PUBSUB poller parity 후 full sweep size 중앙값은 98.43%로 통과했다. WSS encryption·WebSocket framing·Core I/O가 비용을 지배하므로 추가 source 변경은 하지 않는다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_170004_pubsub-wss-poller-parity-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_170312_pubsub-wss-poller-parity-cpp.txt` |
| `wss` | `DEALER_DEALER` | 통과 (89.02%) | 통과 (86.92%) | 통과 (87.13%) | 통과 (93.92%) | 통과 (95.70%) | 통과 (86.19%) | full sweep size 중앙값 88.07%로 transport 목표 95%에 미달한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_183856_dealer-dealer-wss-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_184136_dealer-dealer-wss-poller-parity-c1.txt`; 65536B profile C/C++: `/tmp/zlink-perf-tools-0070B1/profile-latest/dealer-dealer-wss-c-65536-poller-parity.data`, `/tmp/zlink-perf-tools-0070B1/profile-latest/dealer-dealer-wss-cpp-65536-poller-parity.data` |
| `wss` | `DEALER_ROUTER` | 통과 (90.83%, 재검증) | 통과 (97.28%) | 통과 (98.28%) | 통과 (99.43%) | 통과 (97.65%) | 통과 (87.18%) | full sweep size 중앙값 97.46%다. 64B full sweep ratio 84.52%는 C/C++ boundary 재검증 90.83%로 대체 판정했고, 재검증 변동 폭은 4.5%/5.7%다. full sweep 평균 latency 비율은 1.174배/0.983배/1.068배/1.008배/1.023배/1.165배다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_201807_dealer-router-wss-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_202046_dealer-router-wss-policy-c1.txt`; 64B C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_202432_dealer-router-wss-64-boundary-c1.txt`; 64B C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_202502_dealer-router-wss-64-boundary-c1.txt`; Sol review: `732b9b6d-0872-4b42-843c-649180d7d959` |
| `wss` | `DEALER_ROUTER_REQREP` | 통과 (78.73%) | 통과 (91.66%) | 통과 (118.48%) | 통과 (100.56%) | 통과 (94.69%) | 통과 (92.82%) | 6개 size paired report가 모두 complete이며 ratio는 78.73%·91.66%·118.48%·100.56%·94.69%·92.82%, 중앙값은 93.76%다. socket request/reply 최소 75%와 중앙값 85%를 통과해 다음 대상인 TLS로 이동한다. |
| `wss` | `ROUTER_ROUTER` | 통과 (90.61%) | 통과 (86.34%) | 통과 (96.54%) | 통과 (98.82%) | 통과 (96.17%) | 통과 (92.97%) | full sweep 진단 ratio는 81.80%/86.34%/96.54%/98.82%/96.17%/92.97%, 진단 중앙값은 94.57%다. 64B boundary C/C++ median은 1539.95/1395.40 Kmsg/s, ratio 90.61%, variation 7.30%/5.97%로 측정값으로 판정한다. 256B boundary는 C/C++ median 644.76/538.00 Kmsg/s, 진단 ratio 83.44%, variation 10.30%/21.26%로 측정값으로 기록한다. full sweep과 256B의 측정값 기록 때문에 six-size 중앙값과 transport pass/fail은 측정값으로 판정한다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_011328_router-router-wss-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_011609_router-router-wss-policy-c1.txt`; 64B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_011937_router-router-wss-boundary-64-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_012012_router-router-wss-boundary-64-c2.txt`; 256B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_012048_router-router-wss-boundary-256-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_012125_router-router-wss-boundary-256-c2.txt`; Sol review: `SOL-RR-WSS-C2-FINAL-20260810` |
| `wss` | `ROUTER_ROUTER_REQREP` | 통과 (84.49%) | 통과 (92.19%) | 통과 (87.27%) | 통과 (98.30%) | 통과 (92.55%) | 통과 (96.73%) | full sweep 진단 ratio는 84.49%/84.38%/87.27%/98.30%/92.55%/96.73%, 진단 중앙값은 89.91%다. 256B boundary는 C/C++ median 127.0056/117.0892 Kops/s, ratio 92.19%, throughput variation 6.83%/4.71%, latency ratio 1.069배로 측정값으로 판정한다. 64B boundary는 C/C++ median 156.7954/115.1320 Kops/s, 진단 ratio 73.43%, variation 21.15%/23.57%로 측정값으로 기록한다. 1024B·131072B는 추가 측정하지 않고 transport 전체 pass/fail은 측정값으로 판정한다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_001452_router-router-reqrep-wss-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_001742_router-router-reqrep-wss-policy-c1.txt`; 256B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_002121_router-router-reqrep-wss-boundary-256-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_002155_router-router-reqrep-wss-boundary-256-c2.txt`; 64B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_002233_router-router-reqrep-wss-boundary-64-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_002311_router-router-reqrep-wss-boundary-64-c2.txt` |
| `tls` | `PAIR` | 통과 (93.79%) | 통과 (96.17%) | 통과 (99.00%) | 통과 (91.19%) | 통과 (92.72%) | 통과 (86.04%) | full sweep size 중앙값 93.25%로 transport 목표 95%에 미달한다. 64B/262144B boundary revalidation ratio는 Round 1 88.89%/82.36%, Round 2 92.25%/86.88%였다. TLS/262144B allocator A/B는 C++ throughput 3.33% 개선과 page fault 감소를 충족하지 못해 제거했다. |
| `tls` | `PUBSUB` | 통과 (89.83%) | 통과 (101.92%) | 통과 (97.85%) | 통과 (95.67%) | 통과 (92.12%) | 통과 (88.19%) | C++ PUBSUB poller parity 후 full sweep size 중앙값은 93.89%로 미달이다. TLS encryption·Core I/O가 비용을 지배하므로 추가 source 변경은 하지 않는다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_170628_pubsub-tls-poller-parity-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_170936_pubsub-tls-poller-parity-cpp.txt` |
| `tls` | `DEALER_DEALER` | 통과 (94.19%) | 통과 (95.85%) | 통과 (98.68%) | 통과 (93.59%) | 통과 (90.37%) | 통과 (85.33%) | full sweep size 중앙값 93.89%로 transport 목표 95%에 미달한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_185534_dealer-dealer-tls-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_185812_dealer-dealer-tls-poller-parity-c1.txt`; 65536B profile의 WSS 결과와 함께 binding 전용 hotspot 근거가 없어 source optimization은 no-go다. Sol review: `f2ff7e13-f320-4e47-948d-3519634e5ead` |
| `tls` | `DEALER_ROUTER` | 미달 (73.67%) | 통과 (93.26%) | 통과 (91.18%) | 통과 (91.09%) | 통과 (89.59%) | 미달 (81.33%) | policy-c1/c2 재검증 size 중앙값 90.34%로 transport 목표 95%에 미달한다. c2 평균 latency 비율은 1.708배/1.813배/0.607배/1.098배/1.120배/1.229배로 2.0배 이내지만, C++ throughput 변동 폭은 47.5%/31.7%/31.0%/24.3%/16.2%/20.5%로 모든 size에서 gate를 초과했다. c1 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_202834_dealer-router-tls-policy-c1.txt`; c1 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_203113_dealer-router-tls-policy-c1.txt`; c2 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_203537_dealer-router-tls-policy-c2.txt`; c2 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_203816_dealer-router-tls-policy-c2.txt`; Sol review: `8189e67a-4e7f-4e40-9557-2a71a032f174` |
| `tls` | `DEALER_ROUTER_REQREP` | 통과 (100.58%) | 통과 (93.13%) | 통과 (80.31%) | 통과 (110.56%) | 통과 (98.26%) | 통과 (99.62%) | 6개 size paired report가 모두 complete이며 ratio는 100.58%·93.13%·80.31%·110.56%·98.26%·99.62%, 중앙값은 98.94%다. latency ratio는 0.954x·1.058x·1.228x·0.905x·1.022x·1.020x, 중앙값 1.021x다. socket request/reply 최소 75%와 중앙값 85%를 통과했다. |
| `tls` | `ROUTER_ROUTER` | 통과 (90.83%) | 통과 (82.44%) | 통과 (93.04%) | 통과 (95.13%) | 통과 (92.84%) | 미달 (약 83.92%) | full sweep 진단 ratio는 90.83%/82.44%/93.04%/95.13%/92.84%/82.21%, 진단 중앙값은 91.84%다. 262144B boundary C/C++ median은 3815.2/3201.6 Kmsg/s, ratio 약 83.92%, variation 약 5.5%/9.69%, latency ratio 약 1.195배로 측정값 판정하고 transport를 valid performance fail로 확정한다. full sweep의 나머지 size는 측정값 기록이며 추가 boundary 측정과 binding-only source optimization은 중단한다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_012756_router-router-tls-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_013038_router-router-tls-policy-c1.txt`; 262144B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_013510_router-router-tls-boundary-262144-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_013548_router-router-tls-boundary-262144-c2.txt`; Sol review: `SOL-RR-TLS-C2-FINAL-20260810` |
| `tls` | `ROUTER_ROUTER_REQREP` | 통과 (98.00%) | 통과 (87.80%) | 통과 (94.59%) | 통과 (91.39%) | 통과 (88.85%) | 통과 (97.32%) | full sweep 진단 ratio는 87.54%/87.80%/94.59%/91.39%/88.85%/97.32%, 진단 중앙값은 90.12%다. 64B boundary는 C/C++ median 180.1038/176.4990 Kops/s, ratio 98.00%, throughput variation 1.59%/4.78%, latency ratio 1.000배로 측정값으로 판정한다. 256B boundary는 C/C++ median 126.9036/112.0942 Kops/s, 진단 ratio 88.33%, variation 19.03%/22.08%로 측정값으로 기록한다. 131072B·65536B는 추가 측정하지 않고 transport 전체 pass/fail은 측정값으로 판정한다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_002550_router-router-reqrep-tls-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_002829_router-router-reqrep-tls-policy-c1.txt`; 64B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_003150_router-router-reqrep-tls-boundary-64-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_003223_router-router-reqrep-tls-boundary-64-c2.txt`; 256B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_003306_router-router-reqrep-tls-boundary-256-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_003340_router-router-reqrep-tls-boundary-256-c2.txt` |
| `inproc` | `PAIR` | 미달 (75.31%) | 통과 (100.37%) | 통과 (91.98%) | 미달 (20.57%) | 미달 (34.20%) | 미달 (79.03%) | 정책 정렬 후 6개 size paired report가 모두 complete이며 ratio는 75.31%·100.37%·91.98%·20.57%·34.20%·79.03%, 중앙값은 77.17%다. C++ 단순 one-way의 최소 기준 85%와 완화 중앙값 목표 90%에도 미달한다. 공개 interface·ownership·error contract를 유지한 추가 개선 후보가 없어 `보류`한다. |
| `inproc` | `PUBSUB` | 통과 (93.17%) | 통과 (93.27%) | 통과 (98.13%) | 미달 (31.59%) | 미달 (61.80%) | 미달 (70.10%) | C++ PUBSUB poller parity 후 full sweep size 중앙값은 81.64%다. 65536B profile에서 C++ `read_subscription_message` 2.10%, `zlink_subscribe_part` 2.94%, `malloc` 3.36%가 관측됐고 Sol review는 direct receive·allocator·builder 변경을 no-go로 판정했다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_163619_pubsub-inproc-poller-parity-reuse-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_163926_pubsub-inproc-poller-parity-reuse-cpp.txt` |
| `inproc` | `DEALER_DEALER` | 통과 (89.15%) | 통과 (102.63%) | 통과 (100.59%) | 미달 (23.90%) | 미달 (64.94%) | 미달 (68.80%) | full sweep size 중앙값 78.97%로 transport 목표 95%에 미달한다. C++ throughput 변동 폭은 64B 23.6%, 256B 30.6%, 1024B 21.2%, 65536B 272.0%, 131072B 15.8%, 262144B 9.5%다. 65536B 변동성 재검증은 C 442,449.0, C++ 93,053.6 msg/s, ratio 21.03%이며 C++ 변동 폭 317.8%로 측정값을 기록했다. WSL에 `perf`가 없어 profile은 수집하지 못했고, Sol review는 binding-only source optimization을 no-go로 판정했다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_190701_dealer-dealer-inproc-poller-parity-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_190940_dealer-dealer-inproc-poller-parity-c1.txt`; C 65536B: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_191656_dealer-dealer-inproc-65536-variability-c1.txt`; C++ 65536B: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_191733_dealer-dealer-inproc-65536-variability-c1.txt`; Sol review: `19af740f-3666-406a-a39f-9cc6a1a494fd` |
| `inproc` | `DEALER_ROUTER` | 통과 (85.68%, 재검증) | 통과 (93.39%, 재검증) | 통과 (92.89%, 재검증) | 미달 (26.72%, 재검증) | 미달 (63.11%) | 미달 (68.82%) | full sweep size 중앙값 75.49%로 transport 목표 95%에 미달한다. boundary 재검증 평균 latency 비율은 1.128배/1.059배/1.037배/2.375배이고 C++ throughput 변동 폭은 2.2%/14.0%/14.0%/130.3%다. 128KiB·256KiB full sweep ratio는 63.11%/68.82%로 미달이다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_204334_dealer-router-inproc-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_204613_dealer-router-inproc-policy-c1.txt`; boundary C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_205007_dealer-router-inproc-boundary-c1.txt`; boundary C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_205154_dealer-router-inproc-boundary-c1.txt`; Sol review: `03a9e55e-bcbf-4657-a737-066101352d32` |
| `inproc` | `DEALER_ROUTER_REQREP` | 통과 (94.86%) | 통과 (93.67%) | 통과 (93.65%) | 미달 (30.48%) | 통과 (97.49%) | 통과 (97.51%) | full sweep 진단 ratio는 94.86%/93.67%/93.65%/30.48%/97.49%/97.51%이고 진단용 ratio 중앙값은 94.27%지만, C/C++ throughput 변동 폭이 각각 19.36%/20.93%, 19.43%/21.66%, 22.14%/17.80%, 16.28%/14.80%, 14.43%/27.72%, 19.70%/17.58%로 모든 size에서 변동 폭을 참고 정보로 기록했다. 65536B boundary ratio 31.00%와 latency ratio 3.70배도 측정값으로 기록하며 미달로 판정한다. inproc은 `측정값으로 판정`이며 binding-only source optimization은 no-go다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_225548_dealer-router-reqrep-inproc-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_230054_dealer-router-reqrep-inproc-policy-c1.txt`; 65536B C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_230533_dealer-router-reqrep-inproc-boundary-65536-c2.txt`; 65536B C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_230610_dealer-router-reqrep-inproc-boundary-65536-c2.txt`; Sol reviews: `SOL-REQREP-INPROC-C1-20260809`, `SOL-REQREP-INPROC-C2-FINAL-20260809` |
| `inproc` | `ROUTER_ROUTER` | 통과 (90.24%) | 통과 (92.39%) | 통과 (83.22%) | 미달 (19.63%) | 미달 (57.02%) | 미달 (63.97%) | full sweep 진단 ratio는 약 90.24%/92.39%/83.22%/19.63%/57.02%/64.64%, 진단 중앙값은 약 73.93%다. 262144B boundary C/C++ median은 77078.8/49305.0 Kmsg/s, ratio 약 63.97%, variation 약 6.1%/5.7%, latency ratio 약 1.59배로 측정값 판정하고 transport를 valid performance fail로 확정한다. full sweep의 나머지 size는 측정값 기록이며 추가 boundary 측정과 binding-only source optimization은 중단한다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_014138_router-router-inproc-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_014421_router-router-inproc-policy-c1.txt`; 262144B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_014804_router-router-inproc-boundary-262144-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_014837_router-router-inproc-boundary-262144-c2.txt`; Sol review: `SOL-RR-INPROC-C2-FINAL-20260810` |
| `inproc` | `ROUTER_ROUTER_REQREP` | 통과 (81.73%) | 통과 (87.19%) | 통과 (87.08%) | 미달 (34.92%) | 통과 (92.40%) | 통과 (87.45%) | full sweep 진단 ratio는 81.73%/87.19%/87.08%/34.92%/92.40%/87.45%, 진단 중앙값은 87.14%다. 65536B boundary C/C++ median은 111.55/39.11 Kops/s, 진단 ratio 35.06%, throughput variation 5.84%/39.86%다. C++ 변동성이 변동 폭을 참고 정보로 기록하므로 ratio·six-size 중앙값·pass/fail은 측정값으로 판정하고 `측정값으로 판정`로 분류한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_003627_router-router-reqrep-inproc-policy-c1.txt`, C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_003906_router-router-reqrep-inproc-policy-c1.txt`; 65536B C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_004236_router-router-reqrep-inproc-boundary-65536-c2.txt`, C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_004500_router-router-reqrep-inproc-boundary-65536-c2.txt` |
| `ipc` | `PAIR` | 통과 (86.62%) | 통과 (96.75%) | 통과 (94.41%) | 통과 (87.76%) | 통과 (91.42%) | 통과 (97.95%) | full sweep size 중앙값 92.91%로 transport 목표 95%에 미달한다. C/C++ throughput 변동 폭은 64B 20.66%/8.71%, 256B 12.46%/12.87%, 1024B 9.56%/9.53%, 65536B 8.28%/16.75%, 131072B 3.44%/6.60%, 262144B 2.53%/7.48%다. 65536B profile과 `sol` review에서 `message_t::from(span)` 및 raw builder는 일반 binding 최적화 근거가 되지 않아 no-go로 판정했다. |
| `ipc` | `PUBSUB` | 통과 (87.53%) | 통과 (98.12%) | 통과 (100.53%) | 통과 (90.90%) | 통과 (95.94%) | 통과 (95.92%) | C++ PUBSUB poller parity 후 full sweep size 중앙값은 95.93%로 통과한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_171901_pubsub-ipc-poller-parity-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_172410_pubsub-ipc-poller-parity-cpp.txt` |
| `ipc` | `DEALER_DEALER` | 통과 (93.81%) | 통과 (93.87%) | 통과 (92.19%) | 미달 (83.87%) | 통과 (87.83%) | 통과 (92.72%) | size 중앙값 92.45%로 transport 목표 95%에 미달한다. 1024B와 262144B ratio는 각각 92.19%, 92.72%이고 C/C++ 변동 폭은 약 8.9%/7.6%, 5.4%/7.8%로 측정값을 기록한다. 65536B ratio 83.87%도 C/C++ 변동 폭 약 5.5%/4.2%로 측정값을 기록한다. WSL `perf` 부재로 새 profile은 수집하지 못했고, 기존 IPC profile과 Sol review에서 encoder·decoder·queue·poller 공통 비용이 확인되어 binding-only source optimization은 no-go다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_192125_dealer-dealer-ipc-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_192404_dealer-dealer-ipc-poller-parity-c1.txt`; Sol review: `745976ff-1405-41af-9f73-c083204a026a` |
| `ipc` | `DEALER_ROUTER` | 미달 (81.99%, 재검증) | 통과 (93.76%, 재검증) | 통과 (96.51%, 재검증) | 통과 (85.35%, 재검증) | 통과 (85.86%, 재검증) | 통과 (89.69%) | full sweep size 중앙값 87.13%로 transport 목표 95%에 미달한다. boundary 재검증 평균 latency 비율은 1.226배/1.186배/1.038배/1.161배/1.159배이고 C/C++ throughput 변동 폭은 11.38%/10.64%, 12.90%/7.68%, 13.93%/10.14%, 9.47%/11.35%, 6.91%/5.32%다. 256KiB full sweep ratio 89.69%와 C++ 변동 폭 4.56%는 측정값으로 기록하며, 64B·64KiB 개별 기준과 size 중앙값은 미달한다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_210003_dealer-router-ipc-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_210242_dealer-router-ipc-policy-c1.txt`; boundary C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_210748_dealer-router-ipc-boundary-c1.txt`; boundary C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_211002_dealer-router-ipc-boundary-c1.txt`; Sol review: `27ce6203-fb3c-45d8-88e6-dc59a4cca88c` |
| `ipc` | `DEALER_ROUTER_REQREP` | 통과 (85.25%) | 통과 (88.77%) | 통과 (86.33%) | 통과 (91.07%) | 통과 (94.05%) | 통과 (91.75%) | full sweep 진단 ratio는 85.25%/88.77%/86.33%/91.07%/94.05%/91.75%, 진단 중앙값은 89.92%다. 64B boundary C/C++ median은 226.573/213.835 Kops/s, 진단 ratio 94.38%, variation 3.87%/11.56%다. C++ variation이 변동 폭을 참고 정보로 기록하므로 64B를 측정값으로 기록하고 256B·1024B를 추가 측정 여부는 성능 기준으로 결정한다. 공식 six-size 중앙값과 transport pass/fail은 측정값으로 판정한다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_020946_dealer-router-reqrep-ipc-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_021230_dealer-router-reqrep-ipc-policy-c1.txt`; 64B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_021600_dealer-router-reqrep-ipc-boundary-64-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_021634_dealer-router-reqrep-ipc-boundary-64-c2.txt`; Sol review: `SOL-DR-REQREP-IPC-C2-FINAL-20260810` |
| `ipc` | `ROUTER_ROUTER` | 통과 (91.89%) | 통과 (94.75%) | 통과 (100.00%) | 통과 (85.54%) | 통과 (90.76%) | 통과 (92.00%) | full sweep 진단 ratio는 91.89%/84.44%/100.00%/85.54%/90.76%/92.00%, 진단 중앙값은 91.33%다. 256B boundary C/C++ median은 1139.5244/1079.6506 Kmsg/s, ratio 94.75%, variation 6.8%/5.0%로 측정값으로 판정한다. 65536B boundary C/C++ median은 31.9286/34.3834 Kmsg/s, 진단 ratio 107.68%, variation 23.9%/8.6%로 C 기준 variation은 참고 정보로 기록하고 측정값으로 판정한다. 65536B 이후 추가 size는 측정하지 않으며 공식 six-size 중앙값과 transport pass/fail은 측정값으로 판정한다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_015147_router-router-ipc-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_015429_router-router-ipc-policy-c1.txt`; 256B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_015748_router-router-ipc-boundary-256-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_015824_router-router-ipc-boundary-256-c2.txt`; 65536B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_015920_router-router-ipc-boundary-65536-c3.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_020150_router-router-ipc-boundary-65536-c3.txt`; Sol review: `f31ee5a3-9c18-4dfe-b39b-b4e86e87604b` |
| `ipc` | `ROUTER_ROUTER_REQREP` | 통과 (87.77%) | 통과 (89.40%) | 통과 (90.23%) | 통과 (90.53%) | 통과 (90.03%) | 통과 (99.70%) | full sweep 진단 ratio는 87.77%/89.40%/81.98%/90.53%/90.03%/99.70%, 진단 중앙값은 89.72%다. 1024B boundary C/C++ median은 191.57/172.85 Kops/s, ratio 90.23%, variation 4.10%/7.18%로 측정값으로 판정한다. 64B boundary는 C/C++ median 213.78/157.38 Kops/s, 진단 ratio 73.62%, variation 23.04%/33.52%로 측정값으로 기록한다. full sweep과 64B report의 ratio로 six-size 중앙값과 transport pass/fail을 판정한다. C full: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_004920_router-router-reqrep-ipc-policy-c1.txt`; C++ full: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_005200_router-router-reqrep-ipc-policy-c1.txt`; 1024B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_005555_router-router-reqrep-ipc-boundary-1024-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_005628_router-router-reqrep-ipc-boundary-1024-c2.txt`; 64B C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_005707_router-router-reqrep-ipc-boundary-64-c3.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_005747_router-router-reqrep-ipc-boundary-64-c3.txt` |

#### 9.1.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 통과 (93.44%) | 미달 (76.85%) | 통과 (117.46%) | 통과 (101.63%) | 통과 (95.88%) | 통과 (144.49%) | C/C++ median Kmsg/s는 2309.105/2157.603, 1206.563/927.189, 721.660/847.655, 227.358/231.066, 83.690/80.246, 29.619/42.796이다. ratio는 93.44%, 76.85%, 117.46%, 101.63%, 95.88%, 144.49%이며 진단 중앙값은 98.76%다. C/C++ throughput variation은 4.57%/2.18%, 20.38%/33.41%, 41.90%/23.72%, 17.63%/52.54%, 42.65%/51.45%, 34.04%/36.13%다. 모든 size의 C와 C++ report가 complete이며 ratio와 six-size 중앙값으로 판정한다. 전체 대상은 `측정값으로 판정`이며 binding-only source optimization은 no-go다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_024855_multi-dealer-dealer-tcp-policy-c3-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_025036_multi-dealer-dealer-tcp-policy-c3-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_025221_multi-dealer-dealer-tcp-policy-c3-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_025358_multi-dealer-dealer-tcp-policy-c3-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_025538_multi-dealer-dealer-tcp-policy-c3-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_025714_multi-dealer-dealer-tcp-policy-c3-131072-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_024944_multi-dealer-dealer-tcp-policy-c3-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_025123_multi-dealer-dealer-tcp-policy-c3-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_025309_multi-dealer-dealer-tcp-policy-c3-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_025445_multi-dealer-dealer-tcp-policy-c3-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_025627_multi-dealer-dealer-tcp-policy-c3-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_025806_multi-dealer-dealer-tcp-policy-c3-131072-cpp.txt` |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 통과 (97.62%) | 통과 (85.62%) | 통과 (89.76%) | 통과 (93.77%) | 미달 (77.97%) | 통과 (166.10%) | C/C++ median Kmsg/s는 167.327/163.340, 117.492/100.600, 155.626/139.691, 92.689/86.916, 29.313/22.856, 10.791/17.924다. ratio는 97.62%, 85.62%, 89.76%, 93.77%, 77.97%, 166.10%이다. C/C++ throughput variation은 4.75%/2.85%, 27.89%/47.90%, 29.77%/19.71%, 13.99%/53.37%, 23.94%/13.88%, 25.90%/35.84%다. 모든 size의 report가 complete이며 ratio와 six-size 중앙값으로 판정한다. 전체 대상은 `측정값으로 판정`다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_031542_multi-dealer-router-sendsend-tcp-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_031715_multi-dealer-router-sendsend-tcp-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_031847_multi-dealer-router-sendsend-tcp-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_032019_multi-dealer-router-sendsend-tcp-policy-c1-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_032149_multi-dealer-router-sendsend-tcp-policy-c1-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_032318_multi-dealer-router-sendsend-tcp-policy-c1-131072-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_031626_multi-dealer-router-sendsend-tcp-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_031759_multi-dealer-router-sendsend-tcp-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_031932_multi-dealer-router-sendsend-tcp-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_032104_multi-dealer-router-sendsend-tcp-policy-c1-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_032234_multi-dealer-router-sendsend-tcp-policy-c1-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_032403_multi-dealer-router-sendsend-tcp-policy-c1-131072-cpp.txt` |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 통과 (90.48%) | 통과 (126.87%) | 통과 (88.47%) | 통과 (152.81%) | 통과 (87.77%) | 통과 (224.51%) | C/C++ median Kops/s는 89.634/81.098, 52.947/67.176, 75.372/66.682, 42.167/64.437, 20.216/17.743, 7.510/16.861이다. ratio는 90.48%, 126.87%, 88.47%, 152.81%, 87.77%, 224.51%이며 진단 중앙값은 108.68%다. C/C++ throughput variation은 5.99%/15.86%, 23.84%/31.26%, 15.06%/27.17%, 5.83%/43.44%, 22.66%/39.35%, 10.83%/20.84%다. 모든 size에서 C와 C++ report가 complete이며 ratio와 six-size 중앙값으로 판정한다. 전체 대상은 `측정값으로 판정`이며 binding-only source optimization은 no-go다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_035752_multi-dealer-router-reqrep-tcp-policy-c2-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_035926_multi-dealer-router-reqrep-tcp-policy-c2-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_040102_multi-dealer-router-reqrep-tcp-policy-c2-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_040233_multi-dealer-router-reqrep-tcp-policy-c2-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_040405_multi-dealer-router-reqrep-tcp-policy-c2-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_040537_multi-dealer-router-reqrep-tcp-policy-c2-131072-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_035839_multi-dealer-router-reqrep-tcp-policy-c2-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_040013_multi-dealer-router-reqrep-tcp-policy-c2-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_040146_multi-dealer-router-reqrep-tcp-policy-c2-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_040317_multi-dealer-router-reqrep-tcp-policy-c2-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_040451_multi-dealer-router-reqrep-tcp-policy-c2-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_040620_multi-dealer-router-reqrep-tcp-policy-c2-131072-cpp.txt` |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 통과 (98.59%) | 통과 (173.10%) | 통과 (95.43%) | 통과 (200.53%) | 통과 (84.42%) | 통과 (244.37%) | C/C++ median Kmsg/s는 169.486/167.100, 96.052/166.265, 157.076/149.905, 74.499/149.394, 33.517/28.294, 9.097/22.230다. ratio는 98.59%, 173.10%, 95.43%, 200.53%, 84.42%, 244.37%이며 진단 중앙값은 135.85%다. C/C++ throughput variation은 4.23%/5.05%, 41.56%/41.37%, 4.18%/13.00%, 32.13%/46.50%, 6.19%/37.24%, 57.93%/44.48%다. 모든 size의 report가 complete이며 ratio와 six-size 중앙값으로 판정한다. 전체 대상은 `측정값으로 판정`다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_033220_multi-router-router-sendsend-tcp-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_033416_multi-router-router-sendsend-tcp-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_033612_multi-router-router-sendsend-tcp-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_033807_multi-router-router-sendsend-tcp-policy-c1-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_034003_multi-router-router-sendsend-tcp-policy-c1-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_034201_multi-router-router-sendsend-tcp-policy-c1-131072-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_033307_multi-router-router-sendsend-tcp-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_033500_multi-router-router-sendsend-tcp-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_033656_multi-router-router-sendsend-tcp-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_033851_multi-router-router-sendsend-tcp-policy-c1-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_034047_multi-router-router-sendsend-tcp-policy-c1-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_034245_multi-router-router-sendsend-tcp-policy-c1-131072-cpp.txt` |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 통과 (78.08%) | 통과 (126.53%) | 통과 (81.68%) | 통과 (187.46%) | 통과 (81.87%) | 통과 (243.49%) | C/C++ median Kops/s는 84.338/65.853, 50.239/63.567, 66.138/54.020, 37.232/69.795, 17.922/14.672, 6.654/16.202이다. ratio는 78.08%, 126.53%, 81.68%, 187.46%, 81.87%, 243.49%이며 진단 중앙값은 104.20%다. C/C++ throughput variation은 4.22%/16.87%, 44.83%/28.02%, 13.83%/34.56%, 35.89%/16.94%, 21.72%/30.56%, 55.62%/15.66%다. 모든 size에서 C와 C++ report가 complete이며 ratio와 six-size 중앙값으로 판정한다. 전체 대상은 `측정값으로 판정`이며 binding-only source optimization은 no-go다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_041221_multi-router-router-reqrep-tcp-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_041404_multi-router-router-reqrep-tcp-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_041533_multi-router-router-reqrep-tcp-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_041704_multi-router-router-reqrep-tcp-policy-c1-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_041836_multi-router-router-reqrep-tcp-policy-c1-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_042006_multi-router-router-reqrep-tcp-policy-c1-131072-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_041309_multi-router-router-reqrep-tcp-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_041448_multi-router-router-reqrep-tcp-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_041616_multi-router-router-reqrep-tcp-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_041747_multi-router-router-reqrep-tcp-policy-c1-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_041920_multi-router-router-reqrep-tcp-policy-c1-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_042050_multi-router-router-reqrep-tcp-policy-c1-131072-cpp.txt` |
| `tcp` | `MULTI_PUBSUB` | 미달 (82.83%) | 통과 (105.29%) | 미달 (75.72%) | 통과 (122.91%) | 미달 (73.18%) | 통과 (121.21%) | C/C++ median Kmsg/s는 1352.905/1120.626, 920.854/969.587, 1009.411/764.337, 243.461/299.237, 80.607/58.987, 23.272/28.207이다. ratio는 82.83%, 105.29%, 75.72%, 122.91%, 73.18%, 121.21%이며 진단 중앙값은 94.06%다. C/C++ throughput variation은 7.63%/14.81%, 19.87%/41.46%, 23.04%/23.45%, 21.70%/69.95%, 20.92%/16.44%, 36.09%/76.41%다. 모든 size에서 C와 C++ report가 complete이며 ratio와 six-size 중앙값으로 판정한다. 전체 대상은 `측정값으로 판정`이며 binding-only source optimization은 no-go다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_043500_multi-pubsub-tcp-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_043636_multi-pubsub-tcp-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_043815_multi-pubsub-tcp-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_043952_multi-pubsub-tcp-policy-c1-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_044127_multi-pubsub-tcp-policy-c1-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_044302_multi-pubsub-tcp-policy-c1-131072-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_043548_multi-pubsub-tcp-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_043726_multi-pubsub-tcp-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_043903_multi-pubsub-tcp-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_044040_multi-pubsub-tcp-policy-c1-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_044213_multi-pubsub-tcp-policy-c1-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_044349_multi-pubsub-tcp-policy-c1-131072-cpp.txt` |
| `tcp` | `MULTI_STREAM` | 미달 (61.55%) | 통과 (107.40%) | 통과 (126.61%) | 해당 없음 | 통과 (144.53%) | 해당 없음 | C/C++ raw throughput Kops/s는 64B `251.990, 249.647, 242.610, 226.814, 202.539` / `157.887, 160.262, 149.323, 140.458, 130.702`, 256B `116.751, 135.088, 161.660, 188.329, 211.542` / `178.930, 176.065, 173.621, 163.501, 143.667`, 1024B `115.852, 118.660, 137.509, 160.988, 183.424` / `166.931, 174.107, 184.564, 176.596, 153.609`, 65536B `20.942, 18.942, 15.489, 17.149, 19.583` / `33.959, 34.601, 27.376, 26.308, 24.471`이다. C/C++ 중앙값은 각각 `242.610/149.323`, `161.660/173.621`, `137.509/174.107`, `18.942/27.376` Kops/s이고 진단 ratio는 `61.55%`, `107.40%`, `126.61%`, `144.53%`다. C/C++ variation은 `20.38%/19.80%`, `58.64%/20.31%`, `49.14%/17.78%`, `28.79%/37.00%`로 네 size 모두 변동 폭을 참고 정보로 기록했다. 공식 ratio와 size 중앙값은 측정값으로 판정하며 binding-only source optimization은 no-go다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_050327_multi-stream-tcp-policy-c3-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_050536_multi-stream-tcp-policy-c3-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_050754_multi-stream-tcp-policy-c3-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_050959_multi-stream-tcp-policy-c3-65536-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_050429_multi-stream-tcp-policy-c3-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_050640_multi-stream-tcp-policy-c3-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_050857_multi-stream-tcp-policy-c3-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_051238_multi-stream-tcp-policy-c3-65536-cpp.txt`; Sol review: `SOL-MULTI-STREAM-TCP-FINAL-20260810` |
| `ws` | `MULTI_DEALER_DEALER` | 통과 (91.72%) | 통과 (121.32%) | 미달 (77.88%) | 통과 (159.28%) | 미달 (77.86%) | 통과 (186.15%) | complete report의 측정값으로 C++/C throughput ratio는 91.72%/121.32%/77.88%/159.28%/77.86%/186.15%다. 1024B·65536B는 개별 최소 85%에 미달하고, 변동 폭은 참고 정보로만 기록한다. |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 통과 (99.99%) | 통과 (91.43%) | 통과 (97.09%, runs=1) | 통과 (121.03%, runs=1) | 통과 (101.66%, runs=1) | 통과 (99.55%, runs=1) | C/C++ throughput Kops/s는 157.412/157.401, 86.960/79.507, 159.008/154.386, 114.467/138.535, 31.646/32.172, 16.147/16.074다. C++/C ratio는 99.99%, 91.43%, 97.09%, 121.03%, 101.66%, 99.55%이고 six-size median ratio는 99.77%다. 평균 latency ratio는 1.000배, 1.077배, 1.024배, 0.836배, 0.995배, 1.013배다. 64B·256B는 5회 대표값 median, 나머지는 perf 정책의 기본 `runs=1` 측정값이다. 반복값의 변동 폭은 기록하지만 판정을 막지 않는다. 모든 report가 `status: complete`이며 Core v0.10.1 release, Release, WS, 100 clients, server/client I/O threads 4, auto-HWM balanced, connect concurrency 128, connect-ready 10000ms, monitor HWM 4096000, 5초 duration 조건이다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_065005_multi-dealer-router-sendsend-ws-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_065136_multi-dealer-router-sendsend-ws-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_065706_multi-dealer-router-sendsend-ws-explore-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_065736_multi-dealer-router-sendsend-ws-explore-c1-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_070104_multi-dealer-router-sendsend-ws-explore-c1-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_070127_multi-dealer-router-sendsend-ws-explore-c1-131072-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_065050_multi-dealer-router-sendsend-ws-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_065221_multi-dealer-router-sendsend-ws-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_065719_multi-dealer-router-sendsend-ws-explore-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_070051_multi-dealer-router-sendsend-ws-explore-c1-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_070114_multi-dealer-router-sendsend-ws-explore-c1-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_070136_multi-dealer-router-sendsend-ws-explore-c1-131072-cpp.txt` |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 통과 (87.08%) | 통과 (90.26%) | 통과 (94.28%) | 통과 (76.11%) | 통과 (77.87%) | 통과 (84.78%) | C++/C throughput ratio는 87.08%/90.26%/94.28%/76.11%/77.87%/84.78%, 중앙값 85.93%다. 평균 latency ratio는 1.124x/1.092x/1.044x/1.285x/1.219x/1.461x로 기준 이내다. 모든 report가 `status: complete`이며 socket request/reply 기준으로 통과한다. |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 통과 (89.92%, runs=1) | 통과 (114.02%, runs=1) | 통과 (99.79%, runs=1) | 통과 (97.37%, runs=1) | 통과 (94.35%, runs=1) | 통과 (96.04%, runs=1) | C/C++ throughput Kops/s는 170.345/153.173, 134.573/153.436, 139.813/139.518, 122.011/118.798, 31.371/29.599, 16.209/15.568다. C++/C ratio는 89.92%, 114.02%, 99.79%, 97.37%, 94.35%, 96.04%이고 six-size median ratio는 96.71%다. 평균 latency ratio는 1.097배, 0.882배, 0.994배, 1.021배, 1.065배, 1.052배다. 모든 report가 `status: complete`이며 Core v0.10.1 release, Release, WS, 100 clients, server/client I/O threads 4, auto-HWM balanced, connect concurrency 128, connect-ready 10000ms, monitor HWM 4096000, 5초 duration, `runs=1` 조건이다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_071016_multi-router-router-sendsend-ws-explore-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_071045_multi-router-router-sendsend-ws-explore-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_071123_multi-router-router-sendsend-ws-explore-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_071150_multi-router-router-sendsend-ws-explore-c1-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_071217_multi-router-router-sendsend-ws-explore-c1-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_071244_multi-router-router-sendsend-ws-explore-c1-131072-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_071026_multi-router-router-sendsend-ws-explore-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_071104_multi-router-router-sendsend-ws-explore-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_071133_multi-router-router-sendsend-ws-explore-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_071201_multi-router-router-sendsend-ws-explore-c1-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_071227_multi-router-router-sendsend-ws-explore-c1-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_071255_multi-router-router-sendsend-ws-explore-c1-131072-cpp.txt` |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 통과 (99.34%) | 통과 (107.52%) | 통과 (98.21%) | 통과 (97.91%) | 통과 (98.17%) | 통과 (97.49%) | pool bypass A/B 후 C++/C throughput ratio는 99.34%/107.52%/98.21%/97.91%/98.17%/97.49%, 중앙값 98.19%다. 평균 latency ratio는 0.995x/0.929x/1.007x/0.998x/0.962x/0.942x다. 모든 report가 `status: complete`이며 측정값으로 통과한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_083025_multi-router-router-reqrep-ws-pool-bypass-ab-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_083105_multi-router-router-reqrep-ws-pool-bypass-ab-c1.txt` |
| `ws` | `MULTI_PUBSUB` | 통과 (106.02%) | 통과 (96.10%) | 미달 (92.64%, latency 4.132x) | 미달 (103.72%, latency 5.746x) | 미달 (107.99%, latency 13.728x) | 미달 (103.86%, latency 12.689x) | throughput ratio는 106.02%/96.10%/92.64%/103.72%/107.99%/103.86%, 중앙값 103.79%다. 64B·256B 평균 latency ratio는 1.385x/1.776x로 기준 이내지만 1024B 이상은 측정 latency 기준을 초과한다. throughput은 pool bypass 후에도 통과했고 평균 latency 기준으로 미달 판정한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_083223_multi-pubsub-ws-pool-bypass-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_083818_multi-pubsub-ws-pool-bypass-c1.txt` |
| `ws` | `MULTI_STREAM` | 미달 (78.48%) | 통과 (102.55%) | 통과 (150.72%) | 해당 없음 | 통과 (163.32%) | 해당 없음 | C/C++ raw throughput Kops/s는 64B `207.807, 198.392, 159.796, 171.477, 173.458` / `152.719, 136.132, 138.454, 116.979, 112.569`, 256B `102.953, 120.334, 139.512, 154.285, 162.895` / `143.067, 143.454, 151.249, 137.611, 124.165`, 1024B `101.675, 96.825, 93.546, 83.317, 112.608` / `128.412, 136.557, 156.534, 148.505, 145.932`, 65536B `3.612, 3.523, 4.716, 3.953, 3.539` / `3.162, 3.967, 5.899, 6.311, 6.058`이다. C/C++ 중앙값은 `173.458/136.132`, `139.512/143.067`, `96.825/145.932`, `3.612/5.899` Kops/s이고 진단 ratio는 `78.48%`, `102.55%`, `150.72%`, `163.32%`다. C/C++ variation은 `27.68%/29.49%`, `42.97%/18.93%`, `30.25%/19.27%`, `33.03%/53.38%`로 네 size 모두 변동 폭을 참고 정보로 기록했다. 공식 ratio와 size 중앙값은 측정값으로 판정하며 binding-only source optimization은 no-go다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_052020_multi-stream-ws-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_052224_multi-stream-ws-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_052422_multi-stream-ws-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_052625_multi-stream-ws-policy-c1-65536-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_052118_multi-stream-ws-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_052323_multi-stream-ws-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_052525_multi-stream-ws-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_052734_multi-stream-ws-policy-c1-65536-cpp.txt`; Sol review: `SOL-MULTI-STREAM-WS-FINAL-20260810` |
| `wss` | `MULTI_DEALER_DEALER` | 통과 (87.73%) | 통과 (92.34%) | 통과 (98.07%) | 통과 (86.27%) | 미달 (79.67%) | 통과 (104.59%) | C/C++ throughput Kmsg/s는 2763.621/2424.621, 1332.145/1230.146, 712.370/698.618, 246.882/212.980, 28.178/22.449, 11.689/12.225이다. C++/C ratio는 87.73%, 92.34%, 98.07%, 86.27%, 79.67%, 104.59%, 중앙값은 90.04%다. 평균 latency ms는 C/C++ 1.019/0.250, 1.159/0.587, 26.090/26.414, 77.223/89.926, 804.529/869.646, 1344.131/1324.228이고 latency ratio는 0.245x, 0.506x, 1.012x, 1.164x, 1.081x, 0.985x다. throughput 65536B와 중앙값 목표는 미달이며 latency는 모든 size에서 기준 이내다. C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_085805_multi-dealer-dealer-wss-explore-c1.txt`; C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_085856_multi-dealer-dealer-wss-explore-c1.txt` |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 통과 (96.35%) | 통과 (89.93%) | 통과 (81.42%) | 통과 (81.36%) | 통과 (83.48%) | 통과 (93.15%) | C/C++ throughput Kops/s는 142.107/136.922, 136.042/122.341, 127.542/103.849, 94.889/77.203, 12.575/10.498, 6.194/5.770이다. C++/C ratio는 96.35%, 89.93%, 81.42%, 81.36%, 83.48%, 93.15%, 중앙값은 86.70%다. 평균 latency ms는 C/C++ 0.337/0.350, 0.352/0.392, 0.375/0.461, 0.509/0.624, 3.954/4.732, 8.027/8.616이고 latency ratio는 1.039x, 1.114x, 1.229x, 1.226x, 1.197x, 1.073x다. 모든 throughput cell은 80% 이상이고 중앙값은 85% 이상이며 latency도 기준 이내다. C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_091057_multi-dealer-router-sendsend-wss-explore-c1.txt`; C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_091141_multi-dealer-router-sendsend-wss-explore-c1.txt` |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 통과 (94.06%) | 통과 (80.49%) | 통과 (80.49%) | 통과 (83.56%) | 통과 (78.79%) | 통과 (92.71%) | C/C++ throughput Kops/s는 89665.4/84340.4, 85262.4/68628.8, 79559.2/64039.6, 64892.0/54220.8, 11780.0/9281.6, 5722.4/5305.0이다. C++/C ratio는 94.06%, 80.49%, 80.49%, 83.56%, 78.79%, 92.71%, 중앙값은 82.02%다. 평균 latency ms는 C/C++ 0.450/0.469, 0.468/0.564, 0.493/0.597, 0.644/0.751, 4.044/5.136, 8.547/9.215이고 latency ratio는 1.042x, 1.205x, 1.211x, 1.166x, 1.270x, 1.078x다. 개별 throughput 최소 75%와 latency 기준은 충족하지만 socket request/reply 중앙값 목표 85%는 미달이다. public contract를 유지한 추가 개선 후보가 없어 최종 상태는 `보류`다. C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_091611_multi-dealer-router-reqrep-wss-explore-c2.txt`; C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_091656_multi-dealer-router-reqrep-wss-explore-c2.txt` |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 통과 (104.09%) | 통과 (101.35%) | 통과 (110.36%) | 통과 (100.40%) | 통과 (111.91%) | 통과 (123.69%) | C/C++ throughput Kops/s는 136.292/141.861, 117.405/118.992, 112.902/124.602, 88.890/89.242, 9.807/10.975, 4.800/5.937이다. C++/C ratio는 104.09%, 101.35%, 110.36%, 100.40%, 111.91%, 123.69%, 중앙값은 107.22%다. 평균 latency ratio는 0.960x, 0.985x, 0.904x, 0.991x, 0.899x, 0.809x로 기준 이내다. 두 report 모두 `status: complete`다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_093536_multi-router-router-sendsend-wss-after-contract-audit-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_093656_multi-router-router-sendsend-wss-after-contract-audit-c1.txt` |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 통과 (93.26%) | 통과 (90.33%) | 통과 (86.67%) | 통과 (80.87%) | 통과 (80.80%) | 통과 (91.49%) | C/C++ throughput Kops/s는 91314.0/85156.4, 87729.8/79244.0, 81789.2/70884.6, 63446.6/51308.8, 11672.6/9431.4, 6192.2/5665.2다. 평균 latency ms는 0.438/0.463, 0.453/0.488, 0.479/0.541, 0.658/0.785, 4.099/5.011, 7.919/8.631이고 latency ratio는 1.057x/1.077x/1.129x/1.193x/1.222x/1.090x다. C++/C 중앙값 ratio는 86.67%이며 두 report 모두 `status: complete`다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_100938_multi-router-router-reqrep-wss-short-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_101029_multi-router-router-reqrep-wss-short-c1.txt` |
| `wss` | `MULTI_PUBSUB` | 통과 (94.04%) | 통과 (90.30%) | 통과 (96.42%) | 통과 (97.56%) | 통과 (91.99%, latency 3.309x) | 통과 (96.61%, latency 4.802x) | C/C++ throughput Kmsg/s는 1506766.4/1417018.4, 1584536.4/1430860.8, 986063.6/950737.6, 319759.2/311967.0, 30390.4/27955.4, 13332.8/12880.8다. C++/C throughput 중앙값은 95.23%다. 평균 latency ms는 1827.422/1843.128, 1389.193/1313.059, 622.982/642.891, 215.345/292.563, 250.967/830.383, 306.182/1470.307이고 ratio는 1.009x/0.945x/1.032x/1.359x/3.309x/4.802x다. throughput은 단순 one-way 최소 85%와 중앙값 95%를 충족하지만 65536B·131072B latency 2.0x 기준은 미달한다. C/C++ report 모두 `status: complete`; C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_101741_multi-pubsub-wss-short-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_102043_multi-pubsub-wss-harness-parity-c2.txt` |
| `wss` | `MULTI_STREAM` | 통과 (106.75%) | 통과 (95.91%) | 통과 (107.44%) | 해당 없음 | 통과 (118.32%) | 해당 없음 | C/C++ raw throughput Kops/s는 64B `128.928, 109.147, 84.353, 86.632, 69.727` / `94.961, 110.992, 92.478, 89.576, 83.148`, 256B `86.173, 107.883, 95.472, 89.697, 65.029` / `80.546, 97.478, 97.658, 86.024, 80.639`, 1024B `69.917, 95.782, 88.933, 69.469, 71.630` / `76.546, 92.795, 89.871, 76.960, 69.416`, 65536B `2.309, 2.822, 3.783, 2.567, 2.861` / `2.858, 4.392, 3.339, 3.421, 3.077`이다. C/C++ 중앙값은 `86.632/92.478`, `89.697/86.024`, `71.630/76.960`, `2.822/3.339` Kops/s이고 진단 ratio는 `106.75%`, `95.91%`, `107.44%`, `118.32%`다. C/C++ variation은 `68.34%/30.11%`, `47.78%/19.89%`, `36.73%/30.38%`, `52.23%/45.94%`로 네 size 모두 변동 폭을 참고 정보로 기록했다. 공식 ratio와 size 중앙값은 측정값으로 판정하며 binding-only source optimization은 no-go다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_053203_multi-stream-wss-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_053649_multi-stream-wss-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_054139_multi-stream-wss-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_054632_multi-stream-wss-policy-c1-65536-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_053423_multi-stream-wss-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_053913_multi-stream-wss-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_054405_multi-stream-wss-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_054922_multi-stream-wss-policy-c1-65536-cpp.txt`; Sol review: `SOL-MULTI-STREAM-WSS-FINAL-20260810` |
| `tls` | `MULTI_DEALER_DEALER` | 통과 (95.04%) | 통과 (96.77%) | 통과 (100.63%) | 통과 (99.76%) | 통과 (93.72%) | 통과 (99.12%) | aggregate throughput 중앙값 97.95%, aggregate latency 중앙값 0.978x, 통과. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_112546_multi-dealer-dealer-tls-short-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_113100_multi-dealer-dealer-tls-short-c2.txt` |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 통과 (101.32%) | 통과 (92.43%) | 통과 (79.79%) | 통과 (79.13%) | 통과 (87.49%) | 통과 (105.25%) | aggregate throughput 중앙값 89.96%, aggregate latency 중앙값 1.114x, 통과. 1024B·4096B throughput은 개별 outlier로 기록한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_113648_multi-dealer-router-sendsend-tls-short-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_113736_multi-dealer-router-sendsend-tls-short-c2.txt` |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 통과 (84.54%) | 통과 (96.40%) | 통과 (94.67%) | 통과 (96.32%) | 통과 (110.52%) | 통과 (86.15%) | aggregate throughput 중앙값 95.50%, aggregate latency 중앙값 1.030x, 통과. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_114047_multi-dealer-router-reqrep-tls-short-c1.txt`; 최종 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_114927_multi-dealer-router-reqrep-tls-size-ctor-c3.txt` |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 통과 (100.82%) | 통과 (99.01%) | 통과 (101.04%) | 통과 (101.62%) | 통과 (103.58%) | 통과 (125.09%) | C/C++ throughput Kops/s는 `155.298/156.566`, `153.647/152.122`, `144.560/146.061`, `120.482/122.428`, `16.708/17.306`, `7.570/9.470`이다. C++/C ratio는 `100.82% / 99.01% / 101.04% / 101.62% / 103.58% / 125.09%`, 중앙값은 `101.33%`다. 평균 latency ratio는 `0.987x / 1.010x / 0.869x / 0.980x / 0.965x / 0.801x`, aggregate 중앙값은 `0.972x`다. 두 report 모두 `status: complete`이며 추가 source 변경 없이 통과 판정했다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_115341_multi-router-router-sendsend-tls-short-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_115430_multi-router-router-sendsend-tls-short-c2.txt` |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 통과 (111.07%) | 통과 (96.37%) | 통과 (82.53%) | 통과 (93.72%) | 통과 (99.54%) | 통과 (116.25%) | baseline 중앙값 82.98%에서 copy constructor `_storage()` zero-init 제거 후 aggregate throughput 중앙값 97.95%, aggregate latency 중앙값 1.008x로 개선·통과. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_120243_multi-router-router-reqrep-tls-short-c1.txt`; baseline C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_120331_multi-router-router-reqrep-tls-short-c2.txt`; final C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_120837_multi-router-router-reqrep-tls-copy-ctor-c3.txt` |
| `tls` | `MULTI_PUBSUB` | 통과 (98.67%) | 통과 (88.45%) | 통과 (95.73%) | 통과 (99.44%) | 통과 (99.72%) | 통과 (96.75%) | aggregate throughput 중앙값 97.71%, aggregate latency 중앙값 1.052x, 통과. 256B throughput과 65536B·131072B latency는 개별 outlier로 기록한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_111320_multi-pubsub-tls-storage-default-full-c12.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_111359_multi-pubsub-tls-storage-default-full-c12.txt` |
| `tls` | `MULTI_STREAM` | 미달 (83.99%) | 미달 (78.29%) | 통과 (102.04%) | 해당 없음 | 통과 (111.10%) | 해당 없음 | C/C++ raw throughput Kops/s는 64B `145.966, 122.985, 111.650, 99.883, 79.807` / `89.429, 125.331, 82.387, 100.383, 93.776`, 256B `75.863, 113.987, 121.684, 114.323, 99.137` / `75.907, 89.246, 110.007, 119.302, 80.515`, 1024B `93.708, 82.341, 79.116, 99.676, 108.116` / `128.989, 116.063, 95.621, 91.701, 72.109`, 65536B `6.418, 8.782, 7.058, 4.569, 6.845` / `5.441, 9.159, 9.112, 7.605, 4.240`이다. C/C++ 중앙값은 `111.650/93.776`, `113.987/89.246`, `93.708/95.621`, `6.845/7.605` Kops/s이고 진단 ratio는 `83.99%`, `78.29%`, `102.04%`, `111.10%`다. C/C++ variation은 `59.27%/45.79%`, `40.20%/48.63%`, `30.95%/59.48%`, `61.55%/64.68%`로 네 size 모두 측정값을 기록했다. 공식 ratio와 size 중앙값은 측정값으로 판정하며 binding-only source optimization은 no-go다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_055446_multi-stream-tls-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_055920_multi-stream-tls-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_060358_multi-stream-tls-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_061033_multi-stream-tls-policy-c1-65536-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_055701_multi-stream-tls-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_060138_multi-stream-tls-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_060819_multi-stream-tls-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_061300_multi-stream-tls-policy-c1-65536-cpp.txt`; Sol review: `SOL-MULTI-STREAM-TLS-FINAL-20260810` |

### 9.1.3 Multi 최종 재측정 정정

기존 `MULTI_PUBSUB / tls` 행은 직접 수신 경로 적용 전의 초기 paired 결과다. 최종 판정은
아래 재측정 행을 사용한다. size별 값은 기록하고 throughput·latency aggregate 중앙값으로
transport 상태를 판정한다.

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 최종 결과 |
|-----------|---------|----|-----|------|------|-------|--------|-----------|
| `tls` | `MULTI_PUBSUB` | 통과 (98.67%) | 통과 (88.45%) | 통과 (95.73%) | 통과 (99.44%) | 통과 (99.72%) | 통과 (96.75%) | aggregate throughput 중앙값 97.71%, aggregate latency 중앙값 1.052x, 통과. 256B throughput과 65536B·131072B latency는 개별 outlier로 기록한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_111320_multi-pubsub-tls-storage-default-full-c12.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_111359_multi-pubsub-tls-storage-default-full-c12.txt` |

### 9.1.4 Multi DEALER_DEALER TLS 최종 측정

`MULTI_DEALER_DEALER / tls`는 C를 먼저 측정하고 C++을 단독 실행했다. 두 report가 모두
`status: complete`이고 6개 size의 결과가 있으므로 throughput과 latency aggregate 중앙값으로
판정한다.

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 최종 결과 |
|-----------|---------|----|-----|------|------|-------|--------|-----------|
| `tls` | `MULTI_DEALER_DEALER` | 통과 (95.04%) | 통과 (96.77%) | 통과 (100.63%) | 통과 (99.76%) | 통과 (93.72%) | 통과 (99.12%) | aggregate throughput 중앙값 97.95%, aggregate latency 중앙값 0.978x, 통과. 64B latency 1.143x와 65536B latency 1.041x는 개별 기록값이다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_112546_multi-dealer-dealer-tls-short-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_113100_multi-dealer-dealer-tls-short-c2.txt` |

### 9.1.5 Multi DEALER_ROUTER_SENDSEND TLS 최종 측정

`MULTI_DEALER_ROUTER_SENDSEND / tls`는 C를 먼저 측정하고 C++을 단독 실행했다. 두 report가 모두
`status: complete`이고 6개 size의 결과가 있으므로 throughput과 latency aggregate 중앙값으로
판정한다.

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 최종 결과 |
|-----------|---------|----|-----|------|------|-------|--------|-----------|
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 통과 (101.32%) | 통과 (92.43%) | 통과 (79.79%) | 통과 (79.13%) | 통과 (87.49%) | 통과 (105.25%) | aggregate throughput 중앙값 89.96%, aggregate latency 중앙값 1.114x, 통과. 1024B·4096B throughput은 개별 outlier로 기록한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_113648_multi-dealer-router-sendsend-tls-short-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_113736_multi-dealer-router-sendsend-tls-short-c2.txt` |

### 9.1.6 Multi DEALER_ROUTER_REQREP TLS 개선 재측정

초기 paired 결과는 throughput aggregate 중앙값이 84.56%로 socket request/reply의 85% 목표에
미달했다. Sol review `SOL-MULTI-DR-REQREP-TLS-20260810`에 따라 callback state 재사용과
`received_t`/reply ownership 변경은 적용하지 않고, `message_t(size_t)`의 초기 storage zero-init만
제거해 재측정했다. 최종 aggregate 기준은 후보 적용 후 결과로 판정한다.

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 최종 결과 |
|-----------|---------|----|-----|------|------|-------|--------|-----------|
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 통과 (84.54%) | 통과 (96.40%) | 통과 (94.67%) | 통과 (96.32%) | 통과 (110.52%) | 통과 (86.15%) | aggregate throughput 중앙값 95.50%, aggregate latency 중앙값 1.030x, 통과. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_114047_multi-dealer-router-reqrep-tls-short-c1.txt`; 초기 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_114137_multi-dealer-router-reqrep-tls-short-c2.txt`; 최종 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_114927_multi-dealer-router-reqrep-tls-size-ctor-c3.txt` |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 통과 (84.54%) | 통과 (96.40%) | 통과 (94.67%) | 통과 (96.32%) | 통과 (110.52%) | 통과 (86.15%) | aggregate throughput 중앙값 95.50%, aggregate latency 중앙값 1.030x, 통과. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_114047_multi-dealer-router-reqrep-tls-short-c1.txt`; 초기 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_114137_multi-dealer-router-reqrep-tls-short-c2.txt`; 최종 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_114927_multi-dealer-router-reqrep-tls-size-ctor-c3.txt` |

### 9.1.7 Multi ROUTER_ROUTER_SENDSEND TLS 최종 측정

`MULTI_ROUTER_ROUTER_SENDSEND / tls`는 C를 먼저 측정하고 C++을 단독 실행했다. 두 report가 모두
`status: complete`이고 6개 size의 결과가 있으므로 throughput과 latency aggregate 중앙값으로
판정한다. 기준을 통과했으므로 추가 binding source 변경은 적용하지 않는다.

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 최종 결과 |
|-----------|---------|----|-----|------|------|-------|--------|-----------|
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 통과 (100.82%) | 통과 (99.01%) | 통과 (101.04%) | 통과 (101.62%) | 통과 (103.58%) | 통과 (125.09%) | aggregate throughput 중앙값 101.33%, aggregate latency 중앙값 0.972x, 통과. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_115341_multi-router-router-sendsend-tls-short-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_115430_multi-router-router-sendsend-tls-short-c2.txt` |

### 9.1.8 Multi ROUTER_ROUTER_REQREP TLS 개선 재측정

초기 paired 결과는 throughput aggregate 중앙값 `82.98%`로 socket request/reply의 중앙값 목표
`85%`에 미달했다. Sol review `SOL-MULTI-RR-REQREP-TLS-20260810`에 따라 이미 적용된
`message_t(size_t)` 후보는 유지하고, 서버 retry template이 사용하는 copy constructor의
`_storage()` zero-init만 제거해 재측정했다. public interface와 message ownership은 변경하지 않았다.

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 최종 결과 |
|-----------|---------|----|-----|------|------|-------|--------|-----------|
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 통과 (111.07%) | 통과 (96.37%) | 통과 (82.53%) | 통과 (93.72%) | 통과 (99.54%) | 통과 (116.25%) | baseline throughput 중앙값 82.98%에서 최종 aggregate throughput 중앙값 97.95%, aggregate latency 중앙값 1.008x로 개선·통과. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_120243_multi-router-router-reqrep-tls-short-c1.txt`; baseline C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_120331_multi-router-router-reqrep-tls-short-c2.txt`; 최종 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_120837_multi-router-router-reqrep-tls-copy-ctor-c3.txt` |

### 9.2 .NET

- perf 경로: `bindings/dotnet/perf`
- Single 상태: `DEALER_ROUTER/tcp`·`DEALER_ROUTER_REQREP/tcp`·`ROUTER_ROUTER_REQREP/tcp`·`PAIR/ws`·`DEALER_DEALER/ws`·`DEALER_ROUTER/ws`·`ROUTER_ROUTER/ws`·`DEALER_ROUTER_REQREP/ws`·`ROUTER_ROUTER_REQREP/ws`·`PAIR/wss`·`PUBSUB/wss`·`DEALER_DEALER/wss`·`DEALER_ROUTER/wss`·`DEALER_ROUTER_REQREP/wss`·`ROUTER_ROUTER/wss`·`ROUTER_ROUTER_REQREP/wss` 완료·통과, `PAIR/tcp`·`PUBSUB/tcp`·`DEALER_DEALER/tcp`·`ROUTER_ROUTER/tcp`·`PUBSUB/ws`·`PAIR/tls`·`PUBSUB/tls`·`DEALER_DEALER/tls`·`DEALER_ROUTER/tls` 완료·보류, 나머지 대상 측정 중
- Multi 상태: `미측정`
- `미측정` 행은 완료나 다음 언어 전환을 의미하지 않는다. 특히 9.2.2의 `tls` `MULTI_*`
  행은 .NET에서 아직 측정하지 않은 대상이며, C++ 9.1의 같은 이름 행이 완료되어도 .NET
  측정 완료로 간주하지 않는다.
- 다음 작업: `Single DEALER_ROUTER_REQREP / tls`를 C → .NET 순서로 한 대상씩 측정한다.

#### 9.2.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 77.79% | 71.57% | 83.88% | 87.47% | 94.92% | 89.86% | 보류·자체 pass `82.03% → 84.25%`, 평균 latency ratio `0.977x`. 자체 후보는 `flags == None` 분기이며 Sol pass는 추가 후보 no-go. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_133934_dotnet-pair-tcp-c-full.txt`; 자체 before: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_143203_dotnet-pair-tcp-own-before-pool.txt`; after: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_142021_dotnet-pair-tcp-parity-baseline.txt`; Sol no-go: 추가 builder/direct-send/ownership 변경 필요 |
| `tcp` | `PUBSUB` | 62.30% | 70.72% | 89.93% | 98.19% | 86.46% | 91.91% | 보류·자체 pass `80.35% → 83.25%`, 평균 latency ratio `1.130x`. 자체 후보는 `Publish(flags == None)` 분기이며 Sol pass는 추가 message/builder 재사용·private direct 경로 no-go. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_135104_dotnet-pubsub-tcp-c1.txt`; 자체 before: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_142030_dotnet-pubsub-tcp-parity-baseline.txt`; after: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_142943_dotnet-pubsub-tcp-own-after-pool.txt` |
| `tcp` | `DEALER_DEALER` | 46.71% | 68.90% | 87.55% | 82.23% | 91.14% | 92.12% | 보류·자체 pass `76.69% → 78.11%`, 평균 latency ratio `8.142x`. 자체 후보는 `flags == None` 분기이며 Sol pass는 추가 builder pool/private direct/ownership 변경 no-go. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_135400_dotnet-dealer-dealer-tcp-c1.txt`; 자체 before: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_143235_dotnet-dealer-dealer-tcp-own-before-pool.txt`; after: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_142040_dotnet-dealer-dealer-tcp-parity-baseline.txt` |
| `tcp` | `DEALER_ROUTER` | 49.87% | 87.08% | 94.87% | 90.23% | 92.32% | 89.17% | 통과·throughput 산술평균 83.92%, 평균 latency ratio 1.059x. 64B 개별 ratio는 기록값이며 aggregate gate를 바꾸지 않는다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_135529_dotnet-dealer-router-tcp-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_141217_dotnet-dealer-router-tcp-baseline.txt` |
| `tcp` | `DEALER_ROUTER_REQREP` | 67.99% | 68.49% | 73.79% | 111.35% | 108.83% | 109.94% | 통과·throughput 산술평균 90.06%, 평균 latency ratio 1.012x. Router monitor wait를 C의 activity-driven setup과 맞춘 parity correction 후 측정했다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_211628_dealer-router-reqrep-tcp-policy-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_143942_dotnet-dealer-router-reqrep-tcp-paired-v2.txt` |
| `tcp` | `ROUTER_ROUTER` | 41.74% | 73.54% | 92.63% | 89.35% | 93.05% | 89.63% | 보류·동일 조건 paired throughput 산술평균 79.99%, 평균 latency ratio 52.546x. 자체 routed `flags == None` pass는 진단값에서 throughput 99.17%→100.26%, latency 43.945x→40.405x였고, Sol pass는 추가 builder/message pool·private direct·queue 조정 no-go. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_144914_router-router-tcp-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_144931_dotnet-router-router-tcp-paired-final.txt` |
| `tcp` | `ROUTER_ROUTER_REQREP` | 61.81% | 63.07% | 58.03% | 96.05% | 98.61% | 98.70% | 통과·throughput 산술평균 79.38%, 평균 latency ratio 1.119x. C activity-driven readiness와 동일한 setup gate를 적용했다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_145246_router-router-reqrep-tcp-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_145502_dotnet-router-router-reqrep-tcp-paired-final.txt` |
| `ws` | `PAIR` | 78.19% | 73.99% | 97.32% | 94.92% | 94.07% | 97.43% | 통과·throughput 산술평균 89.32%, 평균 latency ratio 1.133x. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_145939_dotnet-pair-ws-c-paired.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_145953_dotnet-pair-ws-paired-final.txt` |
| `ws` | `PUBSUB` | 68.29% | 53.13% | 94.54% | 90.84% | 95.12% | 98.40% | 보류·자체 1차 개선 후 throughput 산술평균 83.39%, 평균 latency ratio 1.090x. 1차 후보는 내부 `PublishMessageUnchecked` 경로이며 baseline 80.39%에서 개선됐다. Sol 2차 lock coalesce 후보는 78.40%·1.193x로 악화되어 제거했다. 추가 contract-safe 후보 no-go. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_150225_dotnet-pubsub-ws-c-paired.txt`; baseline: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_150246_dotnet-pubsub-ws-paired-final.txt`; 1차 after: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_150638_dotnet-pubsub-ws-own-after-unchecked-publish.txt`; Sol 2차 after: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_151007_dotnet-pubsub-ws-sol-after-lock-coalesce.txt` |
| `ws` | `DEALER_DEALER` | 70.80% | 62.08% | 94.44% | 91.10% | 92.77% | 98.79% | 통과·throughput 산술평균 85.00%, 평균 latency ratio 1.013x. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_151412_dotnet-dealer-dealer-ws-c-paired.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_151426_dotnet-dealer-dealer-ws-paired-final.txt` |
| `ws` | `DEALER_ROUTER` | 71.91% | 69.49% | 96.25% | 90.82% | 89.77% | 91.86% | 통과·throughput 산술평균 85.02%, 평균 latency ratio 1.150x. 64B·256B 개별 ratio는 기록값이며 .NET routed one-way aggregate 기준을 바꾸지 않는다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_151702_dotnet-dealer-router-ws-c-paired.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_151909_dotnet-dealer-router-ws-paired-final.txt` |
| `ws` | `DEALER_ROUTER_REQREP` | 62.21% | 70.53% | 89.13% | 94.98% | 96.37% | 101.78% | 통과·자체 parity 수정 후 throughput 산술평균 85.83%, 평균 latency ratio 1.025x. 수정 전 평균은 83.64%·442.376x였고, Sol 2차 후보는 no-go다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_152626_dealer-router-reqrep-ws-paired-c1.txt`; .NET before: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_152639_dotnet-dealer-router-reqrep-ws-paired-final.txt`; .NET after: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_153119_dotnet-dealer-router-reqrep-ws-own-after-c-parity.txt` |
| `ws` | `ROUTER_ROUTER` | 55.04% | 63.47% | 99.73% | 92.43% | 94.11% | 95.69% | 통과·throughput 산술평균 83.41%, 평균 latency ratio 1.143x. 64B·256B 개별 ratio는 기록값이며 .NET routed one-way aggregate 기준을 바꾸지 않는다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_152401_router-router-ws-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_152419_dotnet-router-router-ws-paired-final.txt` |
| `ws` | `ROUTER_ROUTER_REQREP` | 65.12% | 65.30% | 89.65% | 95.19% | 98.66% | 98.86% | 통과·throughput 산술평균 85.46%, 평균 latency ratio 1.021x. 64B·256B 개별 ratio는 기록값이며 .NET socket request/reply aggregate 기준을 바꾸지 않는다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_153631_router-router-reqrep-ws-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_153727_dotnet-router-router-reqrep-ws-own-after-c-parity.txt` |
| `wss` | `PAIR` | 79.59% | 88.32% | 95.92% | 92.97% | 97.37% | 96.16% | 통과·throughput 산술평균 91.72%, 평균 latency ratio 1.048x. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_153917_pair-wss-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_153929_dotnet-pair-wss-paired-final.txt` |
| `wss` | `PUBSUB` | 77.16% | 80.73% | 93.41% | 94.47% | 99.55% | 96.34% | 통과·throughput 산술평균 90.28%, 평균 latency ratio 1.091x. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_154100_pubsub-wss-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_154120_dotnet-pubsub-wss-paired-final.txt` |
| `wss` | `DEALER_DEALER` | 69.59% | 83.52% | 97.04% | 93.95% | 95.11% | 91.45% | 통과·throughput 산술평균 88.44%, 평균 latency ratio 1.146x. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_154250_dealer-dealer-wss-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_154304_dotnet-dealer-dealer-wss-paired-final.txt` |
| `wss` | `DEALER_ROUTER` | 70.60% | 84.93% | 97.86% | 93.69% | 95.87% | 92.32% | 통과·throughput 산술평균 89.21%, 평균 latency ratio 1.018x. 64B·256B 개별 ratio는 기록값이며 .NET routed one-way aggregate 기준을 바꾸지 않는다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_154433_dealer-router-wss-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_154448_dotnet-dealer-router-wss-paired-final.txt` |
| `wss` | `DEALER_ROUTER_REQREP` | 61.55% | 73.74% | 83.05% | 93.50% | 88.95% | 97.12% | 통과·throughput 산술평균 82.98%, 평균 latency ratio 1.104x. 64B·256B 개별 ratio는 기록값이며 .NET socket request/reply aggregate 기준을 바꾸지 않는다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155235_dealer-router-reqrep-wss-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_155250_dealer-router-reqrep-wss-paired-final.txt` |
| `wss` | `ROUTER_ROUTER` | 59.94% | 87.13% | 100.10% | 96.97% | 94.94% | 89.23% | 통과·throughput 산술평균 88.05%, 평균 latency ratio 0.943x. 64B 개별 ratio는 기록값이며 .NET routed one-way aggregate 기준을 바꾸지 않는다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155438_router-router-wss-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_155452_router-router-wss-paired-final.txt` |
| `wss` | `ROUTER_ROUTER_REQREP` | 57.49% | 68.06% | 87.72% | 94.43% | 99.75% | 90.66% | 통과·throughput 산술평균 83.02%, 평균 latency ratio 1.095x. 64B·256B 개별 ratio는 기록값이며 .NET socket request/reply aggregate 기준을 바꾸지 않는다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155632_router-router-reqrep-wss-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_155645_router-router-reqrep-wss-paired-final.txt` |
| `tls` | `PAIR` | 76.07% | 82.35% | 107.48% | 87.68% | 90.46% | 87.11% | 보류·자체 `EpochNs()` clock 개선 후 throughput 산술평균 88.53%, 평균 latency ratio 19.979x. 자체 before throughput ratio는 85.58%/77.74%/101.39%/90.10%/90.06%/88.64%, 산술평균 88.92%, latency ratio는 0.890x/134.697x/17.567x/1.103x/1.100x/1.123x, 산술평균 26.080x였다. Sol 2차 리뷰는 `MessageSocketSendOperation` 재사용과 private direct-send를 public 호출자 참조·mutable state·ownership 계약 위반 위험으로 no-go 판정했다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155818_pair-tls-paired-c1.txt`; .NET before: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_155831_pair-tls-paired-final.txt`; .NET after: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_160225_pair-tls-own-after-clock.txt` |
| `tls` | `PUBSUB` | 68.99% | 79.67% | 105.24% | 97.48% | 91.22% | 88.63% | 보류·자체 lock merge 후 throughput 산술평균 88.54%, 평균 latency ratio 12.862x. before throughput ratio는 68.33%/79.46%/108.84%/95.38%/91.87%/88.37%, 산술평균 88.71%, latency ratio는 1.109x/54.262x/19.755x/1.045x/1.085x/1.127x, 산술평균 13.064x였다. Sol 2차 리뷰는 `PublisherSendOperation` pooling·private direct path·topic validation 시점 변경을 public builder 참조·ownership·error semantics 위반 위험으로 no-go 판정했다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_161248_dotnet-pubsub-tls-paired-c1.txt`; .NET before: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_161306_dotnet-pubsub-tls-paired-before.txt`; .NET after: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_161727_dotnet-pubsub-tls-own-after-lock.txt` |
| `tls` | `DEALER_DEALER` | 61.68% | 83.30% | 111.68% | 92.20% | 91.52% | 90.59% | 보류·자체 `SendMessageUnchecked` AggressiveInlining 후 throughput 산술평균 88.49%, 평균 latency ratio 16.655x. before throughput ratio는 57.67%/81.76%/106.45%/92.32%/83.31%/70.96%, 산술평균 82.08%, latency ratio는 1.541x/92.801x/14.033x/1.081x/1.191x/1.380x, 산술평균 18.671x였다. Sol 2차 리뷰는 `MessageSocketSendOperation` pooling·singleton·private direct-send를 independent builder와 stale-reference·ownership 계약 위반 위험으로 no-go 판정했다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_162033_dotnet-dealer-dealer-tls-paired-c1.txt`; .NET before: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_162044_dotnet-dealer-dealer-tls-paired-before.txt`; .NET after: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_162153_dotnet-dealer-dealer-tls-own-after-inline.txt` |
| `tls` | `DEALER_ROUTER` | 55.42% | 81.70% | 102.54% | 92.30% | 93.56% | 85.78% | 보류·자체 `SendRoutedMessageUnchecked` AggressiveInlining 후보는 throughput 산술평균 85.22%→85.68%로 +0.46%p였지만 latency 15.893x→21.662x로 악화되어 제거했다. 최종 before throughput 산술평균 85.22%, latency ratio 1.294x/69.377x/21.401x/1.078x/1.056x/1.153x, 산술평균 15.893x. Sol 2차 리뷰는 latency 회귀를 근거로 revert하고 추가 routed builder pool/private direct/inlining 후보를 no-go 판정했다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_162440_dotnet-dealer-router-tls-paired-c1.txt`; .NET final before: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_162451_dotnet-dealer-router-tls-paired-before.txt`; 제거한 after: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_162542_dotnet-dealer-router-tls-own-after-inline.txt` |
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
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
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
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
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
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
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
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
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
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
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
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |


## 10. 전체 진행 상태

### 10.1 사전 조건

| 구분 | 상태 | 결과 파일 / 메모 |
|------|------|------------------|
| 버전 3곳 일치 | 확인 | `VERSION`, `core/CMakeLists.txt`, `core/include/zlink.h`가 모두 0.10.1이다. |
| 실제 runtime 버전 | 확인 | GitHub `core/v0.10.1` release prefix와 package provenance를 사용했다. |
| runner inventory | 선택 대상 확인 | `PAIR / inproc`, `PAIR / tcp`, `PAIR / ws`, `PAIR / wss`, `PAIR / tls`, `PAIR / ipc`, `PUBSUB / tcp`, `PUBSUB / ws`, `PUBSUB / wss`, `PUBSUB / tls`, `PUBSUB / inproc`, `DEALER_ROUTER / tcp`의 C·C++ runner 및 실제 binary mapping을 확인했다. 전체 inventory는 미완료다. |
| Multi size 정책 | 확인 | `PERF_MULTI_LATENCY_SAMPLE_CAP=4000000`을 사용한 64·256·1024·4096·65536·131072B 개별 paired 결과를 기록했다. 각 size는 별도 C/C++ report로 보존했다. |
| 무시되는 runner option | 선택 대상 확인 | Effective Options에서 pattern, transport, size, duration, runs, I/O thread, HWM, timeout을 C·C++ 모두 확인했다. |
| memory guard | 미실행 | 이번 측정 결과에는 memory guard를 포함하지 않았다. |
| 재현 환경 manifest | 부분 기록 | release provenance, Multi 측정 조건, size별 report 경로와 session tag를 측정 기록과 결과 표에 남겼다. |

### 10.2 Pattern별 paired 기준 측정

| 구분 | 상태 | 결과 파일 / 메모 |
|------|------|------------------|
| 현재 언어 | .NET |  |
| 현재 pattern | 완료·보류 | `DEALER_ROUTER / tls` 최종 throughput 산술평균은 85.22%로 기준을 충족했지만 평균 latency ratio가 15.893x로 미달했다. 자체 routed inlining 후보는 latency 회귀로 제거했고 다음 대상은 `.NET Single DEALER_ROUTER_REQREP / tls`다. |
| paired C | 완료 | C throughput은 2153255/1032204/337869/13319/7944/4487 Kmsg/s, 평균 latency는 40.947/0.321/0.604/14.883/25.267/45.121 ms다. report는 `perf_c_single_linux_20260810_162440_dotnet-dealer-router-tls-paired-c1.txt`다. |
| binding paired 결과 | 완료·미달 | .NET final before throughput은 1193408/843356/346456/12294/7432/3849 Kmsg/s, 평균 latency는 52.982/22.270/12.926/16.046/26.678/52.010 ms다. 제거한 after throughput은 1142369/857861/338651/12552/7304/4105 Kmsg/s, 평균 latency는 62.651/32.110/15.209/15.756/27.110/49.535 ms다. |
| 개선 결과 | 보류·자체 후보 제거 후 Sol no-go | `SendRoutedMessageUnchecked` AggressiveInlining 후 throughput aggregate는 85.22%에서 85.68%로 소폭 상승했지만 latency aggregate는 15.893x에서 21.662x로 악화되어 attribute를 revert했다. 최종은 before 측정값이며, routed builder pooling/private direct/inlining 추가 후보는 no-go다. |

| request/reply paired C | 완료 | `DEALER_ROUTER_REQREP / tcp` C report median: 212,245.6 / 192,437.8 / 176,009.8 / 17,174.8 / 11,981.0 / 7,339.6 msg/s. C++ 구형 실행은 공식 비교에서 제외했으며, parity 보정 후 C++을 다시 측정했다. |
| request/reply binding paired 결과 | 완료·미달 | C++ full sweep ratio: 95.10%, 93.51%, 96.72%, 94.99%, 95.76%, 93.89%, 중앙값 95.05%. 1024B·65536B boundary 재검증 ratio: 94.64%, 95.73%. 공식 재계산 중앙값 94.87%, latency ratio 모두 1.057배 이내. |
| request/reply 개선 결과 | no-go | 모든 개별 ratio는 85% 이상이지만 공식 중앙값이 95%에 미달한다. boundary 변동 폭은 C/C++ 7.38%/4.69%, 2.35%/5.09%로 기준을 충족했고 binding-only hot-path 근거가 없어 source optimization을 수행하지 않는다. |

| request/reply ws 결과 | 측정값으로 판정 | 측정값 판정 ratio는 64B 96.25%, 256B 92.11%, 65536B 97.01%, 131072B 98.97%, 262144B 94.82%다. 1024B 진단 ratio 86.03%는 C/C++ 변동 폭 20.77%/18.72%와 하향 drift는 참고 정보로 기록한다. WS transport의 pass/fail은 측정값으로 판정하며 binding-only source optimization은 no-go다. |

| request/reply ws parity boundary | 측정값으로 판정 | C++ measured-loop parity 수정 후 256B ratio 92.11%, 131072B ratio 98.97%, 262144B ratio 94.82%, 1024B ratio 86.03%를 모두 측정값으로 판정한다. 1024B도 throughput ratio와 평균 latency 기준으로 판정하며 반복값은 기록만 한다. |
| request/reply wss 결과 | 측정값으로 판정 | 256B만 측정값을 확인했으며 ratio 91.65%, C/C++ 변동 폭 2.11%/4.11%, latency ratio 1.08배다. 64B ratio 78.73%, 1024B ratio 118.49%, 65536B ratio 100.06%는 각각 C 또는 C++ 변동 폭이 10%를 넘어 측정값으로 기록한다. 131072B·262144B도 full sweep paired report가 없어 미측정으로 남겼다. WSS six-size 중앙값과 pass/fail은 측정값으로 판정하며 binding-only source optimization은 no-go다. |
| request/reply tls 결과 | 측정값으로 판정 | 256B 측정값은 ratio 93.13%, C/C++ 변동 폭 7.66%/4.20%, latency ratio 약 1.058배다. 1024B ratio 80.30%와 C/C++ 하향 drift·변동 폭 26.87%/40.03%는 측정값과 함께 참고 정보로 기록하고, 65536B ratio 110.54%와 C variation 12.99%도 같은 방식으로 기록한다. 64B·131072B·262144B는 c1 paired 측정값을 기록하며 추가 측정 여부는 성능 기준으로 결정한다. TLS six-size 중앙값과 pass/fail은 측정값으로 판정하며 binding-only source optimization은 no-go다. |
| request/reply inproc 결과 | 측정값으로 판정 | `ROUTER_ROUTER_REQREP / inproc` full sweep 진단 ratio는 81.73%, 87.19%, 87.08%, 34.92%, 92.40%, 87.45%, 진단용 중앙값은 87.14%다. 65536B boundary C/C++ median은 111.55/39.11 Kops/s, 진단 ratio는 35.06%, variation은 5.84%/39.86%다. C++ 변동성이 변동 폭을 참고 정보로 기록하므로 공식 ratio·six-size 중앙값·pass/fail은 측정값으로 판정하며 binding-only source optimization은 중단한다. |
| request/reply ipc 결과 | 측정값으로 판정 | `ROUTER_ROUTER_REQREP / ipc` full sweep ratio는 87.77%, 89.40%, 81.98%, 90.53%, 90.03%, 99.70%, 중앙값은 89.72%다. 1024B boundary ratio 90.23%, latency ratio 1.017배와 64B boundary ratio 73.62%, latency ratio 약 1.270배를 모두 측정값으로 판정한다. six-size 중앙값과 transport pass/fail은 측정값으로 판정하며 binding-only source optimization은 중단한다. |
| `ROUTER_ROUTER / ws` 결과 | 측정값으로 판정 | full sweep diagnostic ratio는 90.15%, 84.72%, 95.14%, 91.42%, 90.23%, 96.58%, 진단 중앙값은 90.83%다. 256B boundary ratio 94.93%는 C/C++ variation 4.48%/5.56%로 측정값으로 판정한다. 64B boundary ratio 80.35%는 C variation 18.22%는 참고 정보로 기록하고 측정값으로 판정한다. six-size 중앙값과 transport pass/fail은 측정값으로 판정하며 binding-only source optimization은 중단한다. |
| `ROUTER_ROUTER / wss` 결과 | 측정값으로 판정 | full sweep diagnostic ratio는 81.80%, 86.34%, 96.54%, 98.82%, 96.17%, 92.97%, 진단 중앙값은 94.57%다. 64B boundary ratio 90.61%는 C/C++ variation 7.30%/5.97%로 측정값으로 판정한다. 256B boundary ratio 83.44%는 C/C++ variation 10.30%/21.26%로 측정값으로 기록한다. six-size 중앙값과 transport pass/fail은 측정값으로 판정하며 binding-only source optimization은 중단한다. |
| `ROUTER_ROUTER / tls` 결과 | valid performance fail | full sweep diagnostic ratio는 90.83%, 82.44%, 93.04%, 95.13%, 92.84%, 82.21%, 진단 중앙값은 91.84%다. 262144B boundary ratio 약 83.92%는 C/C++ variation 약 5.5%/9.69%, latency ratio 약 1.195배로 측정값으로 확인된 개별 85% 하한 미달을 확정한다. 추가 boundary와 binding-only source optimization은 중단한다. |
| `ROUTER_ROUTER / inproc` 결과 | valid performance fail | full sweep diagnostic ratio는 약 90.24%, 92.39%, 83.22%, 19.63%, 57.02%, 64.64%, 진단 중앙값은 약 73.93%다. 262144B boundary ratio 약 63.97%는 C/C++ variation 약 6.1%/5.7%, latency ratio 약 1.59배로 측정값으로 확인된 개별 85% 하한 미달을 확정한다. 추가 boundary와 binding-only source optimization은 중단한다. |
| `ROUTER_ROUTER / ipc` 결과 | 측정값으로 판정 | full sweep diagnostic ratio는 91.89%, 84.44%, 100.00%, 85.54%, 90.76%, 92.00%, 진단 중앙값은 91.33%다. 256B boundary ratio 94.75%는 C/C++ variation 6.8%/5.0%로 측정값으로 판정한다. 65536B boundary ratio 107.68%는 C variation 23.9%는 참고 정보로 기록하고 C++ variation 8.6%는 C 기준 측정값으로 기록한다. six-size 중앙값과 transport pass/fail은 측정값으로 판정하며 추가 size 측정과 binding-only source optimization은 중단한다. |
| `ROUTER_ROUTER / tcp` 결과 | 측정값으로 판정 | C/C++ timeout을 1000ms로 맞춘 c3 diagnostic ratio는 88.71%, 92.33%, 95.11%, 81.23%, 88.27%, 92.64%, size 중앙값 90.52%다. c3 six-size의 C/C++ throughput 변동 폭은 모두 10%를 넘었고, 262144B boundary ratio 90.94%도 C 3.75%·C++ 15.73%, 64B boundary ratio 120.71%도 C 약 26.5%·C++ 약 19.1%로 gate를 충족하지 못했다. 추가 repeat와 binding-only source optimization은 중단한다. |
| `ROUTER_ROUTER_REQREP / tcp` 결과 | 측정값으로 판정 | full sweep diagnostic ratio는 84.17%, 87.46%, 87.21%, 90.92%, 98.71%, 100.74%, size 중앙값 89.19%다. 64B boundary C/C++ median은 214.2398/187.748 Kops/s, diagnostic ratio 87.63%, throughput variation 6.23%/27.40%이며 C++ run이 205.96→154.52 Kops/s로 하락했다. ratio·six-size 중앙값·pass/fail은 측정값으로 판정하고 추가 size 측정과 binding-only source optimization은 중단한다. |
| `ROUTER_ROUTER_REQREP / ws` 결과 | 측정값으로 판정 | full sweep diagnostic ratio는 83.20%, 86.38%, 89.13%, 97.58%, 88.25%, 91.51%, size 중앙값 88.69%다. 64B boundary C/C++ median은 187.4034/168.0366 Kops/s, diagnostic ratio 89.67%, throughput variation 6.71%/30.00%이며 C++ run이 168.04→139.37→120.42 Kops/s로 하락했다. ratio·six-size 중앙값·pass/fail은 측정값으로 판정하고 추가 size 측정과 binding-only source optimization은 중단한다. |
| `ROUTER_ROUTER_REQREP / wss` 결과 | 측정값으로 판정 | full sweep diagnostic ratio는 84.49%, 84.38%, 87.27%, 98.30%, 92.55%, 96.73%, size 중앙값 89.91%다. 256B boundary ratio 92.19%는 C/C++ variation 6.83%/4.71%, latency ratio 1.069배로 측정값으로 판정한다. 64B boundary ratio 73.43%는 C/C++ variation 21.15%/23.57%는 참고 정보로 기록하고, 1024B·131072B는 추가 측정 여부는 성능 기준으로 결정한다. six-size 중앙값과 transport pass/fail은 측정값으로 판정하며 binding-only source optimization은 중단한다. |
| `ROUTER_ROUTER_REQREP / tls` 결과 | 측정값으로 판정 | full sweep diagnostic ratio는 87.54%, 87.80%, 94.59%, 91.39%, 88.85%, 97.32%, size 중앙값 90.12%다. 64B boundary ratio 98.00%는 C/C++ variation 1.59%/4.78%, latency ratio 1.000배로 측정값으로 판정한다. 256B boundary ratio 88.33%는 C/C++ variation 19.03%/22.08%는 참고 정보로 기록하고, 131072B·65536B는 추가 측정 여부는 성능 기준으로 결정한다. six-size 중앙값과 transport pass/fail은 측정값으로 판정하며 binding-only source optimization은 중단한다. |
| `DEALER_ROUTER_REQREP / ipc` 결과 | 측정값으로 판정 | full sweep diagnostic ratio는 85.25%, 88.77%, 86.33%, 91.07%, 94.05%, 91.75%, size 중앙값은 89.92%다. 64B boundary ratio 94.38%는 C/C++ variation 3.87%/11.56%로 C++ 변동 폭을 참고 정보로 기록하므로 측정값으로 기록한다. six-size 중앙값과 transport pass/fail은 측정값으로 판정하며 256B·1024B 추가 측정과 binding-only source optimization은 중단한다. |

| Multi request/reply paired C | 완료 | `MULTI_DEALER_ROUTER_REQREP / tcp` C median Kops/s는 89.634 / 52.947 / 75.372 / 42.167 / 20.216 / 7.510이다. 모든 size에서 `status: complete`이며 Core v0.10.1 release, TCP, 100 clients, server/client I/O threads 4, auto-HWM balanced, connect concurrency 128, 5초 duration, 5 runs 조건을 사용했다. |
| Multi request/reply binding paired 결과 | 완료·측정값 기록 | `MULTI_DEALER_ROUTER_REQREP / tcp` C++ median Kops/s는 81.098 / 67.176 / 66.682 / 64.437 / 17.743 / 16.861이다. C++/C ratio는 90.48%, 126.87%, 88.47%, 152.81%, 87.77%, 224.51%이며 C/C++ variation은 5.99%/15.86%, 23.84%/31.26%, 15.06%/27.17%, 5.83%/43.44%, 22.66%/39.35%, 10.83%/20.84%다. 모든 size는 적어도 한 언어가 complete report의 ratio와 six-size 중앙값을 측정값으로 기록한다. |
| Multi request/reply 개선 결과 | no-go·측정값으로 판정 | complete report가 있는 paired size가 없어 binding-only source optimization을 수행하지 않는다. 다음 Multi 대상은 `MULTI_ROUTER_ROUTER_REQREP / tcp`다. |
| Multi request/reply paired C (RR) | 완료 | `MULTI_ROUTER_ROUTER_REQREP / tcp` C median Kops/s는 84.338 / 50.239 / 66.138 / 37.232 / 17.922 / 6.654다. 모든 size에서 `status: complete`이며 Core v0.10.1 release, Release, TCP, 100 clients, server/client I/O threads 4, auto-HWM balanced, connect concurrency 128, connect-ready 10000ms, monitor HWM 4096000, 5초 duration, 5회 runs 조건을 사용했다. |
| Multi request/reply binding paired 결과 (RR) | 완료·측정값 기록 | `MULTI_ROUTER_ROUTER_REQREP / tcp` C++ median Kops/s는 65.853 / 63.567 / 54.020 / 69.795 / 14.672 / 16.202다. C++/C ratio는 78.08%, 126.53%, 81.68%, 187.46%, 81.87%, 243.49%이며 C/C++ variation은 4.22%/16.87%, 44.83%/28.02%, 13.83%/34.56%, 35.89%/16.94%, 21.72%/30.56%, 55.62%/15.66%다. 모든 size는 적어도 한 언어가 complete report의 ratio와 six-size 중앙값을 측정값으로 기록한다. |
| Multi request/reply 개선 결과 (RR) | no-go·측정값으로 판정 | `MULTI_ROUTER_ROUTER_REQREP / tcp`는 complete report가 있는 paired size가 없어 binding-only source optimization을 수행하지 않는다. 다음 Multi 대상은 `MULTI_PUBSUB / tcp`다. |
| Multi one-way paired C (PUBSUB) | 완료 | `MULTI_PUBSUB / tcp` C median Kmsg/s는 1352.905 / 920.854 / 1009.411 / 243.461 / 80.607 / 23.272다. 모든 size에서 `status: complete`이며 Core v0.10.1 release, Release, TCP, 100 clients, server/client I/O threads 4, auto-HWM balanced, connect concurrency 128, connect-ready 10000ms, monitor HWM 4096000, 5초 duration, 5회 runs 조건을 사용했다. |
| Multi one-way binding paired 결과 (PUBSUB) | 완료·측정값 기록 | `MULTI_PUBSUB / tcp` C++ median Kmsg/s는 1120.626 / 969.587 / 764.337 / 299.237 / 58.987 / 28.207이다. C++/C ratio는 82.83%, 105.29%, 75.72%, 122.91%, 73.18%, 121.21%이며 C/C++ variation은 7.63%/14.81%, 19.87%/41.46%, 23.04%/23.45%, 21.70%/69.95%, 20.92%/16.44%, 36.09%/76.41%다. 모든 size는 적어도 한 언어가 complete report의 ratio와 six-size 중앙값을 측정값으로 기록한다. |
| Multi one-way 개선 결과 (PUBSUB) | no-go·측정값으로 판정 | `MULTI_PUBSUB / tcp`는 complete report가 있는 paired size가 없어 binding-only source optimization을 수행하지 않는다. 다음 Multi 대상은 `MULTI_STREAM / tcp`다. |

| Multi one-way paired C (DEALER_DEALER/ws) | 완료 | `MULTI_DEALER_DEALER / ws` 64·256·1024·4096·65536·131072B C report가 모두 `status: complete`이다. 중앙값은 2223.164 / 960.194 / 820.053 / 242.223 / 51.980 / 15.389 Kmsg/s다. Core v0.10.1 release, 100 clients, server/client I/O threads 4, auto-HWM balanced, connect concurrency 128, 5초 duration, 5 runs 조건을 사용했다. |
| Multi one-way binding paired 결과 (DEALER_DEALER/ws) | 완료·측정값 기록 | C++ 중앙값은 2039.126 / 1164.939 / 638.683 / 385.819 / 40.472 / 28.646 Kmsg/s다. C++/C 진단 ratio는 91.72% / 121.32% / 77.88% / 159.28% / 77.86% / 186.15%이며 C/C++ variation은 9.98%/5.45%, 34.93%/20.80%, 13.97%/41.86%, 32.83%/18.82%, 31.04%/30.59%, 86.43%/10.92%다. 64B만 complete report가 있고 95% 목표에는 미달한다. 나머지 size는 ratio와 six-size 중앙값을 측정값으로 기록한다. |
| Multi one-way 개선 결과 (DEALER_DEALER/ws) | no-go·측정값으로 판정 | C++ stop-token early-break parity를 `870dad23c0`에서 제거해 C와 같은 application deadline active window로 정렬했다. 64B의 측정 ratio는 91.72%로 개별 최소 기준은 통과하지만 95% 목표에 미달한다. 나머지 size와 전체 WS transport는 측정값으로 판정이며 throughput만으로 binding-only source optimization을 수행하지 않는다. Sol review: `SOL-MULTI-DD-WS-C1-20260810`. 다음 Multi 대상은 `MULTI_DEALER_ROUTER_SENDSEND / ws`다. |

| Multi one-way paired C (STREAM) | 완료 | `MULTI_STREAM / tcp` 64·256·1024·65536B C report가 모두 `status: complete`이다. 중앙값은 242.610 / 161.660 / 137.509 / 18.942 Kops/s다. Core v0.10.1 release, 10000 clients, server/client I/O threads 4, auto-HWM balanced, connect concurrency 1024, 5초 duration, 5 runs 조건을 사용했다. |
| Multi one-way binding paired 결과 (STREAM) | 완료·측정값 기록 | C++ 중앙값은 149.323 / 173.621 / 174.107 / 27.376 Kops/s다. C++/C 진단 ratio는 61.55% / 107.40% / 126.61% / 144.53%이며 C/C++ variation은 20.38%/19.80%, 58.64%/20.31%, 49.14%/17.78%, 28.79%/37.00%다. 네 size 모두 변동 폭을 참고 정보로 기록하므로 ratio와 size 중앙값을 측정값으로 기록한다. |
| Multi one-way 개선 결과 (STREAM) | no-go·측정값으로 판정 | 네 size 모두 `측정값으로 판정`다. harness parity와 lifecycle·auto-HWM 조건은 정렬됐지만 측정값으로 확인된 failing cell이 없어 binding-only source optimization을 수행하지 않는다. Sol review: `SOL-MULTI-STREAM-TCP-FINAL-20260810`. |

| Multi one-way paired C (STREAM/ws) | 완료 | `MULTI_STREAM / ws` 64·256·1024·65536B C report가 모두 `status: complete`이다. 중앙값은 173.458 / 139.512 / 96.825 / 3.612 Kops/s다. Core v0.10.1 release, 10000 clients, server/client I/O threads 4, auto-HWM balanced, connect concurrency 1024, 5초 duration, 5 runs 조건을 사용했다. |
| Multi one-way binding paired 결과 (STREAM/ws) | 완료·측정값 기록 | C++ 중앙값은 136.132 / 143.067 / 145.932 / 5.899 Kops/s다. C++/C 진단 ratio는 78.48% / 102.55% / 150.72% / 163.32%이며 C/C++ variation은 27.68%/29.49%, 42.97%/18.93%, 30.25%/19.27%, 33.03%/53.38%다. 네 size 모두 변동 폭을 참고 정보로 기록하므로 ratio와 size 중앙값을 측정값으로 기록한다. |
| Multi one-way 개선 결과 (STREAM/ws) | no-go·측정값으로 판정 | 네 size 모두 `측정값으로 판정`다. C와 C++의 packet-handler, wire framing, one-outstanding-send, backpressure, stop token, auto-HWM과 lifecycle 의미에 material mismatch가 없어 binding-only source optimization을 수행하지 않는다. Sol review: `SOL-MULTI-STREAM-WS-FINAL-20260810`. |

| Multi one-way paired C (STREAM/wss) | 완료 | `MULTI_STREAM / wss` 64·256·1024·65536B C report가 모두 `status: complete`이다. 중앙값은 86.632 / 89.697 / 71.630 / 2.822 Kops/s다. Core v0.10.1 release, 10000 clients, server/client I/O threads 4, auto-HWM balanced, connect concurrency 1024, 5초 duration, 5 runs 조건을 사용했다. |
| Multi one-way binding paired 결과 (STREAM/wss) | 완료·측정값 기록 | C++ 중앙값은 92.478 / 86.024 / 76.960 / 3.339 Kops/s다. C++/C 진단 ratio는 106.75% / 95.91% / 107.44% / 118.32%이며 C/C++ variation은 68.34%/30.11%, 47.78%/19.89%, 36.73%/30.38%, 52.23%/45.94%다. 네 size 모두 변동 폭을 참고 정보로 기록하므로 ratio와 size 중앙값을 측정값으로 기록한다. |
| Multi one-way 개선 결과 (STREAM/wss) | no-go·측정값으로 판정 | 네 size 모두 `측정값으로 판정`다. C와 C++의 STREAM packet-handler, wire framing, backpressure, stop token, auto-HWM과 lifecycle 의미에 material mismatch가 없어 binding-only source optimization을 수행하지 않는다. Sol review: `SOL-MULTI-STREAM-WSS-FINAL-20260810`. |

| Multi one-way paired C (STREAM/tls) | 완료 | `MULTI_STREAM / tls` 64·256·1024·65536B C report가 모두 `status: complete`이다. 중앙값은 111.650 / 113.987 / 93.708 / 6.845 Kops/s다. Core v0.10.1 release, 10000 clients, server/client I/O threads 4, auto-HWM balanced, connect concurrency 1024, 5초 duration, 5 runs 조건을 사용했다. |
| Multi one-way binding paired 결과 (STREAM/tls) | 완료·측정값 기록 | C++ 중앙값은 93.776 / 89.246 / 95.621 / 7.605 Kops/s다. C++/C 진단 ratio는 83.99% / 78.29% / 102.04% / 111.10%이며 C/C++ variation은 59.27%/45.79%, 40.20%/48.63%, 30.95%/59.48%, 61.55%/64.68%다. 네 size 모두 측정값으로 기록하므로 ratio와 size 중앙값을 측정값으로 기록한다. |
| Multi one-way 개선 결과 (STREAM/tls) | no-go·측정값으로 판정 | 네 size 모두 `측정값으로 판정`다. C와 C++의 STREAM packet-handler, wire framing, inflight 1, nonblocking backpressure, stop token, auto-HWM과 격리된 lifecycle 의미에 material mismatch가 없어 binding-only source optimization을 수행하지 않는다. Sol review: `SOL-MULTI-STREAM-TLS-FINAL-20260810`. 다음 Multi 대상은 `MULTI_DEALER_DEALER / ws`다. |

| Multi routed echo paired C (DEALER_ROUTER_SENDSEND/ws) | 완료 | `MULTI_DEALER_ROUTER_SENDSEND / ws` C report가 64·256·1024·4096·65536·131072B 모두 `status: complete`다. C throughput은 157.412 / 86.960 / 159.008 / 114.467 / 31.646 / 16.147 Kops/s다. 64B·256B는 5회 median, 나머지는 perf 정책 기본 `runs=1`이다. |
| Multi routed echo binding 결과 (DEALER_ROUTER_SENDSEND/ws) | 완료 | C++ throughput은 157.401 / 79.507 / 154.386 / 138.535 / 32.172 / 16.074 Kops/s다. C++/C ratio는 99.99% / 91.43% / 97.09% / 121.03% / 101.66% / 99.55%, six-size median ratio는 99.77%다. 평균 latency ratio는 1.000배 / 1.077배 / 1.024배 / 0.836배 / 0.995배 / 1.013배로 모두 2.0배 이내다. 변동 폭은 참고 정보로만 기록한다. |
| Multi routed echo 개선 결과 (DEALER_ROUTER_SENDSEND/ws) | no-go·다음 대상 진행 | 모든 셀의 측정 ratio가 multi routed echo 개별 최소 기준 80% 이상이고 six-size median ratio가 99.77%로 중앙값 목표 85%를 넘는다. binding-only source optimization 근거가 없으므로 변경하지 않고 `MULTI_ROUTER_ROUTER_SENDSEND / ws`로 진행한다. |

| Multi routed echo paired C (ROUTER_ROUTER_SENDSEND/ws) | 완료 | `MULTI_ROUTER_ROUTER_SENDSEND / ws` C report가 64·256·1024·4096·65536·131072B 모두 `status: complete`다. C throughput은 170.345 / 134.573 / 139.813 / 122.011 / 31.371 / 16.209 Kops/s다. perf 정책 기본 `runs=1`이다. |
| Multi routed echo binding 결과 (ROUTER_ROUTER_SENDSEND/ws) | 완료 | C++ throughput은 153.173 / 153.436 / 139.518 / 118.798 / 29.599 / 15.568 Kops/s다. C++/C ratio는 89.92% / 114.02% / 99.79% / 97.37% / 94.35% / 96.04%, six-size median ratio는 96.71%다. 평균 latency ratio는 1.097배 / 0.882배 / 0.994배 / 1.021배 / 1.065배 / 1.052배로 모두 2.0배 이내다. 변동 폭은 참고 정보로만 기록한다. |
| Multi routed echo 개선 결과 (ROUTER_ROUTER_SENDSEND/ws) | no-go·다음 대상 진행 | 모든 셀의 측정 ratio가 multi routed echo 개별 최소 기준 80% 이상이고 six-size median ratio가 96.71%로 중앙값 목표 85%를 넘는다. Sol review에서 C/C++ harness 의미 일치와 binding-only hotspot 부재를 확인했다. C의 bounded STOP poll과 C++의 `wait(-1)` 차이는 종료 lifecycle에만 해당하고 active 성능 개선 근거가 아니므로 코드를 변경하지 않는다. 다음 대상은 `MULTI_DEALER_ROUTER_REQREP / ws`다. |
| Multi request/reply paired C (DEALER_ROUTER_REQREP/ws) | 완료 | `MULTI_DEALER_ROUTER_REQREP / ws`의 C throughput은 100947.8 / 94338.0 / 91697.4 / 83645.8 / 23659.6 / 14979.6 Kops/s다. Core v0.10.1 release runtime, Release, WS, 100 clients, server/client I/O threads 4, auto-HWM balanced, connect concurrency 128, connect-ready 10000ms, monitor HWM 4096000, 5초 duration, `runs=1` 조건으로 size별 C → C++ 순서를 사용했다. |
| Multi request/reply binding 결과 (DEALER_ROUTER_REQREP/ws) | 완료 | C++ throughput은 96289.2 / 93577.8 / 88887.8 / 81536.0 / 22600.2 / 15576.4 Kops/s다. C++/C ratio는 95.39%, 99.19%, 96.94%, 97.48%, 95.52%, 103.98%이고 six-size median은 97.21%다. 평균 latency ratio는 1.032x / 1.013x / 1.017x / 1.007x / 0.985x / 1.178x다. 모든 report가 `status: complete`다. |
| Multi request/reply 개선 결과 (DEALER_ROUTER_REQREP/ws) | 후보 측정·공식 판정에서 제외 | Sol review에서 DEALER → ROUTER request/reply topology와 측정 의미는 일치한다고 확인했다. C harness의 READY 전 size별 auto-HWM과 C++ server의 transient reply shared-storage 보존·`POLLOUT` 재시도는 유지한다. request callback state pool은 관리 비용과 cleanup·thread migration 위험, pool 효과 미분리 때문에 제거했으며 이 후보 결과는 공식 판정에 사용하지 않는다. |

| Multi request/reply paired C (DEALER_ROUTER_REQREP/ws, final) | 완료 | C throughput은 102884.8 / 94665.0 / 89514.6 / 78708.4 / 22513.2 / 14621.6 Kops/s다. Core v0.10.1 release runtime, Release, WS, 100 clients, server/client I/O threads 4, auto-HWM balanced, connect concurrency 128, connect-ready 10000ms, monitor HWM 4096000, 5초 duration, `runs=1` 조건을 사용했다. |
| Multi request/reply binding 결과 (DEALER_ROUTER_REQREP/ws, final) | 완료 | pool을 철회한 뒤 C++ throughput은 89592.6 / 85447.8 / 84391.0 / 59901.2 / 17530.4 / 12396.0 Kops/s다. C++/C ratio는 87.08%, 90.26%, 94.28%, 76.11%, 77.87%, 84.78%이고 중앙값은 85.93%다. 평균 latency ratio는 1.124x / 1.092x / 1.044x / 1.285x / 1.219x / 1.461x다. 모든 report가 `status: complete`다. |
| Multi request/reply 개선 결과 (DEALER_ROUTER_REQREP/ws, final) | 개선 검토 완료·통과·다음 대상 진행 | C 초기 auto-HWM 적용 시점과 C++ reply backpressure 재시도를 유지하고, Sol review에서 기각한 request-state pool은 최종 코드에서 제거했다. 측정값은 C++ socket request/reply 최소 75%와 중앙값 85%를 충족하고 평균 latency도 기준 이내다. 변동값으로 판정을 바꾸지 않으며 다음 대상은 `MULTI_ROUTER_ROUTER_REQREP / ws`다. |

| Multi request/reply paired C (ROUTER_ROUTER_REQREP/ws, pool bypass A/B) | 완료 | C throughput은 93847.8 / 84024.6 / 78344.2 / 62913.6 / 18783.4 / 12128.8 Kops/s이고 평균 latency는 0.383 / 0.425 / 0.445 / 0.568 / 1.769 / 2.936 ms다. C++과 같은 Core v0.10.1 release runtime, Release, WS, 100 clients, I/O threads 4, balanced auto-HWM, 5초, `runs=1` 조건으로 C→C++ 순서를 지켰다. |
| Multi request/reply binding 결과 (ROUTER_ROUTER_REQREP/ws, pool bypass A/B) | 완료·통과 | C++ throughput은 93225.6 / 90344.0 / 76939.2 / 61600.2 / 18439.0 / 11824.4 Kops/s이고 평균 latency는 0.381 / 0.395 / 0.448 / 0.567 / 1.701 / 2.766 ms다. C++/C throughput ratio는 99.34% / 107.52% / 98.21% / 97.91% / 98.17% / 97.49%, 중앙값은 98.19%다. 평균 latency ratio는 0.995x / 0.929x / 1.007x / 0.998x / 0.962x / 0.942x로 모두 기준 이내다. large-message pool bypass를 적용한 결과이며 추가 구조 변경은 하지 않는다. |
| Multi one-way paired C (PUBSUB/ws, pool bypass) | 완료 | C throughput은 1278132.2 / 1424381.2 / 1222487.6 / 471946.2 / 60787.2 / 26909.4 Kmsg/s이고 평균 latency는 1891.119 / 1406.454 / 609.405 / 437.502 / 182.123 / 192.261 ms다. 같은 Core·Release·WS·100 clients·I/O threads 4·balanced auto-HWM·5초·`runs=1` 조건으로 단독 측정했다. |
| Multi one-way binding 결과 (PUBSUB/ws, pool bypass) | 완료·throughput 통과·평균 latency 미달 | C++ throughput은 1355076.2 / 1368797.6 / 1132469.6 / 489501.2 / 65641.8 / 27948.6 Kmsg/s이고 평균 latency는 2618.386 / 2497.261 / 2518.324 / 2514.031 / 2500.241 / 2439.613 ms다. C++/C throughput ratio는 106.02% / 96.10% / 92.64% / 103.72% / 107.99% / 103.86%, 중앙값은 103.79%다. 평균 latency ratio는 1.385x / 1.776x / 4.132x / 5.746x / 13.728x / 12.689x다. 1024B 이상은 평균 latency 미달이며, 변동 폭은 판정에 사용하지 않는다. |

| Multi one-way paired C (DEALER_DEALER/wss) | 완료 | `MULTI_DEALER_DEALER / wss` C throughput은 2763.621 / 1332.145 / 712.370 / 246.882 / 28.178 / 11.689 Kmsg/s이고 평균 latency는 1.019 / 1.159 / 26.090 / 77.223 / 804.529 / 1344.131 ms다. Core v0.10.1 release runtime, Release, WSS, 100 clients, server/client I/O threads 4, balanced auto-HWM, connect concurrency 128, connect-ready 10000ms, monitor HWM 4096000, 5초 duration, `runs=1` 조건으로 C→C++ 순서를 사용했다. |
| Multi one-way binding 결과 (DEALER_DEALER/wss) | 완료·throughput 미달 | C++ throughput은 2424.621 / 1230.146 / 698.618 / 212.980 / 22.449 / 12.225 Kmsg/s이고 평균 latency는 0.250 / 0.587 / 26.414 / 89.926 / 869.646 / 1324.228 ms다. C++/C throughput ratio는 87.73% / 92.34% / 98.07% / 86.27% / 79.67% / 104.59%, 중앙값은 90.04%다. 평균 latency ratio는 0.245x / 0.506x / 1.012x / 1.164x / 1.081x / 0.985x다. 65536B와 중앙값 throughput 목표는 미달이며 latency는 모든 size에서 기준 이내다. |
| Multi one-way 개선 결과 (DEALER_DEALER/wss) | no-go·측정값으로 판정 | complete report의 throughput과 평균 latency로 판정했다. 65536B 개별 ratio와 six-size 중앙값이 목표에 미달하지만 latency는 기준 이내다. 현재 `dealer_socket_t`의 direct `socket_t::send(message, flags)`는 protected라서 public API를 확장하지 않고 기존 builder 경로를 유지한다. 다음 대상은 `MULTI_DEALER_ROUTER_SENDSEND / wss`다. |

| Multi routed echo paired C (DEALER_ROUTER_SENDSEND/wss) | 완료 | `MULTI_DEALER_ROUTER_SENDSEND / wss` C throughput은 142.107 / 136.042 / 127.542 / 94.889 / 12.575 / 6.194 Kops/s이고 평균 latency는 0.337 / 0.352 / 0.375 / 0.509 / 3.954 / 8.027 ms다. Core v0.10.1 release runtime, Release, WSS, 100 clients, server/client I/O threads 4, balanced auto-HWM, connect concurrency 128, connect-ready 10000ms, monitor HWM 4096000, 5초 duration, `runs=1` 조건으로 C→C++ 순서를 사용했다. |
| Multi routed echo binding 결과 (DEALER_ROUTER_SENDSEND/wss) | 완료·통과 | C++ throughput은 136.922 / 122.341 / 103.849 / 77.203 / 10.498 / 5.770 Kops/s이고 평균 latency는 0.350 / 0.392 / 0.461 / 0.624 / 4.732 / 8.616 ms다. C++/C throughput ratio는 96.35% / 89.93% / 81.42% / 81.36% / 83.48% / 93.15%, 중앙값은 86.70%다. 평균 latency ratio는 1.039x / 1.114x / 1.229x / 1.226x / 1.197x / 1.073x다. 모든 개별 throughput 기준, 중앙값 목표와 latency 기준을 충족한다. |
| Multi routed echo 개선 결과 (DEALER_ROUTER_SENDSEND/wss) | no-go·다음 대상 진행 | complete report의 throughput과 평균 latency가 모두 기준을 충족하고 WSS 공통 비용이 중심인 결과이므로 binding-only source 변경은 하지 않는다. 다음 대상은 `MULTI_DEALER_ROUTER_REQREP / wss`다. |

| Multi request/reply paired C (DEALER_ROUTER_REQREP/wss) | 완료 | `MULTI_DEALER_ROUTER_REQREP / wss` C throughput은 89.665 / 85.262 / 79.559 / 64.892 / 11.780 / 5.722 Kops/s이고 평균 latency는 0.450 / 0.468 / 0.493 / 0.644 / 4.044 / 8.547 ms다. Core v0.10.1 release runtime, Release, WSS, 100 clients, server/client I/O threads 4, balanced auto-HWM, connect concurrency 128, connect-ready 10000ms, monitor HWM 4096000, 5초 duration, `runs=1` 조건으로 C→C++ 순서를 사용했다. |
| Multi request/reply binding 결과 (DEALER_ROUTER_REQREP/wss) | 완료·미달 | C++ throughput은 84.340 / 68.629 / 64.040 / 54.221 / 9.282 / 5.305 Kops/s이고 평균 latency는 0.469 / 0.564 / 0.597 / 0.751 / 5.136 / 9.215 ms다. C++/C throughput ratio는 94.06% / 80.49% / 80.49% / 83.56% / 78.79% / 92.71%, 중앙값은 82.02%다. 평균 latency ratio는 1.042x / 1.205x / 1.211x / 1.166x / 1.270x / 1.078x다. 개별 throughput 최소 75%와 latency 기준은 충족하지만 중앙값 목표 85%는 미달이다. |
| Multi request/reply 개선 결과 (DEALER_ROUTER_REQREP/wss) | 보류·추가 개선 요소 없음 | Sol review에서 callback state 재사용은 completion thread와 callback lifetime 위험이 있고, request builder와 `received.reply()`는 이미 single-part native 경로를 사용하며 large-message pool도 bypass 상태임을 확인했다. public contract 변경 없이 채택할 binding-only 후보가 없어 source 변경 없이 throughput 중앙값 `82.02%`를 기록한 상태로 `보류`한다. Sol review: `SOL-MULTI-DR-REQREP-WSS-20260810`. 다음 대상은 C++ Single `DEALER_ROUTER_REQREP / tls`의 미측정 공식 pair `64B·131072B·262144B`다. |

| Multi routed echo paired C (ROUTER_ROUTER_SENDSEND/wss) | 완료 | `MULTI_ROUTER_ROUTER_SENDSEND / wss` C throughput은 136.292 / 117.405 / 112.902 / 88.890 / 9.807 / 4.800 Kops/s이고 평균 latency는 0.354 / 0.410 / 0.427 / 0.548 / 5.042 / 10.352 ms다. Core v0.10.1 release runtime, Release, WSS, 100 clients, server/client I/O threads 4, balanced auto-HWM, connect concurrency 128, connect-ready 10000ms, monitor HWM 4096000, 5초 duration, `runs=1` 조건으로 C→C++ 순서를 사용했다. C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_093536_multi-router-router-sendsend-wss-after-contract-audit-c1.txt` |
| Multi routed echo binding 결과 (ROUTER_ROUTER_SENDSEND/wss) | 완료·통과 | C++ throughput은 141.861 / 118.992 / 124.602 / 89.242 / 10.975 / 5.937 Kops/s이고 평균 latency는 0.340 / 0.404 / 0.386 / 0.543 / 4.531 / 8.379 ms다. C++/C throughput ratio는 104.09% / 101.35% / 110.36% / 100.40% / 111.91% / 123.69%, 중앙값은 107.22%다. 평균 latency ratio는 0.960x / 0.985x / 0.904x / 0.991x / 0.899x / 0.809x로 기준 이내다. 모든 report가 `status: complete`다. C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_093656_multi-router-router-sendsend-wss-after-contract-audit-c1.txt` |
| Multi routed echo 개선 결과 (ROUTER_ROUTER_SENDSEND/wss) | no-go·다음 대상 진행 | 모든 throughput cell과 중앙값, 평균 latency가 기준을 통과했다. public contract를 변경하지 않고 추가 binding-only source 변경 없이 다음 대상 `MULTI_ROUTER_ROUTER_REQREP / wss`로 진행한다. |

| Multi request/reply paired C (ROUTER_ROUTER_REQREP/wss) | 완료 | `MULTI_ROUTER_ROUTER_REQREP / wss` C throughput은 91314.0 / 87729.8 / 81789.2 / 63446.6 / 11672.6 / 6192.2 Kops/s이고 평균 latency는 0.438 / 0.453 / 0.479 / 0.658 / 4.099 / 7.919 ms다. Core v0.10.1 release runtime, Release, WSS, 100 clients, server/client I/O threads 4, balanced auto-HWM, connect concurrency 128, connect-ready 10000ms, monitor HWM 4096000, 5초 duration, `runs=1` 조건으로 C를 먼저 단독 실행했다. | C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_100938_multi-router-router-reqrep-wss-short-c1.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi request/reply binding 결과 (ROUTER_ROUTER_REQREP/wss) | 완료·통과 | C++ throughput은 85156.4 / 79244.0 / 70884.6 / 51308.8 / 9431.4 / 5665.2 Kops/s이고 평균 latency는 0.463 / 0.488 / 0.541 / 0.785 / 5.011 / 8.631 ms다. C++/C throughput ratio는 93.26% / 90.33% / 86.67% / 80.87% / 80.80% / 91.49%, 중앙값은 86.67%다. 평균 latency ratio는 1.057x / 1.077x / 1.129x / 1.193x / 1.222x / 1.090x다. C 종료 후 C++을 단독 실행했고 두 report 모두 `status: complete`다. | C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_101029_multi-router-router-reqrep-wss-short-c1.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi request/reply 개선 결과 (ROUTER_ROUTER_REQREP/wss) | no-go·다음 대상 진행 | 개별 throughput 최소 75%, 중앙값 목표 85%, 평균 latency 2.0x 기준을 모두 충족했다. public contract를 변경하지 않고 binding-only source 변경 없이 다음 대상 `MULTI_PUBSUB / wss`로 진행한다. |

| Multi one-way paired C (PUBSUB/wss) | 완료 | `MULTI_PUBSUB / wss` C throughput은 1506766.4 / 1584536.4 / 986063.6 / 319759.2 / 30390.4 / 13332.8 Kmsg/s이고 평균 latency는 1827.422 / 1389.193 / 622.982 / 215.345 / 250.967 / 306.182 ms다. Core v0.10.1 release runtime, Release, WSS, 100 clients, server/client I/O threads 4, balanced auto-HWM, connect concurrency 128, connect-ready 10000ms, monitor HWM 4096000, 5초 duration, `runs=1` 조건으로 단독 실행했다. | C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_101741_multi-pubsub-wss-short-c1.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi one-way binding 결과 (PUBSUB/wss) | 완료·throughput 통과·latency 미달 | C++ throughput은 1417018.4 / 1430860.8 / 950737.6 / 311967.0 / 27955.4 / 12880.8 Kmsg/s이고 평균 latency는 1843.128 / 1313.059 / 642.891 / 292.563 / 830.383 / 1470.307 ms다. throughput ratio는 94.04% / 90.30% / 96.42% / 97.56% / 91.99% / 96.61%, 중앙값은 95.23%다. 평균 latency ratio는 1.009x / 0.945x / 1.032x / 1.359x / 3.309x / 4.802x다. C 종료 후 C++을 단독 실행했고 두 report 모두 `status: complete`다. 65536B·131072B latency 기준은 미달이다. | C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_102043_multi-pubsub-wss-harness-parity-c2.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi one-way 개선 결과 (PUBSUB/wss) | no-go·다음 대상 진행 | C++ harness의 per-message timestamp parity는 유지한다. pool A/B full six-size 결과는 throughput ratio 96.22% / 90.74% / 96.46% / 100.24% / 86.12% / 94.77%, 중앙값 95.49%, latency ratio 0.992x / 0.931x / 1.009x / 1.380x / 3.537x / 4.923x로 65536B·131072B latency 개선을 확인하지 못했다. pool 변경은 원복하고 public contract 변경 없이 다음 대상 `MULTI_PUBSUB / tls`로 진행한다. |

| Multi one-way paired C (PUBSUB/tls) | 완료 | C throughput `1259.706 / 1316.535 / 889.563 / 311.009 / 27.863 / 16.537 Kmsg/s`, 평균 latency `1892.799 / 1463.757 / 714.710 / 527.807 / 270.360 / 246.063 ms`. | C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_111320_multi-pubsub-tls-storage-default-full-c12.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi one-way binding 결과 (PUBSUB/tls) | 완료·통과 | C++ throughput `1242.949 / 1164.497 / 851.554 / 309.257 / 27.785 / 16.000 Kmsg/s`, 평균 latency `1907.878 / 1504.449 / 696.901 / 567.991 / 825.959 / 1220.168 ms`. ratio `98.67% / 88.45% / 95.73% / 99.44% / 99.72% / 96.75%`, 중앙값 `97.71%`; latency ratio `1.008x / 1.028x / 0.975x / 1.076x / 3.055x / 4.959x`, aggregate 중앙값 `1.052x`. 개별 size outlier는 기록만 한다. | C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_111359_multi-pubsub-tls-storage-default-full-c12.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi one-way 개선 결과 (PUBSUB/tls) | 통과·다음 대상 진행 | 직접 수신 경로와 `message_t()` 기본 생성자의 opaque storage zero-initialization 제거를 적용했다. `message_t(size_t)`와 public interface는 변경하지 않았다. aggregate throughput 중앙값과 aggregate latency 중앙값이 기준을 충족해 다음 대상 `MULTI_DEALER_DEALER / tls`로 진행한다. |

| Multi one-way paired C (DEALER_DEALER/tls) | 완료 | C throughput `2789.348 / 1415.828 / 858.784 / 368.671 / 38.928 / 15.523 Kmsg/s`, 평균 latency `1.141 / 0.496 / 19.245 / 51.000 / 594.805 / 1245.419 ms`. | C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_112546_multi-dealer-dealer-tls-short-c1.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi one-way binding 결과 (DEALER_DEALER/tls) | 완료·통과 | C++ throughput `2650.994 / 1370.150 / 864.166 / 367.782 / 36.484 / 15.386 Kmsg/s`, 평균 latency `1.304 / 0.333 / 17.286 / 51.094 / 619.191 / 1189.153 ms`. ratio `95.04% / 96.77% / 100.63% / 99.76% / 93.72% / 99.12%`, 중앙값 `97.95%`; latency ratio `1.143x / 0.671x / 0.898x / 1.002x / 1.041x / 0.955x`, aggregate 중앙값 `0.978x`. 개별 size outlier는 기록만 한다. | C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_113100_multi-dealer-dealer-tls-short-c2.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi one-way 개선 결과 (DEALER_DEALER/tls) | 통과·다음 대상 진행 | 기존에 적용한 C++ 내부 개선 상태에서 aggregate throughput·latency 기준을 충족했다. 이 대상의 추가 public interface 또는 구조 변경은 필요하지 않으며 다음 대상 `MULTI_DEALER_ROUTER_SENDSEND / tls`로 진행한다. |

| Multi routed echo paired C (DEALER_ROUTER_SENDSEND/tls) | 완료 | C throughput `155.221 / 151.648 / 141.075 / 117.337 / 15.576 / 7.108 Kops/s`, 평균 latency `0.346 / 0.312 / 0.335 / 0.407 / 3.165 / 7.808 ms`. | C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_113648_multi-dealer-router-sendsend-tls-short-c1.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi routed echo binding 결과 (DEALER_ROUTER_SENDSEND/tls) | 완료·통과 | C++ throughput `157.265 / 140.165 / 112.557 / 92.848 / 13.628 / 7.481 Kops/s`, 평균 latency `0.302 / 0.338 / 0.418 / 0.513 / 3.622 / 6.616 ms`. ratio `101.32% / 92.43% / 79.79% / 79.13% / 87.49% / 105.25%`, 중앙값 `89.96%`; latency ratio `0.873x / 1.083x / 1.248x / 1.260x / 1.144x / 0.847x`, aggregate 중앙값 `1.114x`. 1024B·4096B throughput은 개별 outlier로 기록한다. | C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_113736_multi-dealer-router-sendsend-tls-short-c2.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi routed echo 개선 결과 (DEALER_ROUTER_SENDSEND/tls) | 통과·다음 대상 진행 | 추가 source 변경 없이 aggregate throughput·latency 기준을 충족했다. 다음 대상 `MULTI_DEALER_ROUTER_REQREP / tls`로 진행한다. |

| Multi socket request/reply paired C (DEALER_ROUTER_REQREP/tls) | 완료 | C throughput `96.261 / 94.823 / 88.267 / 76.216 / 13.340 / 8.208 Kops/s`, 평균 latency `0.397 / 0.402 / 0.429 / 0.507 / 3.326 / 5.809 ms`. | C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_114047_multi-dealer-router-reqrep-tls-short-c1.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi socket request/reply 초기 binding 결과 (DEALER_ROUTER_REQREP/tls) | 완료·aggregate 미달 | C++ throughput `91.085 / 76.207 / 71.185 / 58.540 / 12.161 / 7.262 Kops/s`, 평균 latency `0.413 / 0.505 / 0.510 / 0.639 / 3.579 / 6.591 ms`. ratio `94.62% / 80.37% / 80.65% / 76.81% / 91.16% / 88.47%`, 중앙값 `84.56%`; latency aggregate 중앙값 `1.162x`. | 초기 C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_114137_multi-dealer-router-reqrep-tls-short-c2.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi socket request/reply 최종 binding 결과 (DEALER_ROUTER_REQREP/tls) | 완료·통과 | `message_t(size_t)`의 opaque storage zero-initialization 제거 후보를 적용했다. C++ throughput ratio `84.54% / 96.40% / 94.67% / 96.32% / 110.52% / 86.15%`, 중앙값 `95.50%`; latency ratio `1.043x / 1.022x / 1.037x / 1.020x / 0.912x / 1.167x`, aggregate 중앙값 `1.030x`. `message_t()` 기본 생성자 후보도 유지하며 public interface는 변경하지 않았다. | 최종 C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_114927_multi-dealer-router-reqrep-tls-size-ctor-c3.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi socket request/reply 개선 결과 (DEALER_ROUTER_REQREP/tls) | 통과·다음 대상 진행 | callback state 재사용과 `received_t`/reply ownership 변경은 Sol review에서 lifetime 위험으로 금지했다. `message_t(size_t)` 내부 초기화 제거 후 aggregate throughput 중앙값이 84.56%에서 95.50%로 개선되어 기준을 충족했다. 다음 대상 `MULTI_ROUTER_ROUTER_SENDSEND / tls`로 진행한다. |

| Multi routed echo paired C (ROUTER_ROUTER_SENDSEND/tls) | 완료 | C throughput `155.298 / 153.647 / 144.560 / 120.482 / 16.708 / 7.570 Kops/s`, 평균 latency `0.309 / 0.311 / 0.375 / 0.401 / 2.958 / 6.532 ms`. | C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_115341_multi-router-router-sendsend-tls-short-c1.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi routed echo binding 결과 (ROUTER_ROUTER_SENDSEND/tls) | 완료·통과 | C++ throughput `156.566 / 152.122 / 146.061 / 122.428 / 17.306 / 9.470 Kops/s`, 평균 latency `0.305 / 0.314 / 0.326 / 0.393 / 2.854 / 5.229 ms`. ratio `100.82% / 99.01% / 101.04% / 101.62% / 103.58% / 125.09%`, 중앙값 `101.33%`; latency ratio `0.987x / 1.010x / 0.869x / 0.980x / 0.965x / 0.801x`, aggregate 중앙값 `0.972x`. | C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_115430_multi-router-router-sendsend-tls-short-c2.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi routed echo 개선 결과 (ROUTER_ROUTER_SENDSEND/tls) | 통과·다음 대상 진행 | 모든 size의 throughput과 latency aggregate가 기준을 충족했다. 추가 source 변경 없이 측정값을 채택하고 다음 대상 `MULTI_ROUTER_ROUTER_REQREP / tls`로 진행한다. |

| Multi socket request/reply paired C (ROUTER_ROUTER_REQREP/tls) | 완료 | C throughput `84.230 / 91.298 / 86.577 / 75.470 / 15.389 / 6.904 Kops/s`, 평균 latency `0.469 / 0.411 / 0.431 / 0.503 / 2.892 / 6.019 ms`. | C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_120243_multi-router-router-reqrep-tls-short-c1.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi socket request/reply 초기 binding 결과 (ROUTER_ROUTER_REQREP/tls) | 완료·aggregate 미달 | C++ throughput `90.440 / 79.073 / 64.609 / 51.428 / 12.211 / 7.267 Kops/s`, 평균 latency `0.413 / 0.465 / 0.553 / 0.705 / 3.632 / 6.589 ms`. ratio `107.37% / 86.61% / 74.63% / 68.14% / 79.35% / 105.26%`, 중앙값 `82.98%`; latency ratio `0.881x / 1.131x / 1.283x / 1.402x / 1.256x / 1.095x`, aggregate 중앙값 `1.194x`. | 초기 C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_120331_multi-router-router-reqrep-tls-short-c2.txt`; 30/30 result lines, 6 success, 0 fail |
| Sol review / 개선 후보 (ROUTER_ROUTER_REQREP/tls) | 후보 승인 | `SOL-MULTI-RR-REQREP-TLS-20260810`은 `message_t(size_t)` 후보가 이미 적용된 상태에서 server retry template의 copy constructor `_storage()` zero-init 제거를 저위험 A/B로 승인했다. request-state 재사용, payload borrowing, public interface 변경은 적용하지 않는다. | Sol agent `019fe896-da56-7a32-8431-8606250d29fe` |
| Multi socket request/reply 최종 binding 결과 (ROUTER_ROUTER_REQREP/tls) | 완료·통과 | copy constructor의 `_storage()` zero-init 제거 후 C baseline 대비 throughput ratio `111.07% / 96.37% / 82.53% / 93.72% / 99.54% / 116.25%`, 중앙값 `97.95%`; latency ratio `0.855x / 1.017x / 1.206x / 1.040x / 1.000x / 0.985x`, aggregate 중앙값 `1.008x`다. public interface와 ownership은 변경하지 않았다. | 최종 C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_120837_multi-router-router-reqrep-tls-copy-ctor-c3.txt`; 30/30 result lines, 6 success, 0 fail |
| Multi socket request/reply 개선 결과 (ROUTER_ROUTER_REQREP/tls) | 통과·다음 대상 진행 | baseline aggregate throughput 중앙값 `82.98%`에서 `97.95%`로 개선되어 기준을 충족했다. Release contract 11/11, sample smoke 7/7, ASan/UBSan contract 11/11도 통과했다. 다음 대상은 `MULTI_DEALER_ROUTER_REQREP / wss`다. |

### 10.3 언어 진행 상태

| 순서 | 언어 | Single 상태 | Multi 상태 | 다음 작업 |
|------|------|-------------|------------|-----------|
| 1 | C++ | 완료·통과 35 / 보류 7 | 완료·통과 25 / 보류 3 | C++ perf는 추가 실행하지 않고 .NET으로 전환 |
| 2 | .NET | 완료·통과 16 / 보류 9 | 미측정 | 다음 대상은 `Single DEALER_ROUTER_REQREP / tls`; C → .NET 순서로 한 대상씩 측정 |
| 3 | Java | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 4 | Node | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 5 | Go | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 6 | Rust | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 7 | Python | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |

## 11. 측정 기록과 결과

이 표에는 공식 판정에 사용한 최신 paired 측정만 남긴다. 각 결과는 같은 Core v0.10.1 release runtime과 동일한 조건에서 C와 해당 binding의 결과를 비교했으며, 상세한 구현·후보·프로파일 과정은 같은 디렉터리의 `log/`에 기록한다. 전체 측정표와 과거 공식 결과는 언어별 9.x 절을 기준으로 한다.

| 날짜 | 언어 | 대상 | pair tag | size별 throughput ratio | size 중앙값 | 판정 | report |
|------|------|------|----------|-------------------------|------------|------|--------|
| 2026-08-10 | C/C++ | Multi / MULTI_ROUTER_ROUTER_REQREP / ws / 64·256·1024·4096·65536·131072B | `multi-router-router-reqrep-ws-pool-bypass-ab-c1` | 99.34%, 107.52%, 98.21%, 97.91%, 98.17%, 97.49% | 98.19% | 통과·pool bypass 개선 채택 | C throughput Kops/s: 93847.8 / 84024.6 / 78344.2 / 62913.6 / 18783.4 / 12128.8; C++: 93225.6 / 90344.0 / 76939.2 / 61600.2 / 18439.0 / 11824.4. 평균 latency ratio: 0.995x / 0.929x / 1.007x / 0.998x / 0.962x / 0.942x. C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_083025_multi-router-router-reqrep-ws-pool-bypass-ab-c1.txt`; C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_083105_multi-router-router-reqrep-ws-pool-bypass-ab-c1.txt` |
| 2026-08-10 | C/C++ | Multi / MULTI_PUBSUB / ws / 64·256·1024·4096·65536·131072B | `multi-pubsub-ws-pool-bypass-c1` | 106.02%, 96.10%, 92.64%, 103.72%, 107.99%, 103.86% | 103.79% | throughput 통과·평균 latency 미달 | C throughput Kmsg/s: 1278132.2 / 1424381.2 / 1222487.6 / 471946.2 / 60787.2 / 26909.4; C++: 1355076.2 / 1368797.6 / 1132469.6 / 489501.2 / 65641.8 / 27948.6. 평균 latency ratio: 1.385x / 1.776x / 4.132x / 5.746x / 13.728x / 12.689x. C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_083223_multi-pubsub-ws-pool-bypass-c1.txt`; C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_083818_multi-pubsub-ws-pool-bypass-c1.txt` |
| 2026-08-10 | C++ | Multi / MULTI_DEALER_ROUTER_REQREP / tcp | `multi-dealer-router-reqrep-tcp-policy-c2` | 90.48% (진단), 126.87% (진단), 88.47% (진단), 152.81% (진단), 87.77% (진단), 224.51% (진단) | 108.68% (측정값 중앙값) | 모든 size가 측정값으로 판정; ratio와 six-size 중앙값을 측정값으로 기록 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_035752_multi-dealer-router-reqrep-tcp-policy-c2-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_035926_multi-dealer-router-reqrep-tcp-policy-c2-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_040102_multi-dealer-router-reqrep-tcp-policy-c2-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_040233_multi-dealer-router-reqrep-tcp-policy-c2-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_040405_multi-dealer-router-reqrep-tcp-policy-c2-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_040537_multi-dealer-router-reqrep-tcp-policy-c2-131072-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_035839_multi-dealer-router-reqrep-tcp-policy-c2-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_040013_multi-dealer-router-reqrep-tcp-policy-c2-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_040146_multi-dealer-router-reqrep-tcp-policy-c2-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_040317_multi-dealer-router-reqrep-tcp-policy-c2-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_040451_multi-dealer-router-reqrep-tcp-policy-c2-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_040620_multi-dealer-router-reqrep-tcp-policy-c2-131072-cpp.txt` |
| 2026-08-10 | C++ | Multi / MULTI_ROUTER_ROUTER_REQREP / tcp | `multi-router-router-reqrep-tcp-policy-c1` | 78.08% (진단), 126.53% (진단), 81.68% (진단), 187.46% (진단), 81.87% (진단), 243.49% (진단) | 104.20% (측정값 중앙값) | 모든 size가 측정값으로 판정; ratio와 six-size 중앙값을 측정값으로 기록 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_041221_multi-router-router-reqrep-tcp-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_041404_multi-router-router-reqrep-tcp-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_041533_multi-router-router-reqrep-tcp-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_041704_multi-router-router-reqrep-tcp-policy-c1-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_041836_multi-router-router-reqrep-tcp-policy-c1-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_042006_multi-router-router-reqrep-tcp-policy-c1-131072-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_041309_multi-router-router-reqrep-tcp-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_041448_multi-router-router-reqrep-tcp-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_041616_multi-router-router-reqrep-tcp-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_041747_multi-router-router-reqrep-tcp-policy-c1-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_041920_multi-router-router-reqrep-tcp-policy-c1-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_042050_multi-router-router-reqrep-tcp-policy-c1-131072-cpp.txt` |
| 2026-08-10 | C++ | Multi / MULTI_PUBSUB / tcp | `multi-pubsub-tcp-policy-c1` | 82.83% (진단), 105.29% (진단), 75.72% (진단), 122.91% (진단), 73.18% (진단), 121.21% (진단) | 94.06% (측정값 중앙값) | 모든 size가 측정값으로 판정; ratio와 six-size 중앙값을 측정값으로 기록 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_043500_multi-pubsub-tcp-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_043636_multi-pubsub-tcp-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_043815_multi-pubsub-tcp-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_043952_multi-pubsub-tcp-policy-c1-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_044127_multi-pubsub-tcp-policy-c1-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_044302_multi-pubsub-tcp-policy-c1-131072-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_043548_multi-pubsub-tcp-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_043726_multi-pubsub-tcp-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_043903_multi-pubsub-tcp-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_044040_multi-pubsub-tcp-policy-c1-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_044213_multi-pubsub-tcp-policy-c1-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_044349_multi-pubsub-tcp-policy-c1-131072-cpp.txt` |
| 2026-08-10 | C++ | Multi / MULTI_ROUTER_ROUTER_SENDSEND / tcp | `multi-router-router-sendsend-tcp-policy-c1` | 98.59%, 173.10% (진단), 95.43% (진단), 200.53% (진단), 84.42% (진단), 244.37% (진단) | 135.85% (측정값 중앙값) | 64B 측정 셀은 목표 통과; 256B·1024B·4096B·65536B·131072B는 측정값으로 판정 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_033220_multi-router-router-sendsend-tcp-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_033416_multi-router-router-sendsend-tcp-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_033612_multi-router-router-sendsend-tcp-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_033807_multi-router-router-sendsend-tcp-policy-c1-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_034003_multi-router-router-sendsend-tcp-policy-c1-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_034201_multi-router-router-sendsend-tcp-policy-c1-131072-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_033307_multi-router-router-sendsend-tcp-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_033500_multi-router-router-sendsend-tcp-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_033656_multi-router-router-sendsend-tcp-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_033851_multi-router-router-sendsend-tcp-policy-c1-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_034047_multi-router-router-sendsend-tcp-policy-c1-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_034245_multi-router-router-sendsend-tcp-policy-c1-131072-cpp.txt` |
| 2026-08-10 | C++ | Multi / MULTI_DEALER_ROUTER_SENDSEND / tcp | `multi-dealer-router-sendsend-tcp-policy-c1` | 97.62%, 85.62% (진단), 89.76% (진단), 93.77% (진단), 77.97% (진단), 166.10% (진단) | 91.77% (측정값 중앙값) | 64B 측정 셀은 목표 통과; 256B·1024B·4096B·65536B·131072B는 측정값으로 판정 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_031542_multi-dealer-router-sendsend-tcp-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_031715_multi-dealer-router-sendsend-tcp-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_031847_multi-dealer-router-sendsend-tcp-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_032019_multi-dealer-router-sendsend-tcp-policy-c1-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_032149_multi-dealer-router-sendsend-tcp-policy-c1-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_032318_multi-dealer-router-sendsend-tcp-policy-c1-131072-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_031626_multi-dealer-router-sendsend-tcp-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_031759_multi-dealer-router-sendsend-tcp-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_031932_multi-dealer-router-sendsend-tcp-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_032104_multi-dealer-router-sendsend-tcp-policy-c1-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_032234_multi-dealer-router-sendsend-tcp-policy-c1-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_032403_multi-dealer-router-sendsend-tcp-policy-c1-131072-cpp.txt` |
| 2026-08-10 | C++ | Multi / MULTI_DEALER_DEALER / tcp | `multi-dealer-dealer-tcp-policy-c3` | 93.44%, 76.85% (진단), 117.46% (진단), 101.63% (진단), 95.88% (진단), 144.49% (진단) | 98.76% (측정값 중앙값) | 64B 측정 셀은 개별 최소 기준 통과·목표 미달; 나머지 다섯 크기는 측정값으로 판정 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_024855_multi-dealer-dealer-tcp-policy-c3-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_025036_multi-dealer-dealer-tcp-policy-c3-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_025221_multi-dealer-dealer-tcp-policy-c3-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_025358_multi-dealer-dealer-tcp-policy-c3-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_025538_multi-dealer-dealer-tcp-policy-c3-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_025714_multi-dealer-dealer-tcp-policy-c3-131072-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_024944_multi-dealer-dealer-tcp-policy-c3-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_025123_multi-dealer-dealer-tcp-policy-c3-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_025309_multi-dealer-dealer-tcp-policy-c3-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_025445_multi-dealer-dealer-tcp-policy-c3-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_025627_multi-dealer-dealer-tcp-policy-c3-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_025806_multi-dealer-dealer-tcp-policy-c3-131072-cpp.txt` |
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
| 2026-08-09 | C++ | Single / DEALER_DEALER / inproc | `dealer-dealer-inproc-poller-parity-c1` | 89.15%, 102.63%, 100.59%, 23.90%, 64.94%, 68.80% | 78.97% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_190701_dealer-dealer-inproc-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_190940_dealer-dealer-inproc-poller-parity-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_DEALER / inproc / 65536B 재검증 | `dealer-dealer-inproc-65536-variability-c1` | 21.03% | 21.03% | 미달 (측정값) | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_191656_dealer-dealer-inproc-65536-variability-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_191733_dealer-dealer-inproc-65536-variability-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_DEALER / ipc | `dealer-dealer-ipc-poller-parity-c1` | 93.81%, 93.87%, 92.19%, 83.87%, 87.83%, 92.72% | 92.45% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_192125_dealer-dealer-ipc-poller-parity-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_192404_dealer-dealer-ipc-poller-parity-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / tcp | `dealer-router-tcp-policy-c3` | 82.99%, 98.00%, 98.29%, 85.24%, 87.99%, 94.93% | 91.46% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_195450_dealer-router-tcp-policy-c3.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_195728_dealer-router-tcp-policy-c3.txt` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / ws | `dealer-router-ws-policy-c1` | 87.85%, 89.52%, 99.61%, 93.71%, 92.05%, 97.13% | 92.88% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_200556_dealer-router-ws-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_200838_dealer-router-ws-policy-c1.txt`; 평균 latency ratio: 1.067배/1.042배/1.009배/1.075배/1.081배/1.027배; C++ throughput 변동 폭: 9.1%/14.0%/12.7%/13.9%/16.7%/4.6%; Sol review: `4b2cf27a-87c1-4143-8aaf-117b3831f102` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / wss | `dealer-router-wss-policy-c1` | 84.52%, 97.28%, 98.28%, 99.43%, 97.65%, 87.18% | 97.46% | 통과·64B 재검증 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_201807_dealer-router-wss-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_202046_dealer-router-wss-policy-c1.txt`; 평균 latency ratio: 1.174배/0.983배/1.068배/1.008배/1.023배/1.165배 |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / wss / 64B 재검증 | `dealer-router-wss-64-boundary-c1` | 90.83% | 90.83% | 통과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_202432_dealer-router-wss-64-boundary-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_202502_dealer-router-wss-64-boundary-c1.txt`; C/C++ throughput 변동 폭: 4.5%/5.7%; Sol review: `732b9b6d-0872-4b42-843c-649180d7d959` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / tls | `dealer-router-tls-policy-c1` | 77.47%, 95.96%, 96.43%, 92.65%, 93.75%, 88.58% | 93.20% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_202834_dealer-router-tls-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_203113_dealer-router-tls-policy-c1.txt`; 평균 latency ratio: 1.376배/2.301배/1.201배/1.077배/1.070배/1.132배 |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / tls 재검증 | `dealer-router-tls-policy-c2` | 73.67%, 93.26%, 91.18%, 91.09%, 89.59%, 81.33% | 90.34% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_203537_dealer-router-tls-policy-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_203816_dealer-router-tls-policy-c2.txt`; 평균 latency ratio: 1.708배/1.813배/0.607배/1.098배/1.120배/1.229배; C++ throughput 변동 폭: 47.5%/31.7%/31.0%/24.3%/16.2%/20.5%; Sol review: `8189e67a-4e7f-4e40-9557-2a71a032f174` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / inproc | `dealer-router-inproc-policy-c1` | 82.15%, 87.87%, 87.46%, 33.80%, 63.11%, 68.82% | 75.49% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_204334_dealer-router-inproc-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_204613_dealer-router-inproc-policy-c1.txt`; 평균 latency ratio: 1.229배/1.107배/1.126배/2.286배/1.538배/1.571배; C++ throughput 변동 폭: 9.7%/31.1%/18.9%/168.9%/7.4%/5.3%; Sol review: `03a9e55e-bcbf-4657-a737-066101352d32` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / inproc boundary 재검증 | `dealer-router-inproc-boundary-c1` | 85.68%, 93.39%, 92.89%, 26.72% | - | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_205007_dealer-router-inproc-boundary-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_205154_dealer-router-inproc-boundary-c1.txt`; 평균 latency ratio: 1.128배/1.059배/1.037배/2.375배; C++ throughput 변동 폭: 2.2%/14.0%/14.0%/130.3% |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / ipc | `dealer-router-ipc-policy-c1` | 76.98%, 92.83%, 98.78%, 82.71%, 84.56%, 89.69% | 87.13% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_210003_dealer-router-ipc-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_210242_dealer-router-ipc-policy-c1.txt`; 평균 latency ratio: 1.583배/1.127배/1.007배/1.198배/1.178배/1.109배; C++ throughput 변동 폭: 14.55%/11.35%/11.75%/11.22%/10.91%/4.56%; Sol review: `27ce6203-fb3c-45d8-88e6-dc59a4cca88c` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER / ipc boundary 재검증 | `dealer-router-ipc-boundary-c1` | 81.99%, 93.76%, 96.51%, 85.35%, 85.86% | 85.86% | 미달 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_210748_dealer-router-ipc-boundary-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_211002_dealer-router-ipc-boundary-c1.txt`; 평균 latency ratio: 1.226배/1.186배/1.038배/1.161배/1.159배; C/C++ throughput 변동 폭: 11.38%/10.64%/12.90%/7.68%/13.93%/10.14%/9.47%/11.35%/6.91%/5.32%; Sol review: `27ce6203-fb3c-45d8-88e6-dc59a4cca88c` |

| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / tcp full sweep | `dealer-router-reqrep-tcp-policy-c2` | 95.10%, 93.51%, 96.72%, 94.99%, 95.76%, 93.89% | 95.05% | boundary 재검증 전 임시 결과 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_211628_dealer-router-reqrep-tcp-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_212518_dealer-router-reqrep-tcp-policy-c2.txt` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / tcp / 1024B boundary | `dealer-router-reqrep-tcp-boundary-1024-c1` | 94.64% | 94.64% | 미달·재계산 대상 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_213036_dealer-router-reqrep-tcp-boundary-1024-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_213108_dealer-router-reqrep-tcp-boundary-1024-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / tcp / 65536B boundary | `dealer-router-reqrep-tcp-boundary-65536-c1` | 95.73% | 95.73% | 통과·재계산 대상 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_213143_dealer-router-reqrep-tcp-boundary-65536-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_213219_dealer-router-reqrep-tcp-boundary-65536-c1.txt` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / tcp 최종 | `dealer-router-reqrep-tcp-policy-final` | 95.10%, 93.51%, 94.64%, 95.73%, 95.76%, 93.89% | 94.87% | 미달·no-go | latency ratio 1.022배/1.057배/0.964배/1.041배/1.036배/1.047배. boundary 재검증 후 공식 중앙값이 95%에 미달하고 binding-only hot-path 근거가 없어 개선하지 않는다. |

| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / ws / 64·256·65536·131072·262144B 공식 cell | `dealer-router-reqrep-ws-policy-final-c3` | 96.25%, 92.11%, 97.01%, 98.97%, 94.82% | 96.25% (5개 cell 측정값 중앙값) | 5개 cell 채택·측정값으로 판정 | C/C++ official reports: 64B `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_220013_dealer-router-reqrep-ws-parity-64-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_220043_dealer-router-reqrep-ws-parity-64-c2.txt`; 256B `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_221848_dealer-router-reqrep-ws-boundary-256-c3-repeat.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_221920_dealer-router-reqrep-ws-boundary-256-c3-repeat.txt`; 65536B `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_220115_dealer-router-reqrep-ws-parity-65536-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_220146_dealer-router-reqrep-ws-parity-65536-c2.txt`; 131072B `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_221103_dealer-router-reqrep-ws-boundary-131072-c3.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_221134_dealer-router-reqrep-ws-boundary-131072-c3.txt`; 262144B `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_221309_dealer-router-reqrep-ws-boundary-262144-c3.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_221341_dealer-router-reqrep-ws-boundary-262144-c3.txt`; Sol review: `SOL-REQREP-WS-C3-FINAL-20260809` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / ws / 1024B 진단 cell | `dealer-router-reqrep-ws-boundary-1024-c3-repeat` | 86.03% (진단값) | 86.03% | 통과 (86.03%) | C/C++ report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_221953_dealer-router-reqrep-ws-boundary-1024-c3-repeat.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_222026_dealer-router-reqrep-ws-boundary-1024-c3-repeat.txt`; C/C++ throughput 변동 폭 20.77%/18.72%, 하향 drift; Sol review: `SOL-REQREP-WS-C3-FINAL-20260809` |

| 2026-08-10 | C/C++ | Single / DEALER_ROUTER_REQREP / wss / 64·256·1024·65536·131072·262144B 최종 paired | `dealer-router-reqrep-wss-policy-final-c3` | 78.73%, 91.66%, 118.48%, 100.56%, 94.69%, 92.82% | 93.76% | 통과 | C median Kops/s: 169.9074 / 133.8474 / 56.7648 / 3.9784 / 2.7550 / 1.4842; C++ median Kops/s: 133.7758 / 122.6814 / 67.2556 / 4.0006 / 2.6088 / 1.3776. latency ratio: 1.216x / 1.080x / 0.843x / 0.994x / 1.055x / 1.083x, 중앙값 1.067x. C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_223651_dealer-router-reqrep-wss-boundary-64-c2.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_223544_dealer-router-reqrep-wss-boundary-256-c2.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_223757_dealer-router-reqrep-wss-boundary-1024-c2.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_223903_dealer-router-reqrep-wss-boundary-65536-c2.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_122459_dealer-router-reqrep-wss-boundary-131072-c3.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_124208_dealer-router-reqrep-wss-boundary-262144-c4.txt`; C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_223724_dealer-router-reqrep-wss-boundary-64-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_223617_dealer-router-reqrep-wss-boundary-256-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_223828_dealer-router-reqrep-wss-boundary-1024-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_223935_dealer-router-reqrep-wss-boundary-65536-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_124133_dealer-router-reqrep-wss-boundary-131072-cpp5.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_124246_dealer-router-reqrep-wss-boundary-262144-cpp5.txt`; harness fix commit `c15988103d` |

| 2026-08-10 | C/C++ | Single / DEALER_ROUTER_REQREP / tls / 64·256·1024·65536·131072·262144B 최종 paired | `dealer-router-reqrep-tls-policy-final-c3` | 100.58%, 93.13%, 80.31%, 110.56%, 98.26%, 99.62% | 98.94% | 통과 | latency ratio 0.954x / 1.058x / 1.228x / 0.905x / 1.022x / 1.020x, 중앙값 1.021x. C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_125057_dealer-router-reqrep-tls-boundary-64-c3.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_224926_dealer-router-reqrep-tls-boundary-256-c2.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_225036_dealer-router-reqrep-tls-boundary-1024-c2.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_225141_dealer-router-reqrep-tls-boundary-65536-c2.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_125201_dealer-router-reqrep-tls-boundary-131072-c3.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_125305_dealer-router-reqrep-tls-boundary-262144-c3.txt`; C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_125128_dealer-router-reqrep-tls-boundary-64-cpp3.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_224959_dealer-router-reqrep-tls-boundary-256-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_225107_dealer-router-reqrep-tls-boundary-1024-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_225213_dealer-router-reqrep-tls-boundary-65536-c2.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_125233_dealer-router-reqrep-tls-boundary-131072-cpp3.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_125342_dealer-router-reqrep-tls-boundary-262144-cpp3.txt` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / inproc full sweep | `dealer-router-reqrep-inproc-policy-c1` | 94.86%, 93.67%, 93.65%, 30.48%, 97.49%, 97.51% (진단값) | 94.27% (측정값 중앙값) | 미달 (30.48%) | C/C++ throughput variation: 19.36%/20.93%, 19.43%/21.66%, 22.14%/17.80%, 16.28%/14.80%, 14.43%/27.72%, 19.70%/17.58%; C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_225548_dealer-router-reqrep-inproc-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_230054_dealer-router-reqrep-inproc-policy-c1.txt`; Sol review: `SOL-REQREP-INPROC-C1-20260809` |
| 2026-08-09 | C++ | Single / DEALER_ROUTER_REQREP / inproc / 65536B boundary | `dealer-router-reqrep-inproc-boundary-65536-c2` | 31.00% (진단값) | 31.00% (측정값 중앙값) | 미달 (31.00%) | C median 124.6578 Kops/s, C variation 7.09%; C++ median 38.6542 Kops/s, C++ variation 26.68%; latency ratio 약 3.70배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_230533_dealer-router-reqrep-inproc-boundary-65536-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_230610_dealer-router-reqrep-inproc-boundary-65536-c2.txt`; Sol review: `SOL-REQREP-INPROC-C2-FINAL-20260809` |
| 2026-08-09 | C++ | Single / ROUTER_ROUTER / tcp / 64·256·1024·65536·131072·262144B | `router-router-tcp-policy-c3` | 88.71%, 92.33%, 95.11%, 81.23%, 88.27%, 92.64% (진단값) | 90.52% (측정값 중앙값) | 통과 (90.52%) | C/C++ throughput variation은 six-size 모두 10% 초과. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_232354_router-router-tcp-policy-c3.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_232641_router-router-tcp-policy-c3.txt`; Sol review: `f51e6b95-c5a1-478d-9cb8-5b0e8d0e390d` |
| 2026-08-09 | C++ | Single / ROUTER_ROUTER / tcp / 262144B boundary | `router-router-tcp-boundary-262144-c4` | 90.94% (진단값) | 90.94% (측정값 중앙값) | 통과 (90.94%) | C median 14,674.6 Kmsg/s, C variation 3.75%; C++ median 13,345.4 Kmsg/s, C++ variation 15.73%. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_232946_router-router-tcp-boundary-262144-c4.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_233019_router-router-tcp-boundary-262144-c4.txt` |
| 2026-08-09 | C++ | Single / ROUTER_ROUTER / tcp / 64B boundary | `router-router-tcp-boundary-64-c4` | 120.71% (진단값) | 120.71% (측정값 중앙값) | 통과 (120.71%) | C median 1,436,443.0 Kmsg/s, C variation 약 26.5%; C++ median 1,733,951.0 Kmsg/s, C++ variation 약 19.1%. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_233054_router-router-tcp-boundary-64-c4.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_233128_router-router-tcp-boundary-64-c4.txt` |
| 2026-08-09 | C++ | Single / ROUTER_ROUTER_REQREP / tcp / 64·256·1024·65536·131072·262144B | `router-router-reqrep-tcp-policy-c1` | 84.17%, 87.46%, 87.21%, 90.92%, 98.71%, 100.74% (진단값) | 89.19% (측정값 중앙값) | 통과 (89.19%) | C median 213.0778/187.5958/171.1564/16.4114/10.7340/6.7358, C++ median 179.3480/164.0730/149.2678/14.9214/10.5950/6.7858 Kops/s. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_234907_router-router-reqrep-tcp-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_235148_router-router-reqrep-tcp-policy-c1.txt`; full sweep C/C++ throughput variation은 모든 size에서 10% 초과다. |
| 2026-08-09 | C++ | Single / ROUTER_ROUTER_REQREP / tcp / 64B boundary | `router-router-reqrep-tcp-boundary-64-c2` | 87.63% (진단값) | 87.63% (측정값 중앙값) | 통과 (87.63%) | C median 214.2398 Kops/s, C variation 6.23%; C++ median 187.748 Kops/s, C++ variation 27.40%; C++ raw throughput 205.96/204.86/187.75/158.74/154.52 Kops/s, C++/C latency ratio 1.117배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260809_235550_router-router-reqrep-tcp-boundary-64-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260809_235622_router-router-reqrep-tcp-boundary-64-c2.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / ws / 64·256·1024·65536·131072·262144B | `router-router-reqrep-ws-policy-c1` | 83.20%, 86.38%, 89.13%, 97.58%, 88.25%, 91.51% (진단값) | 88.69% (측정값 중앙값) | 통과 (88.69%) | C median 183.8204/144.3586/87.6194/9.9400/7.3188/4.5578, C++ median 152.9452/124.6924/78.0936/9.6996/6.4586/4.1708 Kops/s. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_000325_router-router-reqrep-ws-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_000608_router-router-reqrep-ws-policy-c1.txt`; C/C++ throughput variation은 64B부터 262144B까지 모두 10% 초과다. |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / ws / 64B boundary | `router-router-reqrep-ws-boundary-64-c2` | 89.67% (진단값) | 89.67% (측정값 중앙값) | 통과 (89.67%) | C median 187.4034 Kops/s, C variation 6.71%; C++ median 168.0366 Kops/s, C++ variation 30.00%; C++ raw throughput 169.72/170.84/168.04/139.37/120.42 Kops/s, C++/C latency ratio 1.075배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_001032_router-router-reqrep-ws-boundary-64-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_001105_router-router-reqrep-ws-boundary-64-c2.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / wss / 64·256·1024·65536·131072·262144B | `router-router-reqrep-wss-policy-c1` | 84.49%, 84.38%, 87.27%, 98.30%, 92.55%, 96.73% (진단값) | 89.91% (측정값 중앙값) | 통과 (89.91%) | C median 172.3856/130.8440/67.6952/4.0140/2.4570/1.3752, C++ median 145.6416/110.4048/59.0774/3.9456/2.2740/1.3302 Kops/s. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_001452_router-router-reqrep-wss-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_001742_router-router-reqrep-wss-policy-c1.txt`; C/C++ throughput variation은 모든 size에서 10% 초과다. |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / wss / 256B boundary | `router-router-reqrep-wss-boundary-256-c2` | 92.19% | 92.19% | 개별 측정 통과·transport 중앙값 평가 | C median 127.0056 Kops/s, C variation 6.83%; C++ median 117.0892 Kops/s, C++ variation 4.71%; latency ratio 1.069배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_002121_router-router-reqrep-wss-boundary-256-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_002155_router-router-reqrep-wss-boundary-256-c2.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / wss / 64B boundary | `router-router-reqrep-wss-boundary-64-c2` | 73.43% (진단값) | 73.43% (측정값 중앙값) | 미달 (73.43%) | C median 156.7954 Kops/s, C variation 21.15%; C++ median 115.1320 Kops/s, C++ variation 23.57%; C++ raw throughput 138.36/111.22/115.13/117.08/111.42 Kops/s, C++/C latency ratio 1.330배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_002233_router-router-reqrep-wss-boundary-64-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_002311_router-router-reqrep-wss-boundary-64-c2.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / tls / 64·256·1024·65536·131072·262144B | `router-router-reqrep-tls-policy-c1` | 87.54%, 87.80%, 94.59%, 91.39%, 88.85%, 97.32% (진단값) | 90.12% (측정값 중앙값) | 통과 (90.12%) | C median 186.3206/166.8908/115.9280/5.5954/3.3372/1.8296, C++ median 163.1066/146.5266/109.6520/5.1136/2.9652/1.7806 Kops/s. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_002550_router-router-reqrep-tls-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_002829_router-router-reqrep-tls-policy-c1.txt`; C/C++ throughput variation은 262144B를 제외한 모든 size에서 10% 초과하고 262144B도 C++ variation 14.04%다. |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / tls / 64B boundary | `router-router-reqrep-tls-boundary-64-c2` | 98.00% | 98.00% | 개별 측정 통과·transport 중앙값 평가 | C median 180.1038 Kops/s, C variation 1.59%; C++ median 176.4990 Kops/s, C++ variation 4.78%; latency ratio 1.000배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_003150_router-router-reqrep-tls-boundary-64-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_003223_router-router-reqrep-tls-boundary-64-c2.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / tls / 256B boundary | `router-router-reqrep-tls-boundary-256-c2` | 88.33% (진단값) | 88.33% (측정값 중앙값) | 통과 (88.33%) | C median 126.9036 Kops/s, C variation 19.03%; C++ median 112.0942 Kops/s, C++ variation 22.08%; C++ raw throughput 111.48/100.17/112.09/113.21/124.92 Kops/s, C++/C latency ratio 1.126배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_003306_router-router-reqrep-tls-boundary-256-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_003340_router-router-reqrep-tls-boundary-256-c2.txt` |

| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / inproc / 64·256·1024·65536·131072·262144B | `router-router-reqrep-inproc-policy-c1` | 81.73%, 87.19%, 87.08%, 34.92%, 92.40%, 87.45% (진단값) | 87.14% (측정값 중앙값) | 미달 (34.92%) | C median 270.2672/271.9840/254.9178/108.1982/69.1480/41.3462, C++ median 220.8716/237.1504/221.9798/37.7862/63.8894/36.0592 Kops/s. C/C++ throughput variation은 19.83%/20.83%, 21.08%/23.85%, 18.59%/13.82%, 15.17%/25.12%, 19.19%/11.67%, 18.00%/19.47%다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_003627_router-router-reqrep-inproc-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_003906_router-router-reqrep-inproc-policy-c1.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / inproc / 65536B boundary | `router-router-reqrep-inproc-boundary-65536-c2` | 35.06% (진단값) | 35.06% (측정값 중앙값) | 미달 (35.06%) | C median 111.55 Kops/s, C variation 5.84%; C++ median 39.11 Kops/s, C++ variation 39.86%; C++ raw throughput 36.53/39.11/52.12/38.93/44.84 Kops/s. C/C++ latency mean은 0.060/0.193ms로 diagnostic latency ratio는 3.217배다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_004236_router-router-reqrep-inproc-boundary-65536-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_004500_router-router-reqrep-inproc-boundary-65536-c2.txt` |

| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / ipc / 64·256·1024·65536·131072·262144B | `router-router-reqrep-ipc-policy-c1` | 87.77%, 89.40%, 81.98%, 90.53%, 90.03%, 99.70% (진단값) | 89.72% (측정값 중앙값) | 통과 (89.72%) | C median 222.7546/190.4732/184.5198/18.1786/12.6214/7.2300, C++ median 195.5072/170.2774/151.2780/16.4578/11.3636/7.2084 Kops/s. C/C++ variation은 19.43%/15.54%, 24.20%/16.94%, 21.45%/14.17%, 25.08%/18.17%, 16.24%/17.52%, 17.43%/19.00%로 full sweep 모든 셀이 변동 폭을 참고 정보로 기록했다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_004920_router-router-reqrep-ipc-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_005200_router-router-reqrep-ipc-policy-c1.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / ipc / 1024B boundary | `router-router-reqrep-ipc-boundary-1024-c2` | 90.23% | 90.23% | 개별 측정 통과·transport 중앙값 평가 | C median 191.5740 Kops/s, C variation 4.10%; C++ median 172.8540 Kops/s, C++ variation 7.18%; C++ raw throughput 161.48/172.85/173.88/173.15/171.50 Kops/s; latency ratio 1.017배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_005555_router-router-reqrep-ipc-boundary-1024-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_005628_router-router-reqrep-ipc-boundary-1024-c2.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER_REQREP / ipc / 64B boundary | `router-router-reqrep-ipc-boundary-64-c3` | 73.62% (진단값) | 73.62% (측정값 중앙값) | 미달 (73.62%) | C median 213.7766 Kops/s, C variation 23.04%; C++ median 157.3782 Kops/s, C++ variation 33.52%; C++ raw throughput 202.14/150.75/157.38/149.39/170.37 Kops/s; latency ratio 약 1.270배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_005707_router-router-reqrep-ipc-boundary-64-c3.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_005747_router-router-reqrep-ipc-boundary-64-c3.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER / ws / 64·256·1024·65536·131072·262144B | `router-router-ws-policy-c1` | 90.15%, 84.72%, 95.14%, 91.42%, 90.23%, 96.58% (진단값) | 90.83% (측정값 중앙값) | 통과 (90.83%) | C median 1433.4012/950.0524/387.1972/20.9188/14.4162/8.2212, C++ median 1292.1568/804.8760/368.3756/19.1250/13.0080/7.9400 Kmsg/s. C/C++ variation은 25.67%/20.80%, 22.74%/17.92%, 21.83%/20.66%, 16.16%/20.35%, 12.21%/14.22%, 13.75%/16.12%다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_010214_router-router-ws-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_010456_router-router-ws-policy-c1.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER / ws / 256B boundary | `router-router-ws-boundary-256-c2` | 94.93% | 94.93% | 개별 측정 통과·transport 중앙값 평가 | C median 935.4714 Kmsg/s, C variation 4.48%; C++ median 888.0406 Kmsg/s, C++ variation 5.56%; latency ratio 약 1.077배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_010825_router-router-ws-boundary-256-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_010859_router-router-ws-boundary-256-c2.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER / ws / 64B boundary | `router-router-ws-boundary-64-c3` | 80.35% (진단값) | 80.35% (측정값 중앙값) | 통과 (80.35%) | C median 1378.9038 Kmsg/s, C variation 18.22%; C++ median 1107.9908 Kmsg/s, C++ variation 1.93%; C++ raw throughput 1106.85/1103.73/1107.99/1125.06/1115.32 Kmsg/s; latency ratio 약 1.160배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_010935_router-router-ws-boundary-64-c3.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_011011_router-router-ws-boundary-64-c3.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER / wss / 64·256·1024·65536·131072·262144B | `router-router-wss-policy-c1` | 81.80%, 86.34%, 96.54%, 98.82%, 96.17%, 92.97% (진단값) | 94.57% (측정값 중앙값) | 통과 (94.57%) | C median 1531.72/685.28/214.47/8.76/5.35/2.92, C++ median 1252.91/591.70/207.06/8.65/5.15/2.71 Kmsg/s. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_011328_router-router-wss-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_011609_router-router-wss-policy-c1.txt`; full sweep C/C++ throughput variation은 모든 size에서 10% 초과다. |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER / wss / 64B boundary | `router-router-wss-boundary-64-c2` | 90.61% | 90.61% | 개별 측정 통과·transport 중앙값 평가 | C median 1539.95 Kmsg/s, C variation 7.30%; C++ median 1395.40 Kmsg/s, C++ variation 5.97%; C++/C latency ratio 약 0.958배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_011937_router-router-wss-boundary-64-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_012012_router-router-wss-boundary-64-c2.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER / wss / 256B boundary | `router-router-wss-boundary-256-c2` | 83.44% (진단값) | 83.44% (측정값 중앙값) | 통과 (83.44%) | C median 644.76 Kmsg/s, C variation 10.30%; C++ median 538.00 Kmsg/s, C++ variation 21.26%; C++/C latency ratio 약 1.086배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_012048_router-router-wss-boundary-256-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_012125_router-router-wss-boundary-256-c2.txt` |

| 2026-08-10 | C++ | Single / ROUTER_ROUTER / tls / 64·256·1024·65536·131072·262144B | `router-router-tls-policy-c1` | 90.83%, 82.44%, 93.04%, 95.13%, 92.84%, 82.21% (진단값) | 91.84% (측정값 중앙값) | valid performance fail·측정값 기록 | C median 1595.69/866.12/292.02/11.51/6.84/3.71, C++ median 1449.42/714.04/271.69/10.96/6.35/3.05 Kmsg/s. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_012756_router-router-tls-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_013038_router-router-tls-policy-c1.txt`; full sweep C/C++ throughput variation은 모든 size에서 10% 초과다. |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER / tls / 262144B boundary | `router-router-tls-boundary-262144-c2` | 약 83.92% | 약 83.92% | 측정값으로 fail | C median 3815.2 Kmsg/s, C variation 약 5.5%; C++ median 3201.6 Kmsg/s, C++ variation 약 9.69%; C++/C latency ratio 약 1.195배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_013510_router-router-tls-boundary-262144-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_013548_router-router-tls-boundary-262144-c2.txt`; Sol review: `SOL-RR-TLS-C2-FINAL-20260810` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER / inproc / 64·256·1024·65536·131072·262144B | `router-router-inproc-policy-c1` | 약 90.24%, 92.39%, 83.22%, 19.63%, 57.02%, 64.64% (진단값) | 약 73.93% (측정값 중앙값) | valid performance fail·측정값 기록 | C median 2266.924/1721.010/1621.674/417.602/158.306/75.897, C++ median 2045.758/1590.123/1349.495/81.969/90.263/49.048 Kmsg/s. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_014138_router-router-inproc-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_014421_router-router-inproc-policy-c1.txt`; full sweep C/C++ paired variation은 모든 size에서 10% 초과다. |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER / inproc / 262144B boundary | `router-router-inproc-boundary-262144-c2` | 약 63.97% | 약 63.97% | 측정값으로 fail | C median 77078.8 Kmsg/s, C variation 약 6.1%; C++ median 49305.0 Kmsg/s, C++ variation 약 5.7%; C++/C latency ratio 약 1.59배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_014804_router-router-inproc-boundary-262144-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_014837_router-router-inproc-boundary-262144-c2.txt`; Sol review: `SOL-RR-INPROC-C2-FINAL-20260810` |

| 2026-08-10 | C++ | Single / ROUTER_ROUTER / ipc / 64·256·1024·65536·131072·262144B | `router-router-ipc-policy-c1` | 91.89%, 84.44%, 100.00%, 85.54%, 90.76%, 92.00% (진단값) | 91.33% (측정값 중앙값) | 통과 (91.33%) | C median 1917.0196/1174.1862/626.3310/35.5734/23.0220/14.1090, C++ median 1761.4946/991.4742/626.3044/30.4296/20.8950/12.9810 Kmsg/s. C/C++ variation은 약 32.70%/23.30%, 25.70%/16.44%, 17.46%/23.23%, 14.96%/24.48%, 15.38%/16.18%, 20.70%/24.04%다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_015147_router-router-ipc-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_015429_router-router-ipc-policy-c1.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER / ipc / 256B boundary | `router-router-ipc-boundary-256-c2` | 94.75% | 94.75% | 개별 측정 통과·transport 중앙값 평가 | C median 1139.5244 Kmsg/s, C variation 약 6.8%; C++ median 1079.6506 Kmsg/s, C++ variation 약 5.0%; 평균 latency ratio 약 0.93배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_015748_router-router-ipc-boundary-256-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_015824_router-router-ipc-boundary-256-c2.txt` |
| 2026-08-10 | C++ | Single / ROUTER_ROUTER / ipc / 65536B boundary | `router-router-ipc-boundary-65536-c3` | 107.68% (진단값) | 107.68% (측정값 중앙값) | 통과 (107.68%) | C median 31.9286 Kmsg/s, C variation 약 23.9%; C++ median 34.3834 Kmsg/s, C++ variation 약 8.6%. C 기준 변동성이 변동 폭을 참고 정보로 기록하므로 107.68%는 측정값으로 기록한다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_015920_router-router-ipc-boundary-65536-c3.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_020150_router-router-ipc-boundary-65536-c3.txt`; Sol review: `f31ee5a3-9c18-4dfe-b39b-b4e86e87604b` |

| 2026-08-10 | C++ | Single / DEALER_ROUTER_REQREP / ipc / 64·256·1024·65536·131072·262144B | `dealer-router-reqrep-ipc-policy-c1` | 85.25%, 88.77%, 86.33%, 91.07%, 94.05%, 91.75% (진단값) | 89.92% (측정값 중앙값) | 통과 (89.92%) | C median 233.6114/200.5530/192.8800/17.6368/12.1044/7.7076, C++ median 199.1422/178.0332/166.5126/16.0624/11.3836/7.0716 Kops/s. C/C++ variation은 약 20.73%/19.34%, 22.36%/21.72%, 20.18%/23.89%, 23.19%/26.90%, 21.49%/15.99%, 16.08%/23.90%다. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_020946_dealer-router-reqrep-ipc-policy-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_021230_dealer-router-reqrep-ipc-policy-c1.txt` |
| 2026-08-10 | C++ | Single / DEALER_ROUTER_REQREP / ipc / 64B boundary | `dealer-router-reqrep-ipc-boundary-64-c2` | 94.38% (진단값) | 94.38% (측정값 중앙값) | 통과 (94.38%) | C median 226.573 Kops/s, C variation 3.87%; C++ median 213.835 Kops/s, C++ variation 11.56%; C++ raw throughput 209.67/213.84/216.39/215.76/191.66 Kops/s; latency ratio 약 1.03배. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_021600_dealer-router-reqrep-ipc-boundary-64-c2.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260810_021634_dealer-router-reqrep-ipc-boundary-64-c2.txt`; Sol review: `SOL-DR-REQREP-IPC-C2-FINAL-20260810` |

| 2026-08-10 | C++ | Multi / MULTI_STREAM / tcp / 64·256·1024·65536B | `multi-stream-tcp-policy-c3` | 61.55% (진단), 107.40% (진단), 126.61% (진단), 144.53% (진단) | 117.00% (측정값 중앙값) | 네 size 모두 측정값으로 판정·ratio와 size 중앙값을 측정값으로 기록·binding-only source optimization no-go | C/C++ variation은 각각 20.38%/19.80%, 58.64%/20.31%, 49.14%/17.78%, 28.79%/37.00%다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_050327_multi-stream-tcp-policy-c3-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_050536_multi-stream-tcp-policy-c3-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_050754_multi-stream-tcp-policy-c3-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_050959_multi-stream-tcp-policy-c3-65536-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_050429_multi-stream-tcp-policy-c3-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_050640_multi-stream-tcp-policy-c3-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_050857_multi-stream-tcp-policy-c3-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_051238_multi-stream-tcp-policy-c3-65536-cpp.txt`; Sol review: `SOL-MULTI-STREAM-TCP-FINAL-20260810`. |

| 2026-08-10 | C++ | Multi / MULTI_STREAM / ws / 64·256·1024·65536B | `multi-stream-ws-policy-c1` | 78.48% (진단), 102.55% (진단), 150.72% (진단), 163.32% (진단) | 126.63% (측정값 중앙값) | 네 size 모두 측정값으로 판정·ratio와 size 중앙값을 측정값으로 기록·binding-only source optimization no-go | C/C++ variation은 각각 27.68%/29.49%, 42.97%/18.93%, 30.25%/19.27%, 33.03%/53.38%다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_052020_multi-stream-ws-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_052224_multi-stream-ws-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_052422_multi-stream-ws-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_052625_multi-stream-ws-policy-c1-65536-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_052118_multi-stream-ws-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_052323_multi-stream-ws-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_052525_multi-stream-ws-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_052734_multi-stream-ws-policy-c1-65536-cpp.txt`; Sol review: `SOL-MULTI-STREAM-WS-FINAL-20260810`. |

| 2026-08-10 | C++ | Multi / MULTI_STREAM / wss / 64·256·1024·65536B | `multi-stream-wss-policy-c1` | 106.75% (진단), 95.91% (진단), 107.44% (진단), 118.32% (진단) | 107.09% (측정값 중앙값) | 네 size 모두 측정값으로 판정·ratio와 size 중앙값을 측정값으로 기록·binding-only source optimization no-go | C/C++ variation은 각각 68.34%/30.11%, 47.78%/19.89%, 36.73%/30.38%, 52.23%/45.94%다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_053203_multi-stream-wss-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_053649_multi-stream-wss-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_054139_multi-stream-wss-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_054632_multi-stream-wss-policy-c1-65536-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_053423_multi-stream-wss-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_053913_multi-stream-wss-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_054405_multi-stream-wss-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_054922_multi-stream-wss-policy-c1-65536-cpp.txt`; Sol review: `SOL-MULTI-STREAM-WSS-FINAL-20260810`. |
| 2026-08-10 | C++ | Multi / MULTI_STREAM / tls / 64·256·1024·65536B | `multi-stream-tls-policy-c1` | 83.99% (진단), 78.29% (진단), 102.04% (진단), 111.10% (진단) | 93.02% (측정값 중앙값) | 네 size 모두 측정값으로 판정·ratio와 size 중앙값을 측정값으로 기록·binding-only source optimization no-go | C/C++ variation은 각각 59.27%/45.79%, 40.20%/48.63%, 30.95%/59.48%, 61.55%/64.68%다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_055446_multi-stream-tls-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_055920_multi-stream-tls-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_060358_multi-stream-tls-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_061033_multi-stream-tls-policy-c1-65536-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_055701_multi-stream-tls-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_060138_multi-stream-tls-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_060819_multi-stream-tls-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_061300_multi-stream-tls-policy-c1-65536-cpp.txt`; Sol review: `SOL-MULTI-STREAM-TLS-FINAL-20260810` |
| 2026-08-10 | C++ | Multi / MULTI_DEALER_DEALER / ws / 64·256·1024·4096·65536·131072B | `multi-dealer-dealer-ws-policy-c1` | 91.72% (64B 측정), 121.32% (진단), 77.88% (진단), 159.28% (진단), 77.86% (진단), 186.15% (진단) | 121.32% (측정값 중앙값) | 64B 개별 최소 기준 통과·95% 목표 미달; 나머지 다섯 크기와 전체 transport는 측정값으로 판정 | C/C++ median Kmsg/s는 2223.164/2039.126, 960.194/1164.939, 820.053/638.683, 242.223/385.819, 51.980/40.472, 15.389/28.646이다. C/C++ variation은 각각 9.98%/5.45%, 34.93%/20.80%, 13.97%/41.86%, 32.83%/18.82%, 31.04%/30.59%, 86.43%/10.92%다. C reports: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_062408_multi-dealer-dealer-ws-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_062545_multi-dealer-dealer-ws-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_062719_multi-dealer-dealer-ws-policy-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_062854_multi-dealer-dealer-ws-policy-c1-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_063028_multi-dealer-dealer-ws-policy-c1-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_063206_multi-dealer-dealer-ws-policy-c1-131072-c.txt`; C++ reports: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_062455_multi-dealer-dealer-ws-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_062632_multi-dealer-dealer-ws-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_062806_multi-dealer-dealer-ws-policy-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_062941_multi-dealer-dealer-ws-policy-c1-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_063117_multi-dealer-dealer-ws-policy-c1-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_063257_multi-dealer-dealer-ws-policy-c1-131072-cpp.txt`; parity commit: `870dad23c0`; Sol review: `SOL-MULTI-DD-WS-C1-20260810` |

| 2026-08-10 | C++ | Multi / MULTI_DEALER_ROUTER_SENDSEND / ws / 64·256·1024·4096·65536·131072B | `multi-dealer-router-sendsend-ws-policy-c1` + `multi-dealer-router-sendsend-ws-explore-c1` | 99.99%, 91.43%, 97.09%, 121.03%, 101.66%, 99.55% | 99.77% | 통과·다음 대상 진행 | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_065005_multi-dealer-router-sendsend-ws-policy-c1-64-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_065136_multi-dealer-router-sendsend-ws-policy-c1-256-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_065706_multi-dealer-router-sendsend-ws-explore-c1-1024-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_065736_multi-dealer-router-sendsend-ws-explore-c1-4096-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_070104_multi-dealer-router-sendsend-ws-explore-c1-65536-c.txt`, `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_070127_multi-dealer-router-sendsend-ws-explore-c1-131072-c.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_065050_multi-dealer-router-sendsend-ws-policy-c1-64-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_065221_multi-dealer-router-sendsend-ws-policy-c1-256-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_065719_multi-dealer-router-sendsend-ws-explore-c1-1024-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_070051_multi-dealer-router-sendsend-ws-explore-c1-4096-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_070114_multi-dealer-router-sendsend-ws-explore-c1-65536-cpp.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_070136_multi-dealer-router-sendsend-ws-explore-c1-131072-cpp.txt` |

| 2026-08-10 | C++ | Multi / MULTI_ROUTER_ROUTER_SENDSEND / ws / 64·256·1024·4096·65536·131072B | `multi-router-router-sendsend-ws-explore-c1` | 89.92%, 114.02%, 99.79%, 97.37%, 94.35%, 96.04% | 96.71% | 통과·binding-only 개선 no-go·다음 대상 진행 | C/C++ reports are recorded in 9.1.2; all 12 reports are `status: complete`, with Core v0.10.1 release runtime and the same paired conditions. |
| 2026-08-10 | C++ | Multi / MULTI_DEALER_ROUTER_REQREP / ws / 64·256·1024·4096·65536·131072B | `multi-dealer-router-reqrep-ws-parity-pool-c1` | 95.39%, 99.19%, 96.94%, 97.48%, 95.52%, 103.98% | 97.21% | 후보 측정·공식 판정에서 제외 | C harness의 READY 전 size별 auto-HWM과 C++ reply backpressure 재시도는 유지한다. request callback state pool은 Sol review에서 cleanup·thread migration·`noexcept` allocation 위험과 pool 효과 미분리 문제를 확인해 철회했으며, 이 후보 결과는 최종 판정에 사용하지 않는다. |
| 2026-08-10 | C++ | Multi / MULTI_DEALER_ROUTER_REQREP / ws / 64·256·1024·4096·65536·131072B | `multi-dealer-router-reqrep-ws-parity-rerun-c1` | 87.08%, 90.26%, 94.28%, 76.11%, 77.87%, 84.78% | 85.93% | 개선 검토 후 통과·다음 대상 진행 | C throughput은 102884.8 / 94665.0 / 89514.6 / 78708.4 / 22513.2 / 14621.6 Kops/s, C++ throughput은 89592.6 / 85447.8 / 84391.0 / 59901.2 / 17530.4 / 12396.0 Kops/s다. 평균 latency ratio는 1.124x, 1.092x, 1.044x, 1.285x, 1.219x, 1.461x로 모두 2.0x 이내다. socket request/reply C++ 기준인 개별 75%와 중앙값 85%를 측정값으로 충족했다. |

| 2026-08-10 | C/C++ | Multi / MULTI_DEALER_ROUTER_REQREP / ws / 64·256·1024·4096·65536·131072B | `multi-dealer-router-reqrep-ws-parity-rerun-c1` | 87.08%, 90.26%, 94.28%, 76.11%, 77.87%, 84.78% | 85.93% | 통과·개선 검토 완료·다음 대상 진행 | C median Kops/s: 102884.8 / 94665.0 / 89514.6 / 78708.4 / 22513.2 / 14621.6. C++ median Kops/s: 89592.6 / 85447.8 / 84391.0 / 59901.2 / 17530.4 / 12396.0. 평균 latency ratio: 1.124x / 1.092x / 1.044x / 1.285x / 1.219x / 1.461x. C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_074457_multi-dealer-router-reqrep-ws-parity-rerun-c1.txt`; C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_074538_multi-dealer-router-reqrep-ws-parity-rerun-c1.txt`; Core provenance: `/home/hep7hep7/.cache/zlink/core/0.10.1/linux-x64/share/zlink/core-package-provenance.json`. |

| 2026-08-10 | C/C++ | Multi / MULTI_ROUTER_ROUTER_SENDSEND / wss / 64·256·1024·4096·65536·131072B | `multi-router-router-sendsend-wss-after-contract-audit-c1` | 104.09%, 101.35%, 110.36%, 100.40%, 111.91%, 123.69% | 107.22% | 통과·binding-only 추가 변경 없음·다음 대상 진행 | C throughput Kops/s: 136.292 / 117.405 / 112.902 / 88.890 / 9.807 / 4.800; C++: 141.861 / 118.992 / 124.602 / 89.242 / 10.975 / 5.937. 평균 latency ratio: 0.960x / 0.985x / 0.904x / 0.991x / 0.899x / 0.809x. C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_093536_multi-router-router-sendsend-wss-after-contract-audit-c1.txt`; C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_093656_multi-router-router-sendsend-wss-after-contract-audit-c1.txt`; 두 report 모두 `status: complete`, Core v0.10.1 release. |

| 2026-08-10 | C/C++ | Multi / MULTI_ROUTER_ROUTER_REQREP / wss / 64·256·1024·4096·65536·131072B | `multi-router-router-reqrep-wss-short-c1` | 93.26%, 90.33%, 86.67%, 80.87%, 80.80%, 91.49% | 86.67% | 통과·binding-only 추가 변경 없음·다음 대상 진행 | C throughput Kops/s: 91314.0 / 87729.8 / 81789.2 / 63446.6 / 11672.6 / 6192.2; C++: 85156.4 / 79244.0 / 70884.6 / 51308.8 / 9431.4 / 5665.2. 평균 latency ratio: 1.057x / 1.077x / 1.129x / 1.193x / 1.222x / 1.090x. C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_100938_multi-router-router-reqrep-wss-short-c1.txt`; C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_101029_multi-router-router-reqrep-wss-short-c1.txt`; 두 report 모두 `status: complete`. |

| 2026-08-10 | .NET | Single / ROUTER_ROUTER_REQREP / tcp / 64·256·1024·65536·131072·262144B | `router-router-reqrep-tcp-paired-final` | 61.81%, 63.07%, 58.03%, 96.05%, 98.61%, 98.70% | 79.38% (산술평균) | 통과·평균 latency ratio 1.119x | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_145246_router-router-reqrep-tcp-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_145502_dotnet-router-router-reqrep-tcp-paired-final.txt` |
| 2026-08-10 | .NET | Single / PAIR / ws / 64·256·1024·65536·131072·262144B | `dotnet-pair-ws-paired-final` | 78.19%, 73.99%, 97.32%, 94.92%, 94.07%, 97.43% | 89.32% (산술평균) | 통과·평균 latency ratio 1.133x | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_145939_dotnet-pair-ws-c-paired.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_145953_dotnet-pair-ws-paired-final.txt` |
| 2026-08-10 | .NET | Single / PUBSUB / ws / 64·256·1024·65536·131072·262144B | `dotnet-pubsub-ws-own-after-unchecked-publish` | 68.29%, 53.13%, 94.54%, 90.84%, 95.12%, 98.40% | 83.39% (산술평균) | 보류·자체 1차 개선 후 aggregate 미달·Sol 2차 후보 no-go | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_150225_dotnet-pubsub-ws-c-paired.txt`; baseline: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_150246_dotnet-pubsub-ws-paired-final.txt`; 1차 after: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_150638_dotnet-pubsub-ws-own-after-unchecked-publish.txt`; Sol 2차 after: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_151007_dotnet-pubsub-ws-sol-after-lock-coalesce.txt` |
| 2026-08-10 | .NET | Single / DEALER_DEALER / ws / 64·256·1024·65536·131072·262144B | `dotnet-dealer-dealer-ws-paired-final` | 70.80%, 62.08%, 94.44%, 91.10%, 92.77%, 98.79% | 85.00% (산술평균) | 통과·평균 latency ratio 1.013x | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_151412_dotnet-dealer-dealer-ws-c-paired.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_151426_dotnet-dealer-dealer-ws-paired-final.txt` |
| 2026-08-10 | .NET | Single / DEALER_ROUTER / wss / 64·256·1024·65536·131072·262144B | `dotnet-dealer-router-wss-paired-final` | 70.60%, 84.93%, 97.86%, 93.69%, 95.87%, 92.32% | 89.21% (산술평균) | 통과·평균 latency ratio 1.018x | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_154433_dealer-router-wss-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_154448_dotnet-dealer-router-wss-paired-final.txt` |
| 2026-08-10 | .NET | Single / DEALER_ROUTER_REQREP / wss / 64·256·1024·65536·131072·262144B | `dotnet-dealer-router-reqrep-wss-paired-final` | 61.55%, 73.74%, 83.05%, 93.50%, 88.95%, 97.12% | 82.98% (산술평균) | 통과·평균 latency ratio 1.104x | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155235_dealer-router-reqrep-wss-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_155250_dealer-router-reqrep-wss-paired-final.txt` |
| 2026-08-10 | .NET | Single / ROUTER_ROUTER / wss / 64·256·1024·65536·131072·262144B | `dotnet-router-router-wss-paired-final` | 59.94%, 87.13%, 100.10%, 96.97%, 94.94%, 89.23% | 88.05% (산술평균) | 통과·평균 latency ratio 0.943x | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155438_router-router-wss-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_155452_router-router-wss-paired-final.txt` |
| 2026-08-10 | .NET | Single / ROUTER_ROUTER_REQREP / wss / 64·256·1024·65536·131072·262144B | `dotnet-router-router-reqrep-wss-paired-final` | 57.49%, 68.06%, 87.72%, 94.43%, 99.75%, 90.66% | 83.02% (산술평균) | 통과·평균 latency ratio 1.095x | C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260810_155632_router-router-reqrep-wss-paired-c1.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260810_155645_router-router-reqrep-wss-paired-final.txt` |

### 11.1 이전 측정값 판정 이력

아래 표는 이전 기준으로 남아 있는 분류의 이력이다. `runs=1` 또는 `runs>1`의 대표값과
원시 반복값은 보존한다. 현재 최종 판정은 11.2절의 aggregate 산술평균 규칙을 적용하며,
아래의 `미달` 또는 중앙값 기반 `보류` 문구를 현재 상태로 사용하지 않는다. report가 없는
size만 `미측정`으로 남긴다.

| 대상 | 측정 ratio | 측정 중앙값 | 최종 상태 |
|------|------------|---------------|-------------|
| `ROUTER_ROUTER / tcp` | 88.71%, 92.33%, 95.11%, 81.23%, 88.27%, 92.64% | 90.52% | 통과·binding-only 개선 no-go |
| `ROUTER_ROUTER_REQREP / tcp` | 84.17%, 87.46%, 87.21%, 90.92%, 98.71%, 100.74% | 89.19% | 통과·binding-only 개선 no-go |
| `ROUTER_ROUTER_REQREP / ws` | 83.20%, 86.38%, 89.13%, 97.58%, 88.25%, 91.51% | 88.69% | 통과·binding-only 개선 no-go |
| `ROUTER_ROUTER_REQREP / wss` | 84.49%, 84.38%, 87.27%, 98.30%, 92.55%, 96.73% | 89.91% | 통과·binding-only 개선 no-go |
| `ROUTER_ROUTER_REQREP / tls` | 87.54%, 87.80%, 94.59%, 91.39%, 88.85%, 97.32% | 90.12% | 통과·binding-only 개선 no-go |
| `ROUTER_ROUTER_REQREP / inproc` | 81.73%, 87.19%, 87.08%, 34.92%, 92.40%, 87.45% | 87.14% | 미달·binding-only 개선 no-go |
| `ROUTER_ROUTER_REQREP / ipc` | 87.77%, 89.40%, 81.98%, 90.53%, 90.03%, 99.70% | 89.72% | 통과·binding-only 개선 no-go |
| `DEALER_ROUTER_REQREP / ws` | 96.25%, 92.11%, 86.03%, 97.01%, 98.97%, 94.82% | 95.54% | 통과·다음 대상 진행 |
| `DEALER_ROUTER_REQREP / wss` | 78.73%, 91.66%, 118.48%, 100.56%, 94.69%, 92.82% | 93.76% | 통과·socket request/reply 최소 75%·중앙값 85% 충족 |
| `DEALER_ROUTER_REQREP / tls` | 100.58%, 93.13%, 80.31%, 110.56%, 98.26%, 99.62% | 98.94% | 통과·socket request/reply 최소 75%·중앙값 85% 충족 |
| `DEALER_ROUTER_REQREP / inproc` | 94.86%, 93.67%, 93.65%, 30.48%, 97.49%, 97.51% | 94.27% | 미달·개별 최소 기준 미달 셀 기록 |
| `DEALER_ROUTER_REQREP / ipc` | 85.25%, 88.77%, 86.33%, 91.07%, 94.05%, 91.75% | 89.92% | 통과·binding-only 개선 no-go |
| `MULTI_ROUTER_ROUTER_REQREP / ws` | 99.34%, 107.52%, 98.21%, 97.91%, 98.17%, 97.49% | 98.19% | 통과·pool bypass A/B 결과 반영 |
| `MULTI_PUBSUB / ws` | 106.02%, 96.10%, 92.64%, 103.72%, 107.99%, 103.86% | 103.79% | throughput 통과·1024B 이상 평균 latency 미달 |
| `MULTI_DEALER_ROUTER_REQREP / tcp` | 90.48%, 126.87%, 88.47%, 152.81%, 87.77%, 224.51% | 108.68% | 통과·다음 대상 진행 |
| `MULTI_ROUTER_ROUTER_REQREP / tcp` | 78.08%, 126.53%, 81.68%, 187.46%, 81.87%, 243.49% | 104.20% | 통과·다음 대상 진행 |
| `MULTI_PUBSUB / tcp` | 82.83%, 105.29%, 75.72%, 122.91%, 73.18%, 121.21% | 94.06% | 미달·binding-only 개선 no-go |
| `MULTI_ROUTER_ROUTER_SENDSEND / tcp` | 98.59%, 173.10%, 95.43%, 200.53%, 84.42%, 244.37% | 135.85% | 통과·다음 대상 진행 |
| `MULTI_DEALER_ROUTER_SENDSEND / tcp` | 97.62%, 85.62%, 89.76%, 93.77%, 77.97%, 166.10% | 91.77% | 미달·개별 최소 기준 미달 셀 기록 |
| `MULTI_DEALER_DEALER / tcp` | 93.44%, 76.85%, 117.46%, 101.63%, 95.88%, 144.49% | 98.76% | 미달·개별 최소 기준 미달 셀 기록 |
| `MULTI_STREAM / tcp` | 61.55%, 107.40%, 126.61%, 144.53% | 117.00% | 미달·개별 최소 기준 미달 셀 기록 |
| `MULTI_STREAM / ws` | 78.48%, 102.55%, 150.72%, 163.32% | 126.63% | 미달·개별 최소 기준 미달 셀 기록 |
| `MULTI_STREAM / wss` | 106.75%, 95.91%, 107.44%, 118.32% | 107.09% | 통과·다음 대상 진행 |
| `MULTI_STREAM / tls` | 83.99%, 78.29%, 102.04%, 111.10% | 93.02% | 미달·개별 최소 기준 미달 셀 기록 |
| `MULTI_DEALER_DEALER / ws` | 91.72%, 121.32%, 77.88%, 159.28%, 77.86%, 186.15% | 121.32% | 미달·개별 최소 기준 미달 셀 기록 |
| `MULTI_DEALER_DEALER / wss` | 87.73%, 92.34%, 98.07%, 86.27%, 79.67%, 104.59% | 90.04% | 미달·65536B 개별 기준과 중앙값 목표 미달, 평균 latency ratio 0.245x/0.506x/1.012x/1.164x/1.081x/0.985x |
| `MULTI_DEALER_ROUTER_SENDSEND / wss` | 96.35%, 89.93%, 81.42%, 81.36%, 83.48%, 93.15% | 86.70% | 통과·개별 최소 80%, 중앙값 목표 85%, 평균 latency ratio 최대 1.229x |
| `MULTI_DEALER_ROUTER_REQREP / wss` | 94.06%, 80.49%, 80.49%, 83.56%, 78.79%, 92.71% | 82.02% | 보류·개별 최소 75%와 latency 기준은 충족하지만 중앙값 목표 85% 미달. public interface를 유지한 추가 개선 후보가 없어 보류 |
| `MULTI_ROUTER_ROUTER_SENDSEND / wss` | 104.09%, 101.35%, 110.36%, 100.40%, 111.91%, 123.69% | 107.22% | 통과·다음 대상 진행 |
| `MULTI_ROUTER_ROUTER_REQREP / wss` | 93.26%, 90.33%, 86.67%, 80.87%, 80.80%, 91.49% | 86.67% | 통과·개별 최소 75%, 중앙값 목표 85%, latency ratio 최대 1.222x |
| `MULTI_PUBSUB / wss` | 94.04%, 90.30%, 96.42%, 97.56%, 91.99%, 96.61% | 95.23% | throughput 통과·65536B/131072B latency ratio 3.309x/4.802x 미달·binding-only 개선 no-go |
| `MULTI_PUBSUB / tls` | 98.67%, 88.45%, 95.73%, 99.44%, 99.72%, 96.75% | 97.71% | 통과·aggregate latency 중앙값 1.052x; 256B ratio 88.45%와 65536B·131072B 개별 latency outlier는 기록만 함. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_111320_multi-pubsub-tls-storage-default-full-c12.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_111359_multi-pubsub-tls-storage-default-full-c12.txt` |

| `MULTI_DEALER_DEALER / tls` | 95.04%, 96.77%, 100.63%, 99.76%, 93.72%, 99.12% | 97.95% | 통과·aggregate latency 중앙값 0.978x; 64B·65536B 개별 latency outlier는 기록만 함. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_112546_multi-dealer-dealer-tls-short-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_113100_multi-dealer-dealer-tls-short-c2.txt` |
| `MULTI_DEALER_ROUTER_SENDSEND / tls` | 101.32%, 92.43%, 79.79%, 79.13%, 87.49%, 105.25% | 89.96% | 통과·aggregate latency 중앙값 1.114x; 1024B·4096B 개별 throughput outlier는 기록만 함. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_113648_multi-dealer-router-sendsend-tls-short-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_113736_multi-dealer-router-sendsend-tls-short-c2.txt` |
| `MULTI_DEALER_ROUTER_REQREP / tls` | 84.54%, 96.40%, 94.67%, 96.32%, 110.52%, 86.15% | 95.50% | 통과·aggregate latency 중앙값 1.030x; 초기 median 84.56%에서 `message_t(size_t)` 후보 적용 후 개선. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_114047_multi-dealer-router-reqrep-tls-short-c1.txt`; 초기 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_114137_multi-dealer-router-reqrep-tls-short-c2.txt`; 최종 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_114927_multi-dealer-router-reqrep-tls-size-ctor-c3.txt` |
| `MULTI_DEALER_ROUTER_REQREP / tls` | 84.54%, 96.40%, 94.67%, 96.32%, 110.52%, 86.15% | 95.50% | 통과·aggregate latency 중앙값 1.030x; 초기 median 84.56%에서 `message_t(size_t)` 후보 적용 후 개선. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_114047_multi-dealer-router-reqrep-tls-short-c1.txt`; 초기 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_114137_multi-dealer-router-reqrep-tls-short-c2.txt`; 최종 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_114927_multi-dealer-router-reqrep-tls-size-ctor-c3.txt` |
| `MULTI_ROUTER_ROUTER_SENDSEND / tls` | 100.82%, 99.01%, 101.04%, 101.62%, 103.58%, 125.09% | 101.33% | 통과·aggregate latency 중앙값 0.972x·추가 source 변경 없음·다음 대상 진행. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_115341_multi-router-router-sendsend-tls-short-c1.txt`; C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_115430_multi-router-router-sendsend-tls-short-c2.txt` |
| `MULTI_ROUTER_ROUTER_REQREP / tls` | 111.07%, 96.37%, 82.53%, 93.72%, 99.54%, 116.25% | 97.95% | 통과·aggregate latency 중앙값 1.008x·copy constructor `_storage()` zero-init 제거로 baseline 82.98%에서 개선·다음 대상 진행. C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260810_120243_multi-router-router-reqrep-tls-short-c1.txt`; baseline C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_120331_multi-router-router-reqrep-tls-short-c2.txt`; 최종 C++: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260810_120837_multi-router-router-reqrep-tls-copy-ctor-c3.txt` |

### 11.2 C++ 빠른 최종 판정

2026-08-10 현재 C++ Single 42개 행과 선택된 Multi 28개 행은 C·C++ paired report가 모두
`status: complete`다. 최종 gate는 size별 throughput ratio의 산술평균과 평균 latency ratio의
산술평균으로 적용한다. 개별 size의 `미달` 표시는 결과 기록이며, 종합 평균이 목표를 넘으면
전체 판정을 바꾸지 않는다. 이 절의 판정 뒤에는 C++ perf를 추가 실행하지 않는다.

Single은 35개 행이 `통과`, 다음 7개 `inproc` 행이 `보류`다. 각 행은 이미 hot path 검토와
후보 비교를 마쳤고 공개 interface·ownership·error contract를 유지한 추가 효과 후보가 없다.

| 대상 | throughput ratio 산술평균 | 최종 판정 |
|------|---------------------------:|----------|
| `PAIR / inproc` | 66.91% | 보류 |
| `PUBSUB / inproc` | 74.68% | 보류 |
| `DEALER_DEALER / inproc` | 75.00% | 보류 |
| `DEALER_ROUTER / inproc` | 71.77% | 보류 |
| `DEALER_ROUTER_REQREP / inproc` | 84.61% | 보류 |
| `ROUTER_ROUTER / inproc` | 67.75% | 보류 |
| `ROUTER_ROUTER_REQREP / inproc` | 78.46% | 보류 |

Multi의 throughput ratio 산술평균은 선택된 행에서 모두 해당 pattern 목표를 충족한다. 다만
`MULTI_PUBSUB`의 평균 latency ratio는 `ws 6.576x`, `wss 2.076x`, `tls 2.017x`로 2.0x를
넘어 해당 세 행은 `보류`한다. 나머지 Multi 행은 throughput과 평균 latency aggregate를
통과한다. `MULTI_DEALER_ROUTER_REQREP / wss`의 throughput 평균은 85.02%로 중앙값 85%
목표를 충족하므로 이전의 중앙값 82.02% 기반 보류 표기를 `통과`로 정정한다.

## 12. 완료 기준

다음 조건을 모두 만족해야 작업을 완료한다.

- runner, 정책, 상세 표의 pattern, transport, size inventory가 일치한다.
- 각 pattern의 최종 판정에 사용한 core 0.10.1 C와 binding paired report가 모두
  `status: complete`다.
- 모든 binding 상세 표에 `미측정` 또는 최종 판정 전 `미달`이 없다. aggregate 평균 미달
  행은 hot path 검토와 후보 A/B, 필요한 Sol 리뷰를 마친 뒤 `보류`로 확정할 수 있다.
- 모든 통과 셀에 paired C와 binding report, manifest, 반복값, 비율, 옵션 일치 근거가
  기록되어 있다.
- throughput ratio와 평균 latency ratio의 aggregate 산술평균, client 수, auto-HWM, 대상
  외 대표 셀 회귀 gate를 측정값으로 판정하고, 반복값과 변동 폭은 참고 기록으로 남겼다.
- 변경한 binding의 단위 테스트와 통합 테스트가 통과한다.
- 한 언어의 모든 pattern이 각각 완료되기 전에는 다음 언어로 이동하지 않는다.
- 채택한 성능 개선은 검증된 범위만 커밋하고 원격에 푸시했으며 commit id를 기록했다.
- perf 전용 우회, private API 접근, 무시되는 필수 option, timeout/sleep 증가가 남아
  있지 않다.
- 최종 리뷰에서 public interface가 더 복잡해지지 않았고 비용이 binding 내부에서
  줄었는지 확인했다.
