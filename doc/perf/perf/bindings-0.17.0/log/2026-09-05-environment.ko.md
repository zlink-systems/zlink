# Core 0.17.0 bindings 성능 측정 환경

기록일은 2026-09-05이며, 이 manifest는 inventory gate를 완료한 뒤 paired 측정에 사용할
호스트와 toolchain을 고정한다. 아직 성능 측정은 실행하지 않았다.

## Source와 Core runtime

| 항목 | 값 |
|------|----|
| source | `main` / `87057e86542787fb1ef9c0e3d9a0d60ffc09fe4a` |
| 조사 시작 상태 | clean |
| manifest 작성 상태 | dirty: 이 계획서와 이 manifest만 변경 |
| Core runtime | `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0` |
| Core build | `Release`, LTO 활성화(`CMAKE_BUILD_TYPE=Release`, `ENABLE_LTO=ON`) |
| Core provenance | `29add0ac81` 변경을 포함해 재링크한 local workspace artifact |
| ELF Build ID | `1e93b62c49d42f4de000c415cfc1eb4faae94aab` |
| Core SHA-256 | `a98cc793457dae04fc58aaafc9cf6fcbe70b021e59cba61b8846aab623061025` |

Core runtime은 `file`, `readlink -f`, `readelf -n`으로 실제 파일과 Build ID를 확인했다.
`core/build/CMakeCache.txt`에서 Release와 LTO 설정을 확인했고, Core source와 include 아래에는
runtime보다 새로운 파일이 없었다.

## Host

| 항목 | 값 |
|------|----|
| 환경 | Linux x86_64, WSL2 |
| kernel | `6.6.87.2-microsoft-standard-WSL2` |
| CPU | 12th Gen Intel(R) Core(TM) i7-1260P |
| 논리 CPU | 16 (`nproc`, `lscpu`) |
| 물리 구성 | 1 socket, 8 cores/socket |
| memory | 11.68 GiB (`MemTotal: 12247220 kB`; 측정 표기는 11 GiB) |
| CPU governor | WSL2에서 `/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` 미노출 |

## Compiler와 언어 runtime

| 항목 | 버전 / 경로 |
|------|-------------|
| GCC / G++ | Ubuntu GCC 13.3.0 (`13.3.0-6ubuntu2~24.04.1`) |
| Clang / Clang++ | 설치되지 않음(`command not found`) |
| Node | `v24.19.0` |
| .NET SDK | `8.0.130`, `/usr/lib/dotnet/sdk` |
| Java 기본 runtime/toolchain | OpenJDK `21.0.12`, `javac 21.0.12` |
| Java perf runtime/toolchain | Temurin `22.0.2+9`, `javac 22.0.2`, `JAVA_HOME=/home/hep7hep7/.jdks/jdk-22.0.2+9` |
| Go | `go1.22.2 linux/amd64` |
| Cargo | `1.75.0` |
| Rustc | `1.75.0 (82e1608df 2023-12-21)` |
| Python | `3.12.3` |
| pytest venv | `/home/hep7hep7/.cache/zlink/python-test-venv` (`Python 3.12.3`, `pytest 9.1.1`) |

## Runner inventory

source의 pattern table·CLI parser·README와 각 공식 wrapper의 `--help`를 대조했다. 아래
목록은 alias를 제외한 report canonical pattern이다.

| 언어 | Single pattern | Multi pattern | 기본 transport |
|------|----------------|---------------|----------------|
| C 기준 | `PAIR`, `PUBSUB`, `DEALER_DEALER`, `DEALER_ROUTER`, `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER`, `ROUTER_ROUTER_REQREP` | `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_SENDSEND`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_SENDSEND`, `MULTI_ROUTER_ROUTER_REQREP`, `MULTI_PUBSUB`, `MULTI_STREAM` | Single `tcp,tls,ws,wss,inproc,ipc`; Multi `tcp,tls,ws,wss` |
| C++ | `PAIR`, `PUBSUB`, `DEALER_DEALER`, `DEALER_ROUTER`, `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER`, `ROUTER_ROUTER_REQREP` | `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_SENDSEND`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_SENDSEND`, `MULTI_ROUTER_ROUTER_REQREP`, `MULTI_PUBSUB`, `MULTI_STREAM` | Single `tcp,tls,ws,wss,inproc,ipc`; Multi `tcp,tls,ws,wss` |
| .NET | 위와 같음 | 위와 같음 | Single `tcp,tls,ws,wss,inproc,ipc`; Multi `tcp,tls,ws,wss` |
| Java | 위와 같음 | 위와 같음 | Single `tcp,tls,ws,wss,inproc,ipc`; Multi `tcp,tls,ws,wss` |
| Node | 위와 같음 | 위와 같음 | Single `tcp,tls,ws,wss,inproc,ipc`; Multi `tcp,tls,ws,wss` |
| Go | 위와 같음 | 위와 같음 | Single `tcp,tls,ws,wss,inproc,ipc`; Multi `tcp,tls,ws,wss` |
| Rust | 위와 같음 | 위와 같음 | Single `tcp,tls,ws,wss,inproc,ipc`; Multi `tcp,tls,ws,wss` |
| Python | 위와 같음 | 위와 같음 | Single `tcp,tls,ws,wss,inproc,ipc`; Multi `tcp,tls,ws,wss` |

Linux에서 Node와 Python의 routed Multi variant, Go의
`MULTI_ROUTER_ROUTER_SENDSEND`는 명시적 `ipc` 진단도 등록하지만, 공식 기본 transport에는
포함하지 않는다. 표는 paired 기본 측정 범위를 기록한다. C runner의 Single 기본 크기 6개,
Multi 기본 크기 6개, `MULTI_STREAM` 전용 크기 4개는 계획서 §3과 일치한다.

README는 일부 현재 registry보다 뒤처져 있다. .NET Multi, Java Single/Multi, Node
Single/Multi, Python Single과 `bindings/python/perf/multi/README.md`에는 REQREP canonical
pattern 일부가 빠져 있다. 수정 허용 범위 밖이므로 이 작업에서는 변경하지 않았고, 실제
runner registry와 `ALL` parser를 상세 표의 기준으로 사용했다.

## 측정 규칙

- 동시에 perf process는 하나만 실행한다.
- `--pin-cpu`는 사용하지 않는다.
- 모든 C와 binding runner에 `ZLINK_CORE_SOURCE=local`을 명시한다.
- Java perf에는 `JAVA_HOME=/home/hep7hep7/.jdks/jdk-22.0.2+9`를 명시한다.
- Python 검증에는 `/home/hep7hep7/.cache/zlink/python-test-venv`를 사용한다.
- paired C와 binding은 같은 session tag, pattern, transport, size, duration, runs, client 수와
  I/O thread 수로 순차 실행한다.
