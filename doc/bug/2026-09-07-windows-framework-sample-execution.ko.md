# Windows Framework 빌드·샘플 실행 검증

이 보고서는 Windows에서 확인한 빌드·실행 결과와 실패의 재현 조건을 Framework 유지보수자에게 전달한다.
빌드 성공과 실행 성공은 별도로 판정한다. 사용자 승인에 따라 빌드에 필요한 최소 컴파일 호환성 수정은 허용한다.
실행 중 동기화 문제를 해결하기 위한 Core·bindings·Framework 동작 변경은 범위에 포함하지 않는다.

## 검증 환경

- Windows 작업 경로: `D:/project/zlink`, branch `main`, 검증 시작 revision `84e528cc542b1ee87d590fcc843f3f639b701adf`.
- 최종 동기화 기준은 WSL에서 가져온 `832db1d08f`다. 이 구간에 Framework·Core·C++ binding include/src 변경은 없다. Windows origin SSH 인증 실패로 WSL의 최신 main을 가져와 fast-forward했다.
- Core 입력: `core/v0.17.0`, commit `3beab147a6fc27ceb913a782f77201cb9429f901`.
- 로컬 패키지 경로: `.artifacts/win-core-v0.17.0/.artifacts/windows`.
- 공통 Windows Core DLL SHA256: `4049f3ef323b165a6a155da19a880c9a25878d1cc4062e2cb57191f3309b772e`.
- Framework binding 참조는 0.17.0이다. main의 root VERSION/BINDINGS_VERSION은 0.17.1이므로 main에서 만든 패키지를 0.17.0으로 간주하면 안 된다.
- Java 비교용 `/MT` DLL은 별도 prefix `install/zlink-core-java-mt/0.17.0`와 별도 저장소 `.artifacts/win-core-v0.17.0/.artifacts/windows-java-mt/maven`을 사용한다.

## 빌드 결과

| 언어 | Windows Framework·샘플 빌드 | WSL 비교 |
|---|---|---|
| Node.js | Framework 및 샘플 7개 성공 | Framework·샘플 7개·공용 브라우저 성공 |
| .NET | Framework 프로젝트 9개 및 샘플 7개 성공 | 이번 비교 미실행 |
| Java/Kotlin | Framework 및 Java/Kotlin 각 7개 샘플 성공 | 이번 비교 미실행 |
| C++ | Framework·샘플 executable 28개 및 관련 테스트 15개 성공 | 동일 소스 수정으로 Framework·샘플 전수 빌드 및 관련 테스트 성공 |

## 실행 결과

| 언어 | 성공 확인 | 실패 또는 미검증 |
|---|---|---|
| Node.js | Windows에서 샘플 7개 전체 실행 성공 | ZoneWorld의 앞선 실행에서는 timeout이 있었으나 이후 전체 실행 성공 |
| .NET | TicTacToe 성공, SupportChat 재시도 후 성공 | Bingo, DeliveryDispatch, GameQuest, ShoppingMall ready timeout. ZoneWorld native Windows 실행 미검증 |
| Java/Kotlin | `/MT` 비교에서 초기 JVM 충돌이 사라짐 | DeliveryDispatch, TicTacToe 완료 미확인. Java/Kotlin ZoneWorld Windows 실행기 미완료 |
| C++ | 빌드 및 관련 단위 테스트 성공 | TicTacToe API 프로세스 비정상 종료, 나머지 샘플 실행 미검증 |

SupportChat의 성공은 최초 실행부터 안정적으로 성공했다는 의미가 아니다. 자동 재시도로 실패를 숨기는 변경은 최종 .NET 통합 실행기에 포함하지 않는다.

Windows Node 실행 명령은 `framework/languages/node/samples/run_samples.ps1 -SkipFrameworkBuild`다.
이 실행에서 TicTacToe, Bingo, DeliveryDispatch, SupportChat, GameQuest, ShoppingMall, ZoneWorld의 PASS를 확인했다.
ZoneWorld의 보존 로그는 `%TEMP%/zlink-sample-logs/zlink-zoneworld-nFlEKP/command-1.log`다.

## .NET ready timeout

아래 경로는 해당 Windows 사용자의 `%TEMP%` 아래에 보존된 실행 디렉터리다.

| 샘플 | 실행 디렉터리 | 마지막으로 확인한 실패 |
|---|---|---|
| Bingo | `bingo-dotnet-ca2ce42d9e21459e815f28fafe04291c` | `session-a mesh=room` ready 미충족 |
| DeliveryDispatch | `deliverydispatch-dotnet-998050970cd84b0184f51427632eaadf` | `logs/courier-session.out.log`의 `deliverydispatch-ready kind=route node=courier-session` 대기 실패 |
| ShoppingMall | `shoppingmall-dotnet-96471c393c3843f29046c81974125c29` | `shoppingmall-ready kind=object-route node=api-a target=workflow-a` 대기 실패 |
| GameQuest | `gamequest-dotnet-6cfa05f5cae9471d9f9da326f9c30708` | `gamequest-ready kind=spot-route node=api-b mesh=gamequest` 대기 실패 |

Bingo는 `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`로 확인했다. 로그에서 peer routing ID에 대응하는 예상 endpoint·lifecycle과 admission payload의 endpoint·lifecycle이 일치하지 않아 `route_mismatch`로 거부되는 사례가 있다.
한 사례의 예상 endpoint는 `tcp://127.0.0.1:23780`, payload endpoint는 `tcp://127.0.0.1:23781`이다.
이는 거부 이유에 대한 증거이며 Core의 메시지 손상이나 binding의 동시성 결함을 확정하는 증거는 아니다.
Core·binding·Framework 중 어느 계층에서 불일치가 시작되는지는 최소 재현으로 분리하지 못했다.

DeliveryDispatch의 Python 실행 파일 의존성과 잘못된 `.log` 파일명, GameQuest의 PowerShell 5.1 `Kill(true)` 호출 문제는 실행기 문제다.
표의 timeout은 해당 실행기 문제를 수정한 뒤 관찰한 결과다. 따라서 앞선 스크립트 오류와 동일한 실패로 분류하지 않는다.

## C++ TicTacToe 프로세스 종료

최종 Windows 빌드 스크립트의 exit code는 0이며 샘플 executable 28개를 확인했다.
이 산출물로 TicTacToe 실행기를 실행했으나 `api-a`가 시작 직후 종료돼 channel ready 대기가 30초 후 실패했다.
프로세스 stdout/stderr는 비어 있었다. Windows Application Error event 1000에는 2026-09-07 18:56:35,
`sample_cpp_framework_tictactoe_api.exe`, `ucrtbase.dll`, exception `0xc0000409`가 기록됐다.
WER event 1001은 BEX64로 분류했다.

실행 로그는 `.artifacts/windows/run/cpp-tictactoe-0.17.0`, 실행기의 임시 로그는
`C:/Users/hep7/AppData/Local/Temp/d638036c-734c-4065-92a6-b729492bda96`에 있다.
실행 시 0.17.0 Core·C++ prefix와 vcpkg bin을 PATH 앞에 지정했다.
단순한 executable 미생성이나 DLL 검색 실패 대신 시작한 프로세스의 비정상 종료가 관찰됐지만, 정확한 native stack은 확보하지 못했다.
Java의 native 충돌과 동일한 원인이라고 판정하지 않는다. Core 또는 Framework 동기화 결함 여부도 미확정이다.

## JVM native 충돌 및 실행 실패

Java 22와 `/MD` Core 0.17.0 조합에서 `EXCEPTION_ACCESS_VIOLATION`을 관찰했다.
JVM 로그는 `framework/languages/java/samples/java/DeliveryDispatch/hs_err_pid19936.log`에 있다.
problematic frame은 `msvcp140.dll`, Java 호출 경로는 `NativeContext.setUInt64Option -> NativeContext.<init>`이다.
별도 `/MT` Core로 초기 충돌이 사라져 CRT 로딩 충돌이 유력하지만, 정확한 DLL 로딩 경로와 최소 재현을 통한 원인 확정은 미완료다.

Java용 `/MT` DLL SHA256은 `922d1803fde648221aaa9a4343d64195c628224b51ec0ada0081246a5a0e0d3e`다.
이 DLL의 imports에서 MSVCP/VCRUNTIME 의존성이 없음을 확인했다.
`/MT`에서 DeliveryDispatch 역할 6개가 초기 ready에 도달했으나 courier-node-2의 dispatch peer-route 완료는 확인하지 못했다.
TicTacToe에서는 Redis 연결 거부와 bind 실패가 관찰됐다. 설정 파일 BOM과 cleanup 문제도 있어 이를 라이브러리 결함으로 단정하지 않는다.

## 스크립트와 검증 범위

Windows 진입점은 언어별 `framework/languages/<language>/build-windows.ps1`와 `samples/run_samples.ps1`이다.
빌드·실행 지원용 PowerShell/Node 스크립트, 패키지 경로 설정과 승인된 C++ 컴파일 호환성 수정을 포함한다.
Node의 package.json 변경은 esbuild 실행 명령의 Windows 호환성을 위한 것으로 dependency version은 변경하지 않는다.

C++ 최종 빌드 재현 명령:

```powershell
& framework/languages/cpp/build-windows.ps1 `
  -BuildDir D:/project/zlink/.artifacts/windows/build/framework-cpp-patched-0.17.0 `
  -LocalPackageRoot D:/project/zlink/.artifacts/cpp-clean-0.17.0-package `
  -VcpkgInstalledDir D:/project/zlink/.artifacts/windows-vcpkg-installed `
  -Configuration Release -Parallel 8
```

이 스크립트는 기본 빌드에서 제외되는 샘플 서버까지 명시적으로 빌드한다.
샘플 executable은 위 BuildDir의 `Release` 아래에 생성된다.
Core 입력은 0.17.0이며 C++ binding에는 승인된 컴파일 호환성 패치가 포함돼 있다.
Core 소스 자체는 수정하지 않는다.
binding의 변경 기록은 `.artifacts/cpp-clean-0.17.0-package/install/zlink-cpp/0.17.0/share/zlink/cpp-package-provenance.json`에 있으며 `label=patched-0.17.0`, `source.dirty=true`다.
C++ 빌드 콘솔 출력은 별도 파일로 저장하지 않았으므로 이 보고서에서는 실행 명령·종료 코드 확인과 산출물 경로를 근거로 남긴다.

.NET 재현 명령:

```powershell
& framework/languages/dotnet/build-windows.ps1 `
  -LocalPackageRoot D:/project/zlink/.artifacts/win-core-v0.17.0/.artifacts/windows
$env:ZLINK_LOCAL_PACKAGE_ROOT = 'D:/project/zlink/.artifacts/win-core-v0.17.0/.artifacts/windows'
& framework/languages/dotnet/samples/Bingo/run_sample.ps1
```

.NET Framework 프로젝트 9개와 샘플 solution 7개 Release 빌드는 성공했다.
로그는 `.artifacts/windows/logs/dotnet-build-20260907-181518`에 있다.
별도 NuGet cache에서 `Systems.Zlink/0.17.0`을 사용했고 package 내부 Windows DLL SHA256이 위 Core DLL과 일치함을 확인했다.
통합 실행기의 `-LocalPackageRoot` 인자로 TicTacToe를 다시 실행해 `tictactoe-placement=completed`와 exit code 0을 확인했다.
로그는 `.artifacts/windows/logs/dotnet-tictactoe-final.log`다.

Java의 수정된 contract test로 얻은 22/22 결과는 최종 원본 테스트의 성공 증적으로 사용하지 않는다.
Java/Kotlin 최종 빌드는 원본 binding에서 외부 Gradle init script로 새로 생성한 `/MD` Maven 패키지를 사용한다.
`scripts/local-package/java/windows-package.init.gradle`이 Windows DLL과 provenance의 패키징을 담당하며 binding build.gradle은 수정하지 않는다.
새 `zlink-0.17.0.jar`의 SHA256은 `a50affc6720a6b263e769cab1330e61d40b225c3c151736fb753ec472befeb30`이다.
`build-windows.ps1 -LocalPackageRoot D:/project/zlink/.artifacts/win-core-v0.17.0/.artifacts/windows`로 Framework `assemble`과 전체 샘플 `installDist`가 성공했다.
Java/Kotlin 각 7개 샘플의 배포 디렉터리 50개에서 jar SHA256이 새 Maven 패키지와 일치함을 확인했다. 이 빌드 명령은 테스트와 샘플 실행을 포함하지 않는다.
C++의 private using 선언을 제거한 설치 헤더로 얻은 빌드 결과는 원본 binding 검증으로 사용하지 않는다.
최종 C++ 검증은 사용자 승인을 받은 컴파일 호환성 패치를 적용한 0.17.0 패키지를 대상으로 하며, 원본 태그 그대로의 성공과 구분한다.

## C++ 빌드 차단 오류

MSVC 19.44에서 원본 0.17.0 C++ binding configure는 성공하지만 build가 실패한다.
첫 오류는 `bindings/cpp/include/zlink/Contracts/Sockets/pubsub_socket_contracts.hpp:91,158`의 `C2668`이다.
`subscription_at` 호출 후보를 컴파일러가 구분하지 못한다. `/permissive-` 적용 후에도 같은 오류가 발생한다.
추가로 지정한 `/Zc:twoPhase`는 이 컴파일러가 unknown option으로 무시했다.
LLVM/MinGW는 설치되어 있지 않아 다른 컴파일러에서의 결과는 확인하지 않았다.

검증용 소스는 `.artifacts/cpp-clean-0.17.0-source`, 빌드 디렉터리는 `.artifacts/cpp-clean-0.17.0-package/build/bindings-cpp-0.17.0`이다.
검증 소스를 추출한 `ccb48dd599`와 `core/v0.17.0`의 해당 header blob은 `a291ca74fe45c9b3914405b48bbf5fb767904d23`으로 동일하다.
양쪽 C++ CMakeLists.txt blob도 `a48ab0e001c47f5b74cfa2970880b2af51addf6a`로 동일하다.
따라서 이 오류를 검증용 추출 revision 차이로 설명할 수 없다.

원본 소스 실험에서는 binding package 생성이 막힌다. 사용자 승인에 따라 `private using` 중복 선언을 제거한 패키지는 빌드·설치에 성공했다.
Framework 쪽 호환성 수정은 기존 friend wrapper 사용, Windows `gmtime_s` 분기와 MSVC의 비대입 가능 결과 타입에 대한 promise 저장 방식으로 한정한다.
ZoneWorld의 ZoneNode 샘플에도 같은 Windows `gmtime_s` 분기를 적용한다.
일반 결과 타입·void·참조 및 비-MSVC 경로는 기존 `std::future`를 유지한다. MSVC 문제 타입에만 별도 소유 저장소를 사용한다.
Windows C++은 Release 구성에서 `/Od`로 최적화를 비활성화하고 `/bigobj`를 적용한다. 이 산출물은 최적화된 Release 성능 측정 기준으로 사용하지 않는다.

## WSL 비교 범위

WSL 경로는 `/home/hep7/project/zlink`, branch `main`, 확인 revision은 `832db1d08f`다.
검증 worktree는 순수 checkout이 아니라 Node package.json 6개와 실행·빌드 지원 `.mjs` 3개의 변경 및 C++ 컴파일 호환성 수정을 적용한 dirty 상태다.
Node Framework, 샘플 7개, ZoneWorld 공용 브라우저 빌드는 성공했다. 이번 비교에서 샘플 실행은 수행하지 않았다.
따라서 Windows ready timeout이 Windows에서만 발생한다고 결론 내릴 수 없다.

C++은 같은 binding·Framework 소스 변경으로 0.17.0 패키지 빌드·설치와 Framework·샘플 executable 28개 빌드가 성공했다.
패키지 검증에서 Core 0.17.0은 수락하고 Core 0.16.0은 거부했다.
최종 CMake 테스트 링크 설정까지 반영한 `test_cpp_framework_state_lane` 직접 실행은 15/15, ctest는 1/1 통과했다.
Windows와 WSL의 최종 CMakeLists.txt·state_lane.hpp·해당 테스트 파일 hash가 일치함을 확인했다.
추가 ZoneWorld ZoneNode 분기도 파일 hash 일치를 확인했고 해당 타깃의 증분 컴파일·링크가 성공했다.

WSL `npm ci`는 HTTP tgz와 lockfile integrity 불일치로 실패한다.
다음 명령으로 패키지를 명시 설치한 뒤 빌드는 성공했으며 lockfile은 수정하지 않았다.

```bash
npm install --no-save --no-package-lock --ignore-scripts --no-audit --no-fund \
  /home/hep7/project/zlink/.artifacts/wsl/npm/zlink-systems-zlink-0.17.0.tgz \
  /home/hep7/project/zlink/.artifacts/wsl/npm/zlink-systems-http-client-0.10.0.tgz
npm run build
for sample in TicTacToe.Ts Bingo.Ts DeliveryDispatch.Ts SupportChat.Ts GameQuest.Ts ShoppingMall.Ts ZoneWorld; do
  npm --prefix "samples/$sample" run build || exit 1
done
cd ../shared_sample/zoneworld/client
npm ci --ignore-scripts --no-audit --no-fund && npm run build
```

WSL binding tgz에는 `libzlink.so.0.17.0`이 포함돼 있다. 다만 provenance가 `source.dirty=true`, `tag=local/build-dev`이므로 Windows의 exact tag 빌드와 동일한 입력이라고 보장하지 않는다.
