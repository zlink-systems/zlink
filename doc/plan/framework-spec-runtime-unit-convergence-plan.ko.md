# Framework frozen spec 구현·unit/contract 수렴 실행 계획

## 1. 문서의 역할

이 문서는 2026-08-15에 확정한 Server Framework spec을 기준으로 C++, .NET,
Java/Kotlin, Node Framework 구현과 unit/contract test를 완전히 수렴시키는 단일 실행
계획이다. 작업자는 과거 대화, 삭제된 `99` 문서, 별도 gap 문서나 기존 build
결과를 전제로 삼지 않는다. 이 문서, frozen spec, 현재 source와 새로 실행한
test만으로 작업을 시작하고 완료할 수 있어야 한다.

이 문서는 규범 문서가 아니다. 규범의 유일한 원본은 다음 경로다.

```text
framework/doc/framework/common/spec/server/
```

진행 중에는 위 경로의 KO/EN 문서를 생성·수정·삭제·이동하지 않는다. 구현과
spec이 충돌하면 구현과 test를 고친다. spec 자체가 서로 모순되어 구현 결정을
내릴 수 없는 경우에만 작업을 중단하고 정확한 `file:line`과 양쪽 해석을
보고한다. 임의 해석이나 spec 수정으로 계속 진행하지 않는다.

## 2. 현재 기준 상태

### 2.1 Git과 frozen spec

- 작업 기준 branch는 `main`이다.
- 문서 작성 직전 `main`과 `origin/main`은
  `0ca0d47996eba637b645dc168a06d840131d0a5d`로 같았다.
- Server spec freeze 기준 commit은 `a131a52d3e`이다.
- frozen spec은 KO/EN 123쌍, orphan 0, heading/fenced-code 구조 차이 0으로
  검증됐다.
- Framework 문서 link 12,845개, generated language guide, identifier, tab,
  prose 검사와 contract validator는 freeze 시점에 통과했다.
- 삭제된 구현-gap 문서와 `99` 문서는 정본도 작업 입력도 아니다. 다시 만들거나
  참조하지 않는다.

기존 untracked 항목은 사용자 소유다. 특히 다음 항목을 삭제·이동·stage·commit하지
않는다.

```text
"%ln | sort"
complete
core/builds/auto-hwm-attach/
core_token.commit
reply
route_client
seal_application
write_and_flush
```

`core/builds/auto-hwm-attach/` 아래에는 다수의 untracked build 산출물이 있다.
개별 파일 수와 관계없이 디렉터리 전체를 사용자 소유로 취급한다.

### 2.2 검증 증거의 한계

Core와 binding은 이전 수렴 과정에서 다음 결과가 보고됐다.

- Core Debug와 Release+LTO: 각각 87/87
- C binding: 7/7
- C++ binding: 13/13
- .NET binding: 168/168
- Go binding: 전체 package test 통과
- Java binding: unit 105, integration 3, Netty 3 통과
- Node binding: test-only 58 통과, 의도된 1 skip
- Python binding: 92/92
- Rust binding: 131/131

그러나 위 결과와 현재 HEAD 사이에 commit이 더 들어갔고, 현재
`.artifacts/wsl/install/zlink-core/0.11.1/share/zlink/core-package-provenance.json`
은 source revision `408d6abf40d`, `dirty=true`를 기록한다. 따라서 기존
`.artifacts/wsl` package와 과거 test log는 현재 작업의 최종 통과 증거가 아니다.
작업 시작 시 local Core와 C++/.NET/Java/Node binding package를 현재 HEAD에서
다시 만들어야 한다.

현재 local Core package producer는 provenance의 `source.dirty`를 항상 `true`로
기록한다. 그러므로 이 필드는 freshness 판정에 사용하지 않는다. 현재 HEAD와
`source.revision`의 일치, `verify-package.sh`의 file/runtime hash 검증, 최종 tracked
worktree clean을 함께 사용한다.

Framework는 중간 상태에서 다음 build가 성공한 적이 있다.

- C++ Release production target 9개와 unit/contract executable 42개 compile/link
- .NET production project 9개와 test project 5개 build
- Node workspace와 browser bundle build, test TypeScript typecheck
- JVM Java/Kotlin production module assemble와 test/contract source compile

이 역시 현재 HEAD의 전체 unit/contract green을 뜻하지 않는다. 최종 baseline에서
네 runtime 계열의 전체 in-scope unit/contract suite를 한 번에 통과시킨 증거는 아직
없다. 이것이 이 계획의 남은 핵심 작업이다.

### 2.3 현재 확인된 첫 gap

다음 두 항목은 2026-08-15 read-only 감사에서 확인한 첫 P0 gap이다. 전체 gap
목록이 아니며, 두 항목을 고친 뒤에도 6장의 전수 감사를 계속해야 한다.

1. C++ shared `Relocate` operation 합류 기준

   - Frozen exact contract:
     `framework/doc/framework/common/spec/server/languages/cpp/interfaces/02-configuration-host.en.md:103`
   - 현재 owner:
     `framework/languages/cpp/framework/src/runtime/host/app.cpp:2990`
   - `relocation_options_t` equality:
     `framework/languages/cpp/framework/include/zlink/framework/contracts/configuration/lifecycle.hpp:49`
   - 같은 mode와 effective target인데 deadline만 다른 두 번째 caller는 기존
     operation에 합류하고 첫 operation의 deadline을 따라야 한다. 현재 구현은
     deadline까지 options equality에 포함하고 active operation 확인 전에 후속
     deadline으로 preflight를 다시 수행한다.
   - 첫 RED는
     `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6c_runtime.cpp`
     의 host relocation fixture에 작성한다. 첫 call을 readiness gate에서 멈추고
     다른 deadline의 두 번째 call을 넣어 실제 relocation 1회와 동일 terminal을
     검증한다.

2. C++ relocation target의 실제 선택 기준

   - Frozen exact contract:
     같은 C++ configuration 문서의 target-selection 절
   - Preflight owner:
     `framework/languages/cpp/framework/src/runtime/host/app.cpp:2769`
   - 실제 selector:
     `framework/languages/cpp/framework/src/runtime/host/app.cpp:2909`
   - 현재 실제 선택은 preflight가 검사한 maintenance wave, relocation
     policy/adapter, population/reservation capacity, descriptor와 Core peer의
     RID/generation 및 Admitted/Ready, placement weight를 동일하게 재사용하지
     않는다.
   - 첫 RED는 앞의 m6c fixture를 세 node로 확장한다. list 앞에는 부적격 target,
     뒤에는 적격 target을 두고 적격 target만 선택되는지 검증한다. weight는
     sleep이나 반복 확률 test 대신 deterministic draw seam으로 0-weight 제외와
     positive-weight 선택을 검증한다.

.NET, Java와 Node는 같은 mode/effective target 합류 구현이 존재하지만 서로 다른
deadline의 합류를 명시적으로 고정한 regression evidence가 약하다. 각 언어의
maintenance runtime test에 같은 public behavior test를 추가하여 네 언어 parity를
고정한다.

## 3. 작업 범위

### 3.1 포함 범위

- C++ Framework public surface와 runtime
- .NET Framework public surface와 runtime
- Java Framework, Kotlin projection/adapter와 공용 JVM runtime
- Node Framework public surface와 runtime
- 위 구현을 검증하는 unit test와 contract test
- 변경 owner에 필요한 build configuration과 test fixture
- frozen spec에 없는 API, option, status, metric, compatibility path, retry,
  queue, cap, ACK 또는 fallback의 제거
- 기능 제거 뒤 남는 dead code, pass-through helper, 중복 DTO/adapter와 obsolete
  test의 정리
- focused green 이후의 POSDDD refactoring
- Framework 구현에 필요한 Core/binding public 기능이 이미 해당 Core/binding
  frozen contract에 존재하지만 구현만 빠진 경우의 최소 prerequisite 수정

### 3.2 제외 범위

- `framework/doc/framework/common/spec/server/**` 전체
- 모든 spec 문서의 생성·수정·삭제·이동
- `framework/languages/*/e2e*`, `framework/languages/*/samples*`
- E2E/sample application, fixture, runner와 그 전용 regression test
- E2E와 sample의 실행
- benchmark, perf harness와 수치 calibration
- frozen spec에 없는 새 기능, 새 compatibility surface와 deprecation alias
- 별도 gap 문서, `99` 문서, 장기 ledger의 생성
- unrelated failure의 opportunistic 수정

Static validator가 E2E/sample 파일의 존재를 읽는 것은 실행으로 보지 않지만,
이번 작업에서 그 파일이나 전용 expectation을 고치지 않는다. 일반 solution이나
`check` 명령이 E2E/sample을 끌어오면 사용하지 말고 아래의 분리된 명령을 쓴다.

## 4. 절대 규칙

1. `main`에서만 수정·commit·push한다. branch를 만들거나 전환하지 않는다.
2. 시작할 때와 push 직전에 `main == origin/main`을 확인한다. remote가 앞섰거나
   갈라졌으면 merge/rebase/reset하지 말고 중단하여 보고한다.
3. frozen spec은 수정하지 않는다. test를 통과시키기 위해 규범을 완화하지 않는다.
4. 구현이나 기존 test를 gap 목록으로 사용하지 않는다. frozen spec의 observable
   contract가 우선한다.
5. 기능 test는 public interface, result/error, ownership, lifecycle, deadline,
   cancellation과 backpressure를 검증한다. private field, helper 이름, source
   문자열, 내부 call count만 고정하는 test를 새로 만들지 않는다.
6. spec에 없는 동작은 compatibility를 이유로 유지하지 않는다. 해당 public
   surface, runtime branch, metric/status, test를 함께 제거한다.
7. sample, E2E, manual codec, raw frame 해석, test-only transport나 별도 queue로
   owner 문제를 우회하지 않는다.
8. hard-coded sleep으로 readiness나 callback 종료를 추정하지 않는다. monitor,
   barrier, injected clock, deterministic selector와 explicit completion을 쓴다.
9. 첫 실제 failure에서 멈춰 원인을 분류한다. 같은 원인으로 full suite를 반복하지
   않는다.
10. 관련 focused test가 green이 되기 전에는 구조 refactoring을 섞지 않는다.

## 5. 작업 시작 절차

Repository root에서 실행한다.

```bash
set -euo pipefail
cd /home/hep7/project/zlink

git fetch origin
test "$(git branch --show-current)" = main
test "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)"

# tracked 변경이 없어야 한다. 위 8개 untracked 항목은 그대로 둔다.
test -z "$(git status --porcelain=v1 --untracked-files=no)"

# freeze commit 이후 server spec 변경이 없어야 한다.
git diff --exit-code a131a52d3e..HEAD -- \
  framework/doc/framework/common/spec/server
git diff --exit-code -- framework/doc/framework/common/spec/server
git diff --cached --exit-code -- \
  framework/doc/framework/common/spec/server

scripts/local-package/build-wsl.sh --verify-versions
scripts/local-package/build-wsl.sh --core-source local cpp dotnet java node
scripts/local-package/http-client/build-wsl.sh dotnet java node

CORE_PREFIX="$PWD/.artifacts/wsl/install/zlink-core/$(bash core/version.sh)"
bash scripts/local-package/core/verify-package.sh --prefix "$CORE_PREFIX"

node - "$CORE_PREFIX/share/zlink/core-package-provenance.json" \
  "$(git rev-parse HEAD)" <<'NODE'
const fs = require('node:fs');
const [manifestPath, expected] = process.argv.slice(2);
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
if (manifest.source?.revision !== expected) {
  throw new Error(
    `stale Core package: ${manifest.source?.revision} != ${expected}`
  );
}
NODE
```

`--core-source local`은 package용 Core build를 다시 구성하고 binding native payload를
동기화할 수 있다. 생성된 package/native 산출물을 제품 변경과 함께 stage하지
않는다. HEAD가 바뀌었거나 Core/binding source가 바뀐 뒤 final gate를 실행할 때는
package를 다시 만든다.

### 5.1 현재 HEAD prerequisite baseline

기존 log를 재사용하지 않고 현재 package로 Core public surface와 Framework가 직접
사용하는 네 binding의 unit/contract baseline을 한 번 확인한다. sample runner는
호출하지 않는다.

```bash
# Core public surface focused gate
cmake -S core -B .artifacts/wsl/build/core-public-surface \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_SHARED=ON -DBUILD_STATIC=ON \
  -DBUILD_TESTS=ON -DZLINK_BUILD_TESTS=ON \
  -DBUILD_BENCHMARKS=OFF \
  -DZLINK_CORE_BUILD_BINDING_INTEGRATION_TESTS=OFF \
  -DWITH_DOCS=OFF
cmake --build .artifacts/wsl/build/core-public-surface \
  --target libzlink --parallel "$(nproc)"
ctest --test-dir .artifacts/wsl/build/core-public-surface \
  --output-on-failure --stop-on-failure \
  -R '^contract_public_surface$' -j1

# C++ binding contract
cmake -S bindings/cpp -B .artifacts/wsl/build/binding-cpp-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DZLINK_CPP_CORE_PACKAGE_PREFIX="$CORE_PREFIX" \
  -DZLINK_CPP_BUILD_TESTS=ON \
  -DZLINK_CPP_REGISTER_CTEST=ON \
  -DZLINK_CPP_BUILD_SAMPLES=OFF \
  -DZLINK_CPP_BUILD_BENCHMARKS=OFF
cmake --build .artifacts/wsl/build/binding-cpp-tests --parallel 2
ctest --test-dir .artifacts/wsl/build/binding-cpp-tests \
  --output-on-failure --stop-on-failure -L contract

# .NET binding unit/contract
export ZLINK_LIBRARY_PATH="$CORE_PREFIX/lib/libzlink.so"
export NUGET_PACKAGES="$PWD/.artifacts/wsl/cache/binding-nuget-$(git rev-parse --short=12 HEAD)"
dotnet test bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj -c Release

# Java binding unit/contract; JDK 22
(
  cd bindings/java
  ./gradlew :test :integrationTest :zlink-ext-netty:test \
    --no-daemon --no-parallel --max-workers=1 --fail-fast
)

# Node binding unit/contract; Node 22 이상. sample은 환경 변수로 차단한다.
(
  cd bindings/node
  export ZLINK_CORE_INSTALL_PREFIX="$CORE_PREFIX"
  npm ci
  ZLINK_BINDING_RAW_TEST_ONLY=1 npm test
)
```

여기서 failure가 나면 Framework 작업을 시작하지 않는다. `PACKAGE_STALE`, 실제
Core/binding regression 또는 environment로 먼저 분류한다.

## 6. Gap 확인 절차

별도 gap 문서는 만들지 않는다. 다음 순서를 각 주제에 적용하고, 확인 결과는 해당
수정 commit의 message와 최종 완료 보고에 남긴다.

### 6.1 주제 순서

1. registration, configuration, default, validation과 startup snapshot
2. Core HWM, Application Job Queue, admission, backpressure, reset과 metric
3. message model, serializer, malformed input, reply/error completion
4. channel, stream, client-server, fanout와 logical multicast
5. Spot, Actor, Session, location, Store와 relocation
6. drain, shutdown, cancellation, timeout, ownership와 terminal cleanup
7. monitoring, status, error mapping, tracing과 correlation
8. C++/.NET/Java/Kotlin/Node exact public surface와 numeric type parity
9. internal owner, queue, lifecycle와 forbidden compatibility path

각 주제에서 다음 작업을 모두 수행한다.

1. `server/README.ko.md`의 주제 목차에서 common KO 문서와 40~52 internal
   contract를 찾는다.
2. 같은 주제의 EN 문서와 `server/languages/{cpp,dotnet,java,kotlin,node}` exact
   interface 문서를 읽는다.
3. public symbol, default/range, startup owner, observable state transition,
   result/error, timeout/cancel, permit/byte ownership과 terminal 조건을 추출한다.
4. 네 runtime 계열의 public declaration, configuration wiring, runtime owner와
   test를 각각 대조한다. Kotlin은 Java runtime 재사용과 Kotlin projection을 모두
   확인한다.
5. 구현·test·spec이 일치하면 다음 항목으로 간다. test evidence만 없으면 public
   behavior regression을 추가한다.
6. 구현이 다르면 기존 test가 green이어도 새 RED를 먼저 만든다.
7. spec에 없는 구현이면 test를 완화하지 말고 surface, branch, 상태와 obsolete
   test를 함께 제거한다.

다음 검색은 후보를 좁히는 수단일 뿐이며 검색 결과만으로 일치 판정을 내리지 않는다.

```bash
rg -n 'MUST|MUST NOT|반드시|이어야|해서는 안|금지|default|기본값|timeout|deadline|cancel|ownership|permit|metric' \
  framework/doc/framework/common/spec/server

rg -n 'deprecated|legacy|compat|fallback|retry|ack|queue|capacity|timeout|deadline' \
  framework/languages/{cpp,dotnet,java,node}
```

### 6.2 Gap 증거 형식

수정을 시작하기 전에 다음 네 근거를 terminal update나 commit body에 적는다.

```text
Spec: frozen file:line과 observable requirement
Owner: public declaration과 runtime owner file:symbol
RED: 실패하는 unit/contract test와 실제/기대 결과
Gate: focused command와 이후 full language command
```

근거가 네 항목을 충족하지 않으면 구현을 바꾸지 않는다.

## 7. 구현과 POSDDD 수렴 절차

한 번에 하나의 contract/owner slice만 처리한다.

1. public/functional RED를 추가하고 focused failure를 확인한다.
2. 최소 owner에서 behavior를 수정한다. caller, sample, adapter에 우회 정책을 넣지
   않는다.
3. focused test를 green으로 만든다.
4. 그 green 상태에서만 POSDDD refactoring을 수행한다.
5. refactoring 후 같은 focused test를 다시 실행한다.
6. 같은 subsystem unit/contract test를 실행한다.
7. language full in-scope unit/contract suite와 Release build를 실행한다.
8. exact 파일만 stage하고 diff를 검토한 뒤 commit·push한다.

비자명한 변경은 최소 두 설계를 비교하고 다음 기준으로 owner가 깊은 쪽을 택한다.

- 한 design decision에 owner가 하나인가.
- public interface가 단순하고 complexity가 owner 내부로 흡수되는가.
- pass-through layer, duplicate DTO/adapter, configuration escape hatch가 생기지
  않는가.
- lifecycle의 시간 순서를 module boundary로 잘못 복제하지 않는가.
- Core/binding error, timeout, cancellation, ownership 의미가 Framework 경계에서
  새 의미로 변하지 않는가.
- Infrastructure adapter에 domain policy가 새지 않는가.
- C++/.NET/Java/Kotlin/Node public 이름과 의미가 exact contract와 같은가.

다음 냄새가 생기면 commit 전에 정리한다.

- 한 줄 delegation만 하는 shallow module
- 같은 eligibility/validation을 preflight와 actual operation에 복제
- public option으로 내부 owner 결정을 노출
- return type이나 exception으로 내부 transport 세부를 유출
- data-only object와 여러 곳에 흩어진 behavior
- temporary flag, debug print, dead branch, stale TODO
- spec에 없는 alias, overload, ACK, retry, pending queue 또는 fallback

## 8. Test 작성 규칙

- Public surface는 compile/header/API snapshot 또는 language reflection contract로
  검증한다.
- Behavior는 public call의 result, error kind, status/metric과 lifecycle transition으로
  검증한다.
- Retained payload와 permit은 handler 첫 instruction, reply submit terminal,
  cancellation/shutdown terminal에서 exact-once release를 검증한다.
- FIFO, no-barging, 1:N fanout은 deterministic barrier와 injected scheduler로
  검증한다.
- Timeout은 injected clock/scheduler 또는 bounded explicit event를 사용한다.
- Selector는 deterministic draw seam을 사용하고 확률 반복 test를 쓰지 않는다.
- 제거 대상은 old symbol이 public snapshot에 없고 old behavior가 observable하지
  않음을 검증한다. private helper 이름 부재만 검증하지 않는다.
- 기존 test가 spec 밖 compatibility를 요구하면 삭제하거나 frozen behavior test로
  교체한다. expectation만 약하게 바꾸지 않는다.

## 9. 언어별 명령

모든 명령은 `/home/hep7/project/zlink` 기준이다. Focused test는 정확한 test 이름으로
먼저 실행한다. `sample`, `e2e`, `perf`, browser E2E는 실행하지 않는다.

### 9.1 C++

```bash
cmake -S framework/languages/cpp \
  -B .artifacts/wsl/build/framework-cpp-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT="$PWD/.artifacts/wsl" \
  -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=ON \
  -DZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS=ON \
  -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_E2E=OFF \
  -DZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT=OFF

cmake --build .artifacts/wsl/build/framework-cpp-debug \
  --parallel "$(nproc)"

# 현재 첫 P0 focused gate
ctest --test-dir .artifacts/wsl/build/framework-cpp-debug \
  --output-on-failure --stop-on-failure \
  -R '^test_cpp_framework_m6c_runtime$' -j1

# 전체 in-scope unit/contract
ctest --test-dir .artifacts/wsl/build/framework-cpp-debug \
  --output-on-failure --stop-on-failure \
  -L 'framework-(unit|contract)' -LE 'e2e|sample|perf' -j1

# Release production build-only
cmake -S framework/languages/cpp \
  -B .artifacts/wsl/build/framework-cpp-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT="$PWD/.artifacts/wsl" \
  -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_E2E=OFF \
  -DZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT=OFF

cmake --build .artifacts/wsl/build/framework-cpp-release \
  --parallel "$(nproc)"
```

Mixed-label binary를 고친 경우 그 binary만 focused 실행하고 결과를 명시한다. 전체
gate에 넣기 위해 E2E/sample label을 제거하거나 runner를 실행하지 않는다.

### 9.2 .NET

Solution 전체는 E2E/sample project를 포함하므로 사용하지 않는다.

```bash
export NUGET_PACKAGES="$PWD/.artifacts/wsl/cache/framework-nuget-$(git rev-parse --short=12 HEAD)"

projects=(
  framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj
  framework/languages/dotnet/tests/Zlink.Framework.ContractTests/Zlink.Framework.ContractTests.csproj
  framework/languages/dotnet/tests/Systems.Zlink.Stream.Connector.Tests/Systems.Zlink.Stream.Connector.Tests.csproj
  framework/languages/dotnet/tests/Zlink.Framework.Locations.Redis.Tests/Zlink.Framework.Locations.Redis.Tests.csproj
)

for project in "${projects[@]}"; do
  dotnet restore "$project" --force-evaluate
done

# Focused 예시
dotnet test "${projects[0]}" -c Debug -f net8.0 --no-restore \
  --filter 'FullyQualifiedName~EXACT_NAMESPACE.TYPE_OR_METHOD'

for project in "${projects[@]}"; do
  dotnet test "$project" -c Debug -f net8.0 --no-restore
done

# HTTP client owner를 바꾼 경우 이 project도 추가한다.
dotnet test framework/languages/dotnet/tests/Zlink.HttpClient.UnitTests/Zlink.HttpClient.UnitTests.csproj \
  -c Debug -f net8.0

# Production project만 Release build한다.
while IFS= read -r project; do
  dotnet build "$project" -c Release -f net8.0
done < <(find framework/languages/dotnet/src -name '*.csproj' -print | LC_ALL=C sort)
```

`Zlink.Framework.SampleRegressionTests`와 `e2e/**` project는 실행하지 않는다.

### 9.3 Java/Kotlin

JDK 22를 사용한다. `check`는 `sampleTest`를 연결하므로 실행하지 않는다.

```bash
export GRADLE_USER_HOME="$PWD/.artifacts/wsl/cache/framework-gradle-$(git rev-parse --short=12 HEAD)"
export ZLINK_LIBRARY_PATH="$CORE_PREFIX/lib/libzlink.so"

# Root Gradle script는 core/build runtime이 있으면 이를 우선한다. package build가
# 만든 runtime과 정확히 같은지 확인해 stale runtime 실행을 막는다.
test "$(sha256sum "$ZLINK_LIBRARY_PATH" | awk '{print $1}')" = \
     "$(sha256sum core/build/lib/libzlink.so | awk '{print $1}')"

cd framework/languages/java

# Focused Java unit
./gradlew :zlink-framework-core:test \
  --tests 'systems.zlink.framework.EXACT_TEST' \
  --no-daemon --no-parallel --max-workers=1 --fail-fast

# Focused exact contract; Kotlin contract도 해당 module task로 같은 방식으로 실행한다.
./gradlew :zlink-framework-core:contractTest \
  --tests 'systems.zlink.framework.EXACT_CONTRACT_TEST' \
  --no-daemon --no-parallel --max-workers=1 --fail-fast

# 전체 in-scope Java/Kotlin unit/contract. testkit contract task는 E2E/sample 전용
# contract를 섞으므로 먼저 제외한다.
./gradlew test contractTest -x :zlink-framework-testkit:contractTest \
  --no-daemon --no-parallel --max-workers=1 --fail-fast

# testkit에서 server runtime에 속하는 두 contract만 별도로 실행한다.
./gradlew :zlink-framework-testkit:contractTest \
  --tests 'systems.zlink.framework.testkit.ConnectorCodecContractTest' \
  --tests 'systems.zlink.framework.testkit.FrameworkModuleBoundaryTest' \
  --no-daemon --no-parallel --max-workers=1 --fail-fast

# Production build-only
./gradlew assemble \
  --no-daemon --no-parallel --max-workers=1

cd /home/hep7/project/zlink
```

변경한 owner에 `fakeBackendTest`나 in-process `integrationTest`가 직접 존재하면 해당
task를 focused/full로 추가한다. `sampleTest`, `samples/`, `e2e/`, `e2e-kotlin/`은
실행하지 않는다.

### 9.4 Node

`npm test`와 `verify:ci`는 browser/E2E 또는 sample lint를 포함할 수 있으므로 이번
범위의 full gate로 사용하지 않는다.

```bash
cd framework/languages/node
export npm_config_cache="$PWD/../../../.artifacts/wsl/cache/framework-npm-$(git rev-parse --short=12 HEAD)"

npm ci
npm run build

# Focused 예시
node --test --test-force-exit --test-timeout=600000 \
  test/contract/EXACT_TEST_FILE.test.js

# TypeScript milestone unit/contract
npm run verify:m5-foundation
npm run verify:m6a-runtime
npm run verify:m6b-runtime
npm run verify:m6c-runtime

# Browser, E2E/sample 전용 test를 제외한 전체 JS unit/contract
while IFS= read -r test_file; do
  case "$test_file" in
    */browser/*|*e2e*|*E2E*|*sample*|*Sample*)
      printf 'OUT-OF-SCOPE %s\n' "$test_file"
      continue
      ;;
  esac
  node --test --test-force-exit --test-timeout=600000 "$test_file"
done < <(
  find test/contract test/smoke -type f -name '*.test.js' -print |
    LC_ALL=C sort
)

node node_modules/eslint/bin/eslint.js 'packages/*/src/**/*.ts'
cd /home/hep7/project/zlink
```

출력된 `OUT-OF-SCOPE` 목록은 최종 보고에 그대로 남긴다. 다른 test를 임의 skip하지
않는다.

## 10. Core와 binding prerequisite

Framework만 바꿨다면 5장의 package freshness와 Framework gate만 수행한다.
Core/binding source는 frozen Framework contract를 구현하는 데 필요한 public 기능이
그 계층의 기존 contract에도 이미 규정되어 있는데 구현만 없는 경우에만 바꾼다.
그 계층의 spec 수정이 필요해 보이면 진행 중 spec 변경 금지에 걸리므로 중단하고
보고한다.

Core를 바꾼 경우 Debug와 Release에서 benchmark와 binding integration을 제외하고
`unittest`, `integration`, `regression` label을 모두 실행한다. binding을 바꾼 경우
해당 binding의 sample을 제외한 full unit/contract gate를 실행한다. 공식
`bindings/*/tests/run_tests.sh`가 sample을 실행하면 사용하지 말고 language test
runner를 직접 실행한다. 통과 후 local package를 다시 만들고 Framework gate를
처음부터 재실행한다.

## 11. Spec 밖 코드 제거 감사

각 주제와 최종 단계에서 다음 항목을 검색하고 frozen spec 근거가 없는 것은 public
surface, runtime, test 순서로 함께 제거한다.

- old option/config builder와 alias
- duplicate status/metric/reset API
- terminal reply/error에 적용되는 application HWM/queue
- host-wide capacity보다 먼저 application을 reject하는 per-owner/per-loop cap
- 별도 relocation cap, target completion ACK와 per-record ACK
- source와 target 양쪽에 남은 legacy/bulk relocation path
- owner 바깥 hard-coded timeout/default
- async lifecycle을 가로지르는 retained credit 조기 release 또는 leak
- first handler instruction 전에 permit release하거나 그 이후까지 queue permit을 유지
- alternate retry queue, unbounded temporary workaround와 `int.MaxValue` 우회
- manual serializer/codec와 raw-frame caller policy
- obsolete feature flag, compatibility branch, debug output, stale TODO
- 사용처가 사라진 helper, DTO, adapter와 test fixture

반대로 단순 symbol 검색만으로 삭제하지 않는다. common spec, exact language spec와
실제 observable owner를 확인한다.

## 12. Commit과 push 절차

다음 시점마다 독립 commit을 만든다.

1. 한 contract의 RED와 owner fix가 focused/subsystem green인 시점
2. 같은 decision owner의 POSDDD refactoring이 green인 시점
3. 한 언어의 full unit/contract와 Release build가 green인 시점
4. 네 언어 최종 parity 정리가 끝난 시점

각 commit 전에 실행한다.

```bash
git diff --check
git diff -- framework/doc/framework/common/spec/server
git status --short

# 정확한 파일만 직접 나열한다. git add -A를 사용하지 않는다.
git add path/to/owned-source path/to/owned-test
git diff --cached --check
git diff --cached

git commit -m 'fix-framework-<owner>-<behavior>'

git fetch origin
test "$(git rev-parse HEAD^)" = "$(git rev-parse origin/main)" || {
  echo 'origin/main moved; stop without merge/rebase/reset' >&2
  exit 1
}
git push origin main
test "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)"
```

Push 직전 remote가 이동했다면 임의 merge/rebase하지 않는다. 강제 push, reset,
restore, broad checkout과 사용자 untracked 삭제는 금지한다.

## 13. Failure 분류와 중단 조건

첫 failure는 다음 중 하나로 분류한다.

- `SPEC_GAP`: frozen common/exact contract와 runtime observable behavior 불일치
- `TEST_GAP`: runtime은 맞지만 contract evidence가 없거나 test가 old behavior를 요구
- `PACKAGE_STALE`: provenance revision/runtime/header가 HEAD와 다름
- `ENVIRONMENT`: compiler/JDK/Node/service 등 재현 가능한 외부 prerequisite 부재
- `OUT_OF_SCOPE`: E2E/sample/perf 또는 변경 owner와 무관한 기존 failure

`SPEC_GAP`과 `TEST_GAP`만 현재 owner slice에서 수정한다. `PACKAGE_STALE`은 package를
재생성한다. `ENVIRONMENT`는 명령, version과 첫 error를 기록하고 환경을 바로잡는다.
`OUT_OF_SCOPE`는 expectation을 낮추거나 임의 수정하지 않고 즉시 보고한다. 다만
in-scope full suite에 unresolved failure가 하나라도 있으면 전체 작업은 완료가 아니다.

## 14. 최종 독립 리뷰

모든 language gate가 green인 뒤 구현에 참여하지 않은 별도 reviewer가 read-only로
다음을 다시 확인한다.

1. `server/README.ko.md`의 모든 주제를 common KO/EN, 40~52, 다섯 exact language
   tree와 대조했는가.
2. C++/.NET/JVM/Node의 public surface, defaults, validation, runtime state transition,
   error와 ownership이 같은가.
3. spec에 없는 surface/compatibility/queue/cap/ACK/retry가 남지 않았는가.
4. test가 public interface와 function을 검증하며 private implementation shape에
   묶이지 않았는가.
5. changed owner에 shallow layer, duplicate decision, leaky adapter와 dead code가
   남지 않았는가.
6. E2E/sample code와 runner가 변경·실행되지 않았는가.

Reviewer는 finding마다 frozen `file:line`, source/test owner와 재현 명령을 제시한다.
확인된 in-scope finding이 하나라도 남으면 수정→focused→language full gate→review를
반복한다. reviewer가 `in-scope spec/runtime/test gap 0`을 명시해야 완료 후보가 된다.

## 15. 최종 gate

언어별 명령을 모두 통과한 뒤 repository root에서 실행한다.

```bash
set -euo pipefail
cd /home/hep7/project/zlink

# Frozen spec 불변
git diff --exit-code a131a52d3e..HEAD -- \
  framework/doc/framework/common/spec/server
git diff --exit-code -- framework/doc/framework/common/spec/server
git diff --cached --exit-code -- \
  framework/doc/framework/common/spec/server

# Static contract와 schema. E2E는 inventoryOnly이며 실행하지 않는다.
bash scripts/verify-framework-doc-contracts.sh
node framework/runtime/protocol/validate-service-wire-schema.mjs
node framework/runtime/protocol/verify-service-wire-decoder-fixtures.mjs
node framework/runtime/conformance/validate-runtime-conformance-fixtures.mjs

python3 doc/site/scripts/generate_language_guides.py --check
python3 doc/site/scripts/check_doc_tabs.py framework
python3 doc/site/scripts/check_guide_identifiers.py
python3 doc/site/scripts/check_prose_neutrality.py
python3 doc/site/scripts/check_doc_links.py framework repo-doc core-doc

git diff --check
git status --short
git fetch origin
test "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)"
```

위 static contract script가 E2E/sample process를 시작하면 즉시 중단한다. 현재
baseline에서는 E2E를 `inventoryOnly`로 읽을 뿐 실행하지 않는다.

## 16. 완료 조건

다음 조건을 모두 충족해야만 완료다.

- [ ] Frozen server spec은 freeze commit 이후 byte-level 변경이 없다.
- [ ] 현재 HEAD에서 다시 만든 package로 Core public-surface와 C++/.NET/Java/Node
      binding prerequisite baseline이 통과했다.
- [ ] 모든 common 주제와 exact language contract를 네 runtime 계열에 대해 직접
      대조했다. 현재 알려진 두 C++ P0만 처리하고 종료하지 않았다.
- [ ] C++의 deadline-independent relocation join과 common target eligibility/weighted
      selector가 public functional test로 고정됐다.
- [ ] .NET, Java/Kotlin, Node에도 서로 다른 deadline join parity test가 있다.
- [ ] C++, .NET, Java/Kotlin, Node public surface와 observable runtime behavior가
      frozen spec과 일치한다.
- [ ] Frozen spec에 없는 API, option, status, metric, cap, queue, ACK, retry,
      compatibility/fallback과 obsolete test가 남지 않았다.
- [ ] 모든 새 test는 interface/function 중심이며 private implementation shape나
      sleep timing을 contract로 만들지 않는다.
- [ ] 네 언어의 전체 in-scope unit/contract suite가 skip, hang, failure 없이
      통과한다. 명시한 E2E/sample/browser 전용 test만 scope에서 제외한다.
- [ ] 네 언어 production Release build가 통과한다.
- [ ] Core/binding을 변경했다면 해당 계층 full unit/contract와 package freshness,
      Framework 재검증까지 통과한다.
- [ ] POSDDD audit 후 duplicate owner, shallow pass-through, leaky adapter, dead code,
      debug output와 stale TODO가 없다.
- [ ] E2E/sample source, fixture, test와 runner를 수정하거나 실행하지 않았다.
- [ ] final static contract/schema/link/diff gate가 모두 통과한다.
- [ ] 독립 reviewer가 unresolved in-scope spec/runtime/test finding 0을 확인했다.
- [ ] 모든 coherent change가 `main`에 commit·push됐고 `main == origin/main`이다.
- [ ] tracked worktree가 clean하며 기존 사용자 untracked 항목이 보존됐다.

실패, hang, 설명되지 않은 skip, package provenance mismatch, 미push commit,
unresolved review finding 중 하나라도 있으면 완료가 아니다. 시간 부족, 기존 구현이
대체로 맞아 보임, focused test 통과, build 성공만으로 완료 처리하지 않는다.

## 17. 완료 보고 형식

```text
Baseline/final commit:
Frozen spec integrity: PASS/FAIL

Gap audit:
- audited topics:
- confirmed gaps fixed:
- spec-less surfaces/behavior removed:
- independent review findings: 0 또는 잔여 목록

Language gates:
- C++ unit/contract, Release build:
- .NET unit/contract, Release build:
- Java/Kotlin unit/contract, assemble:
- Node unit/contract, build:

Prerequisite gates:
- Core/binding source changed: yes/no
- package provenance revision:
- changed prerequisite test results:

Scope protection:
- frozen spec changes: 0
- E2E/sample changes and executions: 0
- preserved pre-existing untracked roots:

POSDDD review:
- decision owner/refactoring:
- removed duplicate/shallow/dead code:

Git:
- commits pushed:
- main == origin/main:
- tracked worktree clean:
```

이 형식의 모든 항목에 실제 command 결과와 commit을 적을 수 있을 때만 작업을
종료한다.
