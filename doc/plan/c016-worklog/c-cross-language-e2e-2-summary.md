# §C 재검증 요약 — protobuf include 경로와 cross-language E2E

`add_zlink_protobuf_schema()`가 build root와 protobuf module이 보존한 source-relative output
directory를 모두 public include directory로 공개하도록 수정했다. 재구성과 full build는 성공했고,
0.17.0 Core에 연결된 새 C++ cross-language host가 생성됐다. 7개 C++ sample도 모두 통과했다.

Cross-language 전체 gate는 `.NET client -> C++ spot-route host`가 `Unavailable`로 끝나 중단됐다.
별도 Java selector는 네 방향 모두 통과했고, Node smoke는 실제 stream dispatch/reply 후 .NET flow
listener가 필수 Activity tag를 읽지 못해 실패했다.

## CMake 수정과 protobuf 선택 결과

수정 파일은 `framework/languages/cpp/CMakeLists.txt` 하나다.

- `proto_dir`를 `CMAKE_CURRENT_SOURCE_DIR` 기준 상대 경로로 계산했다.
- 기존 `${CMAKE_CURRENT_BINARY_DIR}` include는 유지했다. 따라서 build root에 생성하는 protobuf
  config 환경도 계속 동작한다.
- `${CMAKE_CURRENT_BINARY_DIR}/${proto_relative_dir}`도 함께 공개했다. CMake `FindProtobuf`
  module처럼 source-relative subdirectory를 보존하는 환경에서 generated header를 찾는다.
- source의 `#include`는 변경하지 않았다.

현재 cache에서는 `find_package(protobuf CONFIG QUIET)`가 실패했다
(`protobuf_DIR=protobuf_DIR-NOTFOUND`). 이어진 `find_package(Protobuf MODULE REQUIRED)`가 Ubuntu
system protobuf 3.21.12를 선택했다. `protoc=/usr/bin/protoc`,
`libprotobuf=/usr/lib/x86_64-linux-gnu/libprotobuf.so`다.

재구성 후 `compile_commands.json`에는 build root와 다음 두 output directory가 함께 들어갔다.

- `build/linux-ninja-c-e2e/samples/Bingo/Shared/Contracts`
- `build/linux-ninja-c-e2e/e2e/RegistrationCodec/Shared`

full build는 28.87초, exit 0이었다. `zlink_cpp_cross_language_host`를 새로 link했고 `ldd`는
`.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so.0`을 가리킨다. cache의 C++ binding과
Core version도 모두 0.17.0이다.

저장소 root에서 brief의 reconfigure 명령을 문자 그대로 실행한 첫 시도는 root에
`CMakePresets.json`이 없어 exit 1이었다. preset 소유 디렉터리인
`framework/languages/cpp`에서 동일 build directory를 지정한 명령은 1.21초, exit 0으로
성공했다.

## Cross-language 결과

`.NET` tree에는 독립 runner script가 없다. `.NET` TestHost를 피어로 실행하는 진입점은 C++
`run_cross_language_smoke.sh`이며, 이 runner는 지정한 `ZLINK_CPP_BUILD_DIR`의 host를 사용했다.

| 범위 | 결과 | exit | 시간 | 근거 |
|---|---:|---:|---:|---|
| C++ full build | PASS | 0 | 28.87s | protobuf 두 target과 새 C++ host link 완료 |
| C++ cross-language all, 첫 실행 | FAIL (D, 해결) | 1 | 151.34s | Playwright headless shell v1234 미설치로 browser connector launch 실패 |
| C++ cross-language all, 재실행 | FAIL (B) | 1 | 239.51s | `.NET -> C++` spot-route request가 `Unavailable`; C++ server event 없음 |
| C++↔.NET 선행 messaging/flow/stream | PASS | 0 | all 실행에 포함 | channel request 양방향, fanout 양방향, stream 양방향 event 확인 |
| C++↔Node 선행 messaging/flow/stream/message-follow | PASS | 0 | all 실행에 포함 | browser stream 포함 기대 event 확인 후 spot-route로 진행 |
| C++ client → .NET spot-route host | PASS | 0 | all 실행에 포함 | reply, not_found, rejected 세 event 확인 |
| .NET client → C++ spot-route host | FAIL (B) | - | 15s client retry, runner 180s wait | client terminal `kind=unavailable`; server request event 0건 |
| all의 이후 C++/Node·Java/C++·relocation·User-Spot Join | NOT RUN | - | - | `set -e`가 위 실패에서 gate 중단 |
| Node cross-language smoke | FAIL (D) | 1 | 32.55s | stream dispatch/reply 성공 후 flow tag assertion 실패 |
| `java-cross`, 첫 실행 | FAIL (D, 실행 환경 수정) | 1 | 61.99s | Java에 directory형 `ZLINK_LIBRARY_PATH`를 잘못 상속해 `UnsatisfiedLinkError` |
| `java-cross`, 환경 수정 재실행 | PASS | 0 | 13.58s | Java↔Node, Java↔.NET 네 spot-route 방향 모두 통과 |

첫 all 실행의 browser 실패는
`framework/languages/node/.cache/ms-playwright/chromium_headless_shell-1234/...`가 없다는 정확한
launch 오류였다. 현재 Node package가 요구하는 Chromium/Headless Shell v1234를 같은 project
cache에 설치한 뒤 해당 단계는 통과했다.

all 재실행에서 `.NET` TestHost의
`framework/languages/dotnet/cross-language/Zlink.Framework.TestHost/Program.cs:913` 요청은 15초
동안 `Unavailable`/`DeadlineExceeded`를 재시도한 뒤 `Program.cs:919-932`에서
`spot-route-error|kind=unavailable|origin=unspecified`를 기록했다. C++ host는 ready였지만
`cpp-spotroute-host.events`가 비어 있었다. 이 실행에는 native errno나 BACKPRESSURED 결과가
기록되지 않았고 awaitable도 미정착 상태로 남지 않았다. 또한 route call은 binding submit 전
`ZLinkRouteClient.cs:403`의 `EnsureKnownRouteMeshPeer`를 거친다. 따라서 D-B85의 EAGAIN을
입증하지 않은 채 bucket E로 추정하지 않고 관찰된 terminal/error 분류인 bucket B로 남긴다.
참고로 실제 .NET binding async REQUEST의 DONTWAIT submit 지점은
`bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:164`다.

Node smoke의 실제 application 동작은 `dispatch:dotnet-to-node, reply:sent`까지 완료됐다. 실패한
assertion은 `framework/languages/node/cross-language/node_dotnet_smoke.js:895-906`의
`assertFlowLog()`다. `.flow`의 네 줄 모두 `packet=<null> flow=<null> origin=<null>`이었고,
TestHost listener는 `TestHostMessageFlowListener.cs:36-41`에서 `packet_name`, `flow_id`,
`flow_origin` Activity tag를 읽는다. runtime terminal 실패가 아니라 flow listener/harness의
attribute 계약 불일치이므로 bucket D다. 다른 작업이 소유한 `.NET` tree는 수정하지 않았다.

## C++ sample 결과

각 sample은 지정 build directory로 `run_samples.sh <selector>`를 원래 배열 순서대로 실행해
개별 exit와 시간을 얻었다. 이어서 인자 없는 aggregate `run_samples.sh`도 실행해 7개 전체 raw
결과를 로그로 보존했으며 198.28초, exit 0이었다.

| sample | 결과 | exit | 개별 시간 |
|---|---:|---:|---:|
| TicTacToe | PASS | 0 | 25.662s |
| Bingo | PASS | 0 | 40.853s |
| DeliveryDispatch | PASS | 0 | 51.440s |
| SupportChat | PASS | 0 | 67.449s |
| GameQuest | PASS | 0 | 21.799s |
| ShoppingMall | PASS | 0 | 53.031s |
| ZoneWorld | PASS | 0 | 145.590s |

GameQuest의 shell `Killed` 행은 실패가 아니다. `run_sample.sh:343-350`이 owner process를
의도적으로 `kill -9`하고 exit 137인지 검증하는 시나리오이며, 이후
`gamequest-placement=completed`와 exit 0을 확인했다.

## 실패 bucket

| bucket | 결과 | 근거 |
|---|---|---|
| A: DONTWAIT/backpressure | 확인 없음 | 어느 보존 로그에도 BACKPRESSURED/EAGAIN/native errno가 기록되지 않음 |
| B: terminal/error classification | 1건 | `.NET -> C++` spot-route가 `kind=unavailable`, `origin=unspecified` terminal로 종료 |
| C: known pre-existing | 해당 없음 | inventory 278, Node lint `spot-timer.ts:137`, SupportChat browser assertion, Java M6A/DocumentationRegression gate는 이번 명령의 실패가 아님 |
| D: environment/runner | 3건, 2건 해결 | Playwright v1234 누락(설치 후 해결), Java에 잘못 상속한 directory형 library path(제거 후 해결), Node flow Activity tag 불일치(잔존) |
| E: D-B85 binding-port dependency | 입증된 실패 없음 | async REQUEST가 DONTWAIT인 정적 지점은 확인했지만 이번 실패에 BACKPRESSURED/EAGAIN 또는 미정착 awaitable 증거가 없음 |

## 실행 명령

```bash
# 저장소 root의 문자 그대로인 첫 시도: preset 파일을 찾지 못해 실패
cmake --preset linux-ninja-debug \
  -B framework/languages/cpp/build/linux-ninja-c-e2e

cd framework/languages/cpp
cmake --preset linux-ninja-debug -B build/linux-ninja-c-e2e
cd ../../..
cmake --build framework/languages/cpp/build/linux-ninja-c-e2e -j16

export ZLINK_CPP_BUILD_DIR=/home/hep7/project/zlink/framework/languages/cpp/build/linux-ninja-c-e2e
export ZLINK_CPP_CROSS_KEEP_RUN_DIR=1
framework/languages/cpp/cross-language/run_cross_language_smoke.sh

cd framework/languages/node
PLAYWRIGHT_BROWSERS_PATH="$PWD/.cache/ms-playwright" npx playwright install chromium
cd ../../..
framework/languages/cpp/cross-language/run_cross_language_smoke.sh

framework/languages/node/cross-language/run_cross_language_smoke.sh

unset ZLINK_LIBRARY_PATH
ZLINK_CPP_CROSS_LANGUAGE_STAGE=java-cross \
  framework/languages/cpp/cross-language/run_cross_language_smoke.sh

ZLINK_CPP_BUILD_DIR=/home/hep7/project/zlink/framework/languages/cpp/build/linux-ninja-c-e2e \
  framework/languages/cpp/samples/run_samples.sh
```

Cross-language 명령에는 원본 brief의 `TMPDIR`, NuGet hash cache, shared compilation 설정과
`/tmp/zlink-{dotnet,node,jvm}-gate.lock`을 적용했다. Java 재실행에서는 Java가
`ZLINK_LIBRARY_PATH`를 파일로 해석하므로 .NET test 전용 directory 값을 제거했다.

## 로그

- `zlink-work/c016/logs/c-e2e-2-01-cpp-reconfigure.log`: root cwd의 preset 실패
- `zlink-work/c016/logs/c-e2e-2-02-cpp-reconfigure.log`: 정상 재구성
- `zlink-work/c016/logs/c-e2e-2-03-cpp-build.log`: full build
- `zlink-work/c016/logs/c-e2e-2-04-cpp-cross-all.log`: Playwright 누락 실패
- `zlink-work/c016/logs/c-e2e-2-05-playwright-install.log`: Chromium v1234 설치
- `zlink-work/c016/logs/c-e2e-2-06-cpp-cross-all-rerun.log`: `.NET -> C++` terminal 실패
- `zlink-work/c016/logs/c-e2e-2-07-node-cross.log`: Node flow assertion 실패
- `zlink-work/c016/logs/c-e2e-2-08-java-cross.log`: 잘못 상속한 library path 실패
- `zlink-work/c016/logs/c-e2e-2-09-java-cross-rerun.log`: Java 네 방향 통과
- `zlink-work/c016/logs/c-e2e-2-10-cpp-samples.log`: 7개 aggregate sample 통과

보존한 실패 run directory는 다음과 같다.

- `/dev/shm/zlink-tmp-dotnet/tmp.l2zVAJPTeF`
- `/dev/shm/zlink-tmp-dotnet/tmp.FYuDiwbtVL`
- `/dev/shm/zlink-tmp-java/tmp.zLRp5AL8h7`

## BLOCKERS

1. `.NET client -> C++ spot-route host`가 known peer/admission 또는 그 이후 request 경로 중
   어디서 `Unavailable`이 되는지 native submit 전후 진단이 필요하다. 이번 증거만으로 D-B85
   EAGAIN이라고 단정할 수 없다.
2. Node smoke의 .NET TestHost Activity listener와 현재 message-flow Activity attribute 이름을
   일치시켜야 한다. 이 작업에서는 명시적으로 제외된 `framework/languages/dotnet/**`를 수정하지
   않았다.
3. 위 첫 blocker 때문에 all-stage runner 후반의 C++/Node spot-route, Java/C++ spot-route,
   relocation과 12개 User-Spot Join 조합은 이번 all 실행에서 검증되지 않았다.
