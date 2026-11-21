# etcd provider 시험 인프라

Node.js 22의 표준 라이브러리만 사용한다. Provider와 Store 관리 process는 이 디렉터리에
구현하지 않는다. 하니스는 단일 개발 머신의 기능·장애 시험용이며 운영 배치의 host 분리를
검증하지 않는다.

## Cluster 명령

```sh
node scripts/etcd/etcd-cluster.mjs up --run-dir scripts/etcd/.runs/example
node scripts/etcd/etcd-cluster.mjs status --run-dir scripts/etcd/.runs/example
node scripts/etcd/etcd-cluster.mjs kill --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs resume --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs pause --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs resume --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs partition --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs heal --run-dir scripts/etcd/.runs/example
node scripts/etcd/etcd-cluster.mjs compact --run-dir scripts/etcd/.runs/example --revision 10
node scripts/etcd/etcd-cluster.mjs down --run-dir scripts/etcd/.runs/example
```

`up`은 새 run directory를 요구한다. Member는 1부터 세며 기본 3개다. `--members 5`와
`--quota-bytes N`을 지원한다. `resume`은 pause한 member를 재개하고 kill한 member는 같은
data directory로 재시작한다. `heal`은 이 run의 모든 peer partition을 해제한다.
`cluster.json`에는 schema version, run ID, 고정 버전, initial cluster token, decimal cluster ID,
client endpoint 목록과 member별 name·포트·data directory·log·member ID를 기록한다.
Docker는 container ID, native는 PID와 `/proc`의 process 시작 시각도 기록한다.

Linux와 WSL은 공식 바이너리를 사용한다. `etcd-version.json`의 SHA-256을 확인하고
`scripts/etcd/.cache/`에 archive를 보관한다. 이 cache는 run directory 밖에 있어야 한다.
`tar`가 필요하며 Linux x64와 arm64를 지원한다. 체크섬 불일치는 실패다.
Windows와 macOS는 Docker의 Linux container를 사용한다. `--backend docker` 또는
`--backend native`로 경로를 명시할 수 있다. Docker는 공식 이미지를 digest로 고정하며
run마다 client·peer network 두 개를 만든다. `partition`은 peer network만 끊는다.
Native는 etcd Raft HTTP의 source member ID를 읽는 peer 전용 proxy로 양방향 연결을 끊는다.
기존 연결도 종료하며 client RPC에는 관여하지 않는다. 이 경로는 고정한 etcd 버전에 맞춘 시험 도구다.

Docker의 client host 포트는 Docker가 배정하며 peer는 Docker network만 사용한다.
Kill한 Docker member를 재시작하면 host 포트가 바뀔 수 있으므로 `cluster.json`의 endpoint를 다시 읽는다.
Native는 `.runs/.ports/`의 포트별 배타 lock에 run ID를 기록하고 `down`에서 자기 lock만 지운다.
포트를 고를 때 이미 사용 중인 포트는 건너뛴다. 고른 뒤 etcd가 bind하기 전에 하니스 밖 process가
같은 포트를 쓰는 경쟁은 막지 못하며, 그 bind 실패는 포트 사용 원인을 보고하고 재시도하지 않는다.

준비 확인은 모든 member의 cluster ID 일치, linearizable read와 Txn 성공을 요구한다.
서버는 spec §9.2의 transaction·요청 크기와 compaction 하한을 사용한다.
포트 충돌이나 준비 실패는 오류로 끝나고 이 run의 자원만 정리한다. `down`은 member log와
data directory를 보존한다. Docker 정리는 run label로, native signal은 PID와 시작 시각으로
대상을 확인한다. `.cluster.lock`은 같은 run의 동시 명령을 거부한다. 강제 종료로 lock만
남았다면 해당 run 명령이 실행 중이지 않은지 확인한 뒤 lock을 지우고 `down`을 실행한다.

## RPC 보류

```sh
node scripts/etcd/hold-proxy.mjs serve --target http://127.0.0.1:2379 --state-file scripts/etcd/.runs/proxy.json
node scripts/etcd/hold-proxy.mjs hold-downstream --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs hold-upstream --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs release --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs disconnect --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs status --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs stop --control http://127.0.0.1:CONTROL_PORT
```

`serve`는 proxy endpoint·control URL·PID를 출력하고 파일에도 기록한다. `--listen PORT`와
`--control PORT`를 생략하면 OS가 포트를 배정한다. 제어 listener는 loopback에만 열린다.
`hold-upstream`은 client→server, `hold-downstream`은 server→client bytes를 보류한다.
현재 연결과 새 연결 모두에 적용한다. `release`는 양쪽을 해제하며 `disconnect`는 현재 연결을
끊는다. 제어 응답이 돌아오면 해당 동작이 적용된 상태다. TCP backpressure로 bytes를
보류하므로 JavaScript 배열에 bytes를 계속 쌓지 않는다. gRPC나 Txn 내용은 해석하지 않는다.

## Raw KV

```sh
node scripts/etcd/raw-kv.mjs put --endpoint http://127.0.0.1:2379 --key-hex 006b --value-hex 00ff
node scripts/etcd/raw-kv.mjs range --endpoint http://127.0.0.1:2379 --key-hex 006b --revision 2
node scripts/etcd/raw-kv.mjs range --endpoint http://127.0.0.1:2379 --key-hex 00 --end-hex 01 --limit 1000
node scripts/etcd/raw-kv.mjs delete --endpoint http://127.0.0.1:2379 --key-hex 006b
node scripts/etcd/raw-kv.mjs txn --endpoint http://127.0.0.1:2379 --body-file scripts/etcd/.runs/txn.json
```

Key와 value 입력은 hex다. Range는 gateway 응답과 `bytes` 배열의 `keyHex`·`valueHex`를
출력한다. Revision은 64-bit decimal 문자열로 전달한다. `--end-hex`는 range와 delete의
exclusive end다. Txn 파일은 etcd v3 gateway JSON이며 byte 필드는 base64다.
Metadata·payload를 서로 다른 Txn으로 쓰면 `mod_revision` 손상 fixture를 만들 수 있다.
도구는 잘못된 저장 형식도 그대로 기록한다. Gateway 오류는 실패 종료로 반환한다.

## Fixture와 검증

```sh
node scripts/etcd/generate-fixtures.mjs
node scripts/etcd/validate-fixtures.mjs
node framework/runtime/conformance/validate-runtime-conformance-fixtures.mjs
node --test scripts/etcd/self-test.mjs
node scripts/etcd/verify-harness.mjs --backend docker --log-dir PATH
node scripts/etcd/verify-harness.mjs --backend native --log-dir PATH
node scripts/etcd/check-conformance-coverage.mjs cpp.json dotnet.json jvm.json node.json
node scripts/etcd/check-conformance-coverage.mjs --language node node.json
```

Golden은 namespace·Framework key bytes, metadata, clock, version과 손상 record를 고정한다.
기존 store record fixture의 preimage bytes를 불투명 key 입력으로 재사용하며 의미를 해석하지 않는다.
적합성 목록은 etcd §12의 25항목과 SPI §10의 provider 공통 10항목을 등록한다. Redis 전용
4항목은 원문 절과 적용 제외 근거를 기록한다. 적합성 목록 JSON은 사람이 관리하며 생성기는 golden byte만 만든다.
Validator는 `{ file, section }`과 bullet 원문을 대조하고 각 bullet의 등록이 정확히 하나인지 확인한다.
`validate-fixtures.mjs --etcd-spec PATH --spi-spec PATH`로 spec 사본을 검사할 수 있다.
저장 시각의 정상 범위는 0..9,223,372,036,854 ms이며 상한 초과와 잘못된 clock 길이는 손상 벡터다.
Store 관리 process가 필요한 case는 실제 관리 process를 시험 adapter가 실행해야 한다.
하니스는 시각 key를 자동으로 만들거나 갱신하지 않는다.

언어별 결과는 다음 JSON 형식이다. 모든 case ID를 한 번씩 `passed`로 제출해야 하며
누락·skip·실패·중복·알 수 없는 ID는 실패다. 기본 검사는 네 언어 모두를 요구한다.

```json
{
  "language": "node",
  "cases": [
    {
      "id": "etcd.cross-language-record",
      "status": "passed",
      "pairs": [{ "writer": "node", "reader": "node", "status": "passed" }]
    }
  ]
}
```

`self-test.mjs`는 가짜 통과 결과의 성공과 누락·skip 결과의 실패, byte 보류·해제와 raw KV
gateway 처리를 확인한다. `verify-harness.mjs`는 두 run을 동시에 띄우고 kill·재시작·pause·peer
partition·heal·compaction과 raw KV를 확인한 뒤 모두 정리한다. 명령별 출력, member log,
manifest와 cleanup 결과를 log directory에 남긴다. `--runs 4 --isolation-only`는 네 run의 격리와 정리만 검사한다.
`--runs 1`은 cluster 하나로 lifecycle을 검사한다. 이 검증은 provider 적합성 실행 결과가 아니다.

## 공통 적합성 실행기

Node workspace의 의존성을 설치하고 `packages/framework-locations-etcd`에서 `npm run build`를
완료한 뒤 실행한다.

```sh
node scripts/etcd/conformance-runner.mjs
node scripts/etcd/conformance-runner.mjs --allow-unverified
node scripts/etcd/check-conformance-coverage.mjs --language node --allow-unverified node-conformance.json
node --test scripts/etcd/conformance-self-test.mjs
```

실행기는 Docker cluster 하나를 생성하고 모든 case와 mutation을 순서대로 실행한 뒤 정리한다.
`conformance-cases.cjs`가 35개 ID의 판정을 소유한다. 등록 함수의 누락과 알 수 없는 ID를
실행 전에 검사한다. `--cases ID,ID`는 조사할 항목을 제한하며 전체 coverage 실패를 유지한다.
`--no-mutations`는 mutation 실행을 끈다. 기본 실행은 등록한 언어마다 mutation 6개의 검출도 요구한다.

Node 의존성은 `framework/languages/node/node_modules`에서 읽는다. 다른 설치 경로를 사용할 때는
`ZLINK_ETCD_NODE_MODULES`를 해당 node_modules 경로로 설정한다. 실행기는 Node 자식 process의
`NODE_PATH`에 이 경로를 전달한다. 기준 provider는 package build가 만든
`framework/languages/node/packages/framework-locations-etcd/dist`를 사용한다.
`ZLINK_ETCD_PROVIDER_DIST`로 격리 복사본의 package build 산출물을 지정할 수 있다.
`index.js` 또는 `admin.js`가 없으면 build 명령을 안내하고 cluster 시작 전에 실패한다.

Mutation 산출물은 source를 `ts.transpileModule`로 변환하고 canonical proto를 복사한다.
같은 변환에서 구문을 바꾸지 않은 대조군으로 mutation 대상 6개 case를 먼저 실행한다.
대조군이 모두 통과해야 mutation을 실행하며, 결과는 `mutation-control-results.json`에 남긴다.
기준 dist·대조군·mutation은 별도 process를 사용한다. 대상 구문이 없으면 mutation은 `not-built`다.

`--adapter-config PATH`는 언어 ID를 key로 하는 JSON 객체를 받는다. 값은 `command`, `args`와
선택적인 `cwd`, `env`다. 등록된 언어로 모든 writer×reader pair를 생성한다. 이번 단계의 기본
설정은 Node만 등록하므로 각 교차언어 case는 1×1 pair다. Protocol과 결과 형식은
[adapter JSONL 문서](../../framework/runtime/conformance/location-store-adapter-protocol-v1.ko.md)가 소유한다.

.NET과 Java는 실행기 밖에서 먼저 build한다. .NET SDK 8과 JDK 25를 사용하며, 저장소 root에서
다음을 실행한다.

```sh
dotnet build framework/languages/dotnet/tests/Zlink.Framework.LocationStore.ProviderAdapter/Zlink.Framework.LocationStore.ProviderAdapter.csproj -c Release -m:1
(cd framework/languages/java && ./gradlew --no-daemon --max-workers=1 -Pzlink.includeSamples=false :zlink-framework-locations-etcd:installProviderAdapterDist)
```

3×3 실행의 adapter 설정 예시는 다음과 같다. `ROOT`를 저장소의 절대 경로로 바꾼다.
Node package dist와 의존성은 위의 환경 변수로 지정할 수 있다.

```json
{
  "node": {
    "command": "node",
    "args": [
      "ROOT/framework/languages/node/test/contract/location-etcd-fixtures/provider-adapter.cjs"
    ]
  },
  "dotnet": {
    "command": "dotnet",
    "args": [
      "ROOT/framework/languages/dotnet/tests/Zlink.Framework.LocationStore.ProviderAdapter/bin/Release/net8.0/Zlink.Framework.LocationStore.ProviderAdapter.dll"
    ]
  },
  "jvm": {
    "command": "ROOT/framework/languages/java/zlink-framework-locations-etcd/build/install/provider-adapter/bin/location-store-provider-adapter",
    "env": { "JAVA_OPTS": "-Xmx256m" }
  }
}
```

```sh
node scripts/etcd/conformance-runner.mjs --adapter-config adapters.json --allow-unverified
node scripts/etcd/check-conformance-coverage.mjs --language node,dotnet,jvm --allow-unverified node-conformance.json dotnet-conformance.json jvm-conformance.json
```

실행기는 .NET DLL이나 Java bin script가 없으면 cluster를 만들기 전에 실패한다. 두 교차언어 ID는
각각 9개 pair를 요구한다. Pair마다 namespace를 분리하며, 누락·중복·skip·실패는 coverage 실패다.

`build-managed-provider.cjs`는 .NET·Java provider source를 격리 사본에 복사해 대조군과 여섯 변형을
build한다. 격리 사본과 산출물은 OS 임시 디렉터리에 두고 실행이 끝나면 삭제한다.
.NET은 기준 Release build의 생성 proto와 배포 DLL 의존성을 사용한다. Java는 배포 jar의
생성 proto를 사용하며 원본 provider class를 제거한 jar 사본에 새 class를 넣는다. 기준 provider와
각 변형은 별도 process로 실행한다. 언어별 대조군이 대상 case 여섯 개를 모두 통과해야 그 언어의
mutation을 실행한다. 대상 구문이 없거나 build가 실패하면 `not-built`이며, assertion 실패만
`detected`다. 결과에는 `language`를 기록하며 build 로그는
`ZLINK_ETCD_TEST_LOG_DIR/mutation-build/<language>/<mutation 또는 control>/`에 보존한다.

## C++와 4×4 실행

Linux에서 Node.js 22, .NET SDK 8, JDK 25, CMake, Ninja, C++20 compiler와 Docker daemon을
사용한다. Node workspace 의존성과 저장소가 지정한 Core·binding package, vcpkg 의존성은 먼저
설치한다. C++ build directory는 다음과 같이 구성한다. `VCPKG_ROOT`, `VCPKG_INSTALLED_DIR`와
`ZLINK_LOCAL_PACKAGE_ROOT`는 준비한 의존성의 절대 경로다.
`ZLINK_LOCAL_PACKAGE_ROOT`가 C++ 전용 cache이면 .NET·Java의 로컬 feed 경로도 먼저 만든다.
빈 feed는 공개 NuGet·Maven 저장소의 의존성 사용을 막지 않는다.

```sh
mkdir -p "$ZLINK_LOCAL_PACKAGE_ROOT/nuget" "$ZLINK_LOCAL_PACKAGE_ROOT/maven"
cmake -S framework/languages/cpp -B /build/cpp -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/.github/workflows/framework-cpp-system-toolchain.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-linux-zlink-release \
  -DZLINK_FRAMEWORK_CPP_WITH_ETCD=ON \
  -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF \
  -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=OFF
CMAKE_BUILD_PARALLEL_LEVEL=1 \
  bash scripts/etcd/build-and-run-4x4.sh /build/cpp /build/conformance --allow-unverified
```

`build-and-run-4x4.sh`는 네 adapter를 build하고 `/build/conformance/adapters.json`을 만든 뒤
공통 실행기를 실행한다. C++ build directory는 호출자가 configure한 static build 경로를 받으며, 두 번째 인자는
.NET adapter 산출물·설정·로그의 출력 경로다. Node와 Java는 각 workspace의 표준 build 경로를
사용한다. Build 출력은 log directory의 `*-build.log`, 실행 출력은 `conformance.log`에 저장한다.
`ZLINK_ETCD_TEST_LOG_DIR`로 로그 위치를 지정할 수 있다. 나머지 인자는 공통 실행기에 전달한다.
`--allow-unverified`는 현재 host 시계 case의 VM 준비 실패를 허용하지 않는다.
매일 밤 `nightly-stress.yml`의 C++ worker 1개 job이 GitHub-hosted runner에서 자기 build directory로
QEMU guest를 준비한 뒤 이 script를 실행하고, log directory를 artifact `etcd-4x4-conformance-logs`로 올린다.

C++ target `zlink_location_store_provider_adapter`는 `WITH_ETCD=ON`에서만 생성하며 ctest에
등록하지 않는다. 별도 adapter 설정을 사용할 때는 `cpp.command`에 이 실행 파일의 절대 경로,
`cpp.env.ZLINK_ETCD_CPP_BUILD_DIR`에 기준 CMake build directory의 절대 경로를 지정한다.
`build-cpp-provider.cjs`는 provider source의 격리 사본과 adapter를 별도 directory에서 build한다.
기준 build의 생성 protobuf object와 공통 라이브러리를 재사용하며 대조군도 같은 절차로 만든다.
대상 구문 불일치와 build 실패는 `not-built`이며, provider 생산 코드는 변경하지 않는다.

두 교차언어 ID는 각각 16개 writer×reader pair를 실행한다. Pair namespace와 35개 ID의
판정·coverage 규칙은 기존 실행기가 소유한다. 다음 명령으로 저장된 결과를 다시 검사한다.

```sh
node scripts/etcd/check-conformance-coverage.mjs --allow-unverified \
  /build/conformance/logs/node-conformance.json /build/conformance/logs/dotnet-conformance.json \
  /build/conformance/logs/jvm-conformance.json /build/conformance/logs/cpp-conformance.json
```

Container에서 실행할 때는 Docker socket과 etcd의 host 공개 port에 접근할 수 있어야 한다.
Docker Desktop의 host loopback port를 container에서 직접 쓸 수 없으면 container loopback에서
`host.docker.internal`로 TCP 전달기를 구성한다. 전달기는 bytes만 전달하며 case나 oracle을
해석하지 않는다. C++ mutation build는 메모리 사용을 제한하도록 순차 실행한다. Cache는 compiler,
vcpkg revision·triplet, Core·binding version이 같은 산출물만 재사용한다. 기존 `x64-linux` cache는
`x64-linux-zlink-release`와 이름을 바꾸어 혼용하지 않으며, 기존 cache 검증에는 configure 시
`-DVCPKG_TARGET_TRIPLET=x64-linux`를 명시한다.

Docker Desktop에서는 Node 의존성과 package build 산출물을 Linux filesystem에 둔다.
Workspace 사본을 Linux volume에 두거나, 동일한 package build 산출물을 복사해
`ZLINK_ETCD_PROVIDER_DIST`와 `ZLINK_ETCD_NODE_MODULES`로 지정한다. 후자는 provider의
companion package인 Framework·stream-wire 산출물과 그 의존성도 함께 배치해야 한다.

Docker 안에서 실행할 때는 runner의 저장소 절대 경로가 host Docker daemon에서도 같은 bind mount
경로를 가리켜야 한다. Docker socket과 host network를 연결하고, SDK·JDK·Node·Docker CLI를 준비한다.
시계 VM을 준비하지 못하면 host 시계 case는 실패한다.

`grpc-proxy.mjs`는 공용 etcd proto의 KV·Auth unary RPC를 실제 etcd로 전달한다. 실행기는
`startProxy({target})`로 proxy를 만들고 `arm({boundary, key|prefix})`로 경계를 선택한다.
`entered`는 해당 경계 도착을 알리고 `release()`는 전달을 재개한다. `loseResponse()`는 해당
응답을 유실시키며 `disconnect()`는 연결을 종료한다. 요청·응답의 protobuf bytes는 그대로 전달하며
디코딩은 선택과 기록에만 사용한다. 시험 cluster의 plaintext HTTP/2 endpoint를 사용한다.

| Boundary            | 보류 지점                                                       |
| ------------------- | --------------------------------------------------------------- |
| `apply-before`      | Put 또는 Delete를 포함한 Txn의 upstream 전달 전                 |
| `txn-response`      | 적용 Txn 응답의 client 전달 전                                  |
| `metadata-response` | 선택한 metadata key 범위의 Range 응답 전달 전                   |
| `cleanup-before`    | Delete를 포함한 정리 Txn의 upstream 전달 전                     |
| `clock-next`        | 선택한 clock Range 응답을 전달한 뒤 같은 key의 다음 RPC 전달 전 |

세 host 시계 case는 기존 provider adapter와 단일 Node 관리 CLI를 공통 QEMU guest에서 실행한다.
etcd와 관찰자는 guest 밖에 둔다. VM 설정 누락·시작 실패·시계 변경 미적용은 `--allow-unverified`와
관계없이 실패한다. Snapshot 유효기간 검사는 같은 instance에서 실제 5분을 기다린다.

Linux에서 같은 버전의 kernel·module tree, static BusyBox와 Dropbear·공유 library prefix를 준비한다.
호스트 package를 설치하지 않고 다음 명령으로 initramfs를 만든다.

```sh
bash scripts/etcd/prepare-clock-vm.sh OUTPUT KERNEL MODULES_ROOT KERNEL_VERSION STATIC_BUSYBOX DROPBEAR_PREFIX
export ZLINK_CLOCK_VM_CONFIG=/absolute/OUTPUT/config.json
node --test scripts/etcd/clock-vm-self-test.mjs
ZLINK_ETCD_ADAPTER_CONFIG=/absolute/adapters.json node scripts/etcd/clock-vm-test.mjs
```

생성한 config에는 `kernel`, `initrd`, `privateKey` 경로가 있다. 선택 항목 `qemu`, `accel`(기본 `kvm`),
`args`, `env`로 QEMU 설치 위치와 실행 환경을 지정한다. `hostRoot`는 QEMU process에서
읽기 전용으로 내보낼 호스트 root의 경로이며 기본값은 `/`다. KVM 권한이 없으면 `tcg`로 실행할 수 있다.
Guest는 호스트 tree를 읽기 전용으로 연결하고 `/tmp`, `/run`, `/root`에는 guest 전용 tmpfs를 쓴다.
호스트 시각과 시각 동기 도구는 변경하지 않는다. Adapter 명령과 산출물은 guest에서도 같은 절대 경로로
접근할 수 있어야 한다. User-mode network에서 호스트 loopback endpoint는 `10.0.2.2`로 연결한다.
반복 시험은 설정한 언어마다 세 항목을 40회 실행하며 adapter 설정을 생략하면 Node만 실행한다.
시계 근거에는 guest boot ID, realtime·monotonic 관측 쌍, provider 관측 결과와 case 완료 기록을 남긴다.
일반 conformance 실행기도 같은 executor를 사용한다.

Node 전용 `location-etcd-host-clock.test.js`는 `ZLINK_ETCD_INTEGRATION=1`일 때 provider와
관리 CLI process에만 `Date.now` preloader를 적용한다. 일 단위 시계 변경의 live·expired read,
조건 write, snapshot page와 관리 시각 증가 상한을 검사한다. 이 보조 시험은 process 시각을 대체하므로
OS 시계 격리의 실행 근거로 쓰지 않는다.

`ZLINK_ETCD_TEST_LOG_DIR`는 결과·명령 출력·manager log·proxy 관찰·member log의 저장 위치다.
기본값은 `scripts/etcd/.conformance-logs/`다. `unverified.json`은 미검증 목록이며
`cleanup.json`은 container·network·process·임시 디렉터리 잔여 수를 기록한다.

## Client capability 결과

2026-11-15에 코드와 GitHub Actions 원본 로그를 대조했다. Client 선택·버전·lockfile·license의 소유 문서는 [C++](../../framework/languages/cpp/extensions/framework-locations-etcd/README.ko.md#etcd-client), [.NET](../../framework/languages/dotnet/src/Zlink.Framework.Locations.Etcd/README.ko.md#etcd-client), [Java](../../framework/languages/java/zlink-framework-locations-etcd/README.ko.md#etcd-client), [Node](../../framework/languages/node/packages/framework-locations-etcd/README.ko.md#etcd-client)의 `etcd client` 절이다.

표의 F·C·N·D·J·K는 아래 명령과 결과 원본을 가리킨다. 명령은 준비된 build에서 관련 시험을 선택하는 재현 명령이며, 이 문서 작업에서는 실행하지 않았다. `없음`은 해당 capability 시험을 찾지 못했다는 뜻이다. `실행 근거 없음`은 시험 코드는 있지만 확인한 CI 로그에 그 시험의 실행 결과가 없다는 뜻이다.

- **F** — [run 43650235394, job 132616840063](https://github.com/zlink-systems/zlink/actions/runs/43650235394/job/132616840063), `Run the etcd 4x4 cross-language conformance`: 성공. 명령: `bash scripts/etcd/build-and-run-4x4.sh framework/languages/cpp/build/linux-ninja-release "$RUNNER_TEMP/etcd-4x4" --allow-unverified`. 표의 이름은 [공통 case 구현](conformance-cases.cjs)의 ID다. 원본 job 로그는 합계만 출력한다. F의 개별 case 통과는 성공한 실행과 현재 case·coverage 코드로부터의 간접 확인이며, per-case artifact를 직접 읽은 결과가 아니다.
- **C** — 같은 run·job의 `test_cpp_framework_locations_etcd`: ctest에서 `Passed`. 선택 명령: `ctest --test-dir framework/languages/cpp/build/linux-ninja-release -R '^test_cpp_framework_locations_etcd$' --output-on-failure`. 표의 `EtcdProvider.*`·`EtcdTls.*`는 [suite의 gtest 이름](../../framework/languages/cpp/extensions/framework-locations-etcd/tests/test_etcd.cpp)이다. 성공한 suite에 포함되지만 원본 로그에는 개별 gtest 결과가 없다. 환경 변수가 필요한 `EtcdLive`는 이 근거에 포함하지 않는다.
- **N** — [run 43650235394, job 132616839962](https://github.com/zlink-systems/zlink/actions/runs/43650235394/job/132616839962), `Run Node framework release gate`: 성공. 원본 TAP 로그에 아래 시험 이름의 `ok`가 있다. Node workspace에서 `node --test test/contract/location-etcd-admin.test.js test/contract/location-etcd-client.test.js test/contract/location-etcd-provider.test.js`로 선택한다.
- **D** — [run 44695707953, job 136153120822](https://github.com/zlink-systems/zlink/actions/runs/44695707953/job/136153120822), `linux-x64`: `Zlink.Framework.Locations.Etcd.Tests.dll` net8.0·net10.0 모두 `Passed!`. 이 시험은 channel이 service config를 받지 않고 송신 16 MiB·수신 8 MiB 설정값을 쓰는지 확인하며, 경계값 전송은 하지 않는다.
- **J** — [run 44695703519, job 136151823591](https://github.com/zlink-systems/zlink/actions/runs/44695703519/job/136151823591), `linux-x64 / Java 25`: 표의 `ZLinkEtcdTransportLimitTest` 두 시험 `PASSED`.
- **K** — [run 44695712497, job 136151854029](https://github.com/zlink-systems/zlink/actions/runs/44695712497/job/136151854029), `linux-x64 / node22`: 표의 두 시험 이름의 `ok`.
- D·J·K는 2026-11-20 `fix/etcd-sizes` 브랜치(#4077 크기 경계 시험)의 Framework .NET·Java·Node run이다.

위 nightly run 전체의 결론은 `failure`다. C++ 시험 job과 Node 시험 job은 성공했으며, 별도 sanitizer job이 실패했다. F는 plaintext etcd harness다. C와 N의 TLS·auth는 fake gRPC server 시험이며 보안 설정을 켠 실제 etcd harness의 결과가 아니다.

| Capability | C++ | .NET | Java | Node |
|---|---|---|---|---|
| Txn Compare VALUE LESS | F: `etcd.expiry-before-apply` | F: `etcd.expiry-before-apply` | F: `etcd.expiry-before-apply` | F: `etcd.expiry-before-apply` |
| Txn Compare CREATE | F: `spi.atomic-conflict` | F: `spi.atomic-conflict` | F: `spi.atomic-conflict` | F: `spi.atomic-conflict` |
| Txn Compare MOD | F: `etcd.condition-change-before-apply` | F: `etcd.condition-change-before-apply` | F: `etcd.condition-change-before-apply` | F: `etcd.condition-change-before-apply` |
| Revision-pinned Range | F: `etcd.snapshot-stability` | F: `etcd.snapshot-stability` | F: `etcd.snapshot-stability` | F: `etcd.snapshot-stability` |
| Linearizable reads | F: `etcd.minority-isolation` | F: `etcd.minority-isolation` | F: `etcd.minority-isolation` | F: `etcd.minority-isolation` |
| Header cluster_id / revision | F: `etcd.cross-language-record`; `etcd.cross-language-version-condition` | F: `etcd.cross-language-record`; `etcd.cross-language-version-condition` | F: `etcd.cross-language-record`; `etcd.cross-language-version-condition` | F: `etcd.cross-language-record`; `etcd.cross-language-version-condition` |
| ErrCompacted | F: `etcd.compacted-snapshot` | F: `etcd.compacted-snapshot` | F: `etcd.compacted-snapshot` | F: `etcd.compacted-snapshot` |
| NOSPACE | F: `etcd.quota-exhaustion` | F: `etcd.quota-exhaustion` | F: `etcd.quota-exhaustion` | F: `etcd.quota-exhaustion` |
| Transport errors | F: `etcd.leader-loss-before-result` | F: `etcd.leader-loss-before-result` | F: `etcd.leader-loss-before-result` | F: `etcd.leader-loss-before-result` |
| Lost Txn reply: no hidden retry | F: `etcd.leader-loss-before-result`; `spi.unknown-commit-reconstruction` | F: `etcd.leader-loss-before-result`; `spi.unknown-commit-reconstruction` | F: `etcd.leader-loss-before-result`; `spi.unknown-commit-reconstruction` | F: `etcd.leader-loss-before-result`; `spi.unknown-commit-reconstruction` |
| `keys_only=true` | 없음 | 없음 | 없음 | 없음 |
| TLS | C: `EtcdTls.MutualTlsCustomCaAndIdentityHostNameAndTrustValidation` | 실행 근거 없음: `EtcdTlsTests.Custom_trusted_CA_and_client_certificate_chain_complete_mutual_TLS`, `Untrusted_server_certificate_fails_before_the_RPC_is_received` | 실행 근거 없음: `ZLinkEtcdTlsTest.customCaAndClientIdentityEstablishMutualTlsAndUntrustedCaFails` | N: `etcd TLS uses normalized HTTPS URLs and supports trusted CA plus client identity` |
| Auth | C: `EtcdProvider.AuthenticationRefreshesOnlyOnceForRecognizedDescriptions`; `EtcdProvider.AuthenticationAndAllWriteCallsShareAbsoluteDeadline` | 실행 근거 없음: `Authentication_rejection_reauthenticates_once_with_the_same_deadline`, `A_second_authentication_rejection_is_not_retried` | 실행 근거 없음: `authenticationErrorReauthenticatesAndResendsOnlyOnce`, `authenticationAndAllWriteRpcsShareOneAbsoluteDeadline` | N: `only a rejected authentication RPC is replayed once, within the original deadline` |
| 16 MiB send boundary | C: `EtcdProvider.MaximumSendLimitUsesStoreFailureBeforeServerDispatch` | D: `EtcdTransportLimitTests.Channels_take_no_service_config_and_use_the_spec_bounds` (channel 설정값) | J: `ZLinkEtcdTransportLimitTest.sendLimitAcceptsExactly16MiBAndFailsOneByteMoreBeforeDispatch` | K: `location-etcd-client.test.js` 'etcd send limit accepts exactly 16 MiB and fails one byte more before dispatch' |
| 8 MiB receive boundary | C: `EtcdProvider.ReceiveLimitAcceptsFiveMiBAndRejectsNineMiBAuthenticationResponses` | D: `EtcdTransportLimitTests.Channels_take_no_service_config_and_use_the_spec_bounds` (channel 설정값) | J: `ZLinkEtcdTransportLimitTest.receiveLimitAcceptsExactly8MiBAndFailsOneByteMore` | K: `location-etcd-client.test.js` 'etcd receive limit accepts exactly 8 MiB and fails one byte more' |
| Cancellation | 없음 (physical RPC 취소); C의 `EtcdProvider.ConsumerWaitCancellationDoesNotCancelOrRetirePhysicalOperation`은 wait 취소 뒤 RPC 유지를 검사한다. | F: `spi.unknown-commit-reconstruction` | F: `spi.unknown-commit-reconstruction` | N: `etcd cancellation cancels a pending RPC; dispose drains and rejects new operations`; F: `spi.unknown-commit-reconstruction` |

.NET·Java의 단위시험 선택 명령은 각각 `dotnet test framework/languages/dotnet/tests/Zlink.Framework.Locations.Etcd.Tests/Zlink.Framework.Locations.Etcd.Tests.csproj`와 Java workspace의 `./gradlew :zlink-framework-locations-etcd:test`다. 확인한 nightly의 .NET job은 다른 test project를, Java job은 core·integration·Spring starter task를 실행하므로 provider 단위시험 결과로 쓰지 않는다. 시험 코드는 [.NET provider](../../framework/languages/dotnet/tests/Zlink.Framework.Locations.Etcd.Tests/EtcdProviderTests.cs), [.NET TLS](../../framework/languages/dotnet/tests/Zlink.Framework.Locations.Etcd.Tests/EtcdTlsTests.cs), [Java provider](../../framework/languages/java/zlink-framework-locations-etcd/src/test/java/systems/zlink/framework/locations/etcd/ZLinkEtcdProviderTest.java), [Java TLS](../../framework/languages/java/zlink-framework-locations-etcd/src/test/java/systems/zlink/framework/locations/etcd/ZLinkEtcdTlsTest.java)에 있다.

F의 cancellation case는 .NET·Java·Node에서 전송 후 abort와 unknown commit을 확인한다. C++에서는 [conformance-cases.cjs:635](conformance-cases.cjs)의 `proxy.disconnect()`를 쓰므로 RPC 취소 capability의 근거가 아니다. 크기 설정과 최대 SPI batch 통과는 client 송수신 제한의 양쪽 경계 시험을 대체하지 않는다. C의 send 시험은 15 MiB 수락·17 MiB 거부, receive 시험은 5 MiB 수락·9 MiB 거부를 검사한다. 정확히 16 MiB·8 MiB의 직전/직후 경계 실측과는 구분한다.

### 남은 작업

- 항목 3: 네 언어의 `keys_only=true`, .NET·Java·Node의 송수신 크기 양쪽 경계, C++의 직접 RPC 취소 시험을 추가한다. 기존 .NET·Java TLS·auth 단위시험의 실행 결과도 기록한다. 실제 TLS·auth-enabled etcd harness 결과와 fake server 결과를 구분한다.
- 항목 5·6: 고정한 resolved 그래프와 capability 실행을 연결하고, .NET·Java lockfile 및 C++·.NET·Java의 전이 의존성 license 목록을 기록한다. .NET의 retry·hedging 비활성화는 관찰된 lost-reply 비재전송 결과와 별도로 선택 transport의 설정·계약 근거가 필요하다.
- C++ pinning: Conan의 etcd-enabled gRPC lock entry와 vcpkg baseline·실제 gRPC/protobuf 해석 버전이 없다. 버전 고정은 별도 결정이며 이 작업에서 build·lockfile을 바꾸지 않는다.
- 과거 F 실행은 host clock 세 ID를 검증하지 않았다. 현재 실행은 위 QEMU 설정을 요구하며 guest 시계 관측 근거를 보존한다.
