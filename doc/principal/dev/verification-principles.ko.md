# 검증 원칙 — 계기가 답하는 질문을 확인한다

> 이 문서는 "무엇을 확인해야 확인한 것이 되는가"를 소유한다. 무엇을 실행하는지(어떤 test,
> 어떤 gate)는 [`AGENTS.md`](../../../AGENTS.md) §4와
> [`CONTRIBUTING.ko.md`](../../../CONTRIBUTING.ko.md)가 소유한다. 이 문서는 그 규칙을
> 반복하지 않는다.
>
> **이 문서는 쌓인다.** 거짓 통과를 한 번 겪을 때마다 §3 표에 한 줄을 더한다. 사례가
> 늘어도 §1·§2의 규칙은 늘지 않아야 한다 — 늘어난다면 규칙이 잘못 쓰인 것이다.

## 1. 두 가지 규칙

**재현을 먼저 한다.** 결함을 자기 손으로 한 번 일으키지 못했으면 고쳤는지 알 수 없다.
재현 환경이 없으면 **만드는 비용과 CI 왕복 비용을 비교한다.** 대개 만드는 쪽이 싸다.

**검사는 양방향으로 시험한다.** "고친 뒤 통과"만 보면 그 검사가 무엇을 잡는지 아무도
모른다. **나쁜 입력을 일부러 만들어 실제로 잡히는지**까지 보고, 두 출력을 함께 보고한다.

## 2. 왜 필요한가

**도구는 네가 물은 질문이 아니라 자기가 아는 질문에 답한다.** 그 둘이 다르면 "고쳤다"가
거짓으로 통과하고, 같은 결함이 다음 회차에 다시 나온다. 그럴듯하게 관련은 있지만 같지
않은 것을 측정하기 때문에, 틀렸다는 신호가 나오지 않는다.

증상이 아니라 **계약**을 본다. "파일이 있다"가 아니라 "쓸 수 있다", "빌드된다"가 아니라
"실행된다"를 확인한다.

## 3. 겪은 것들

| 물은 것 | 쓴 계기 | 계기가 실제로 답한 것 | 출처 |
|---|---|---|---|
| 아카이브에 진짜 코드가 있나 | `nm`, `file` | 심볼 테이블이 있나 — LTO 오브젝트도 갖고 있다 | #397 |
| 컴파일러가 이 플래그를 받나 | `check_cxx_compiler_flag` | **다른 플래그 없이** 받나 — clang은 `-flto`와 함께일 때만 거부했다 | #397 |
| LTO 바이트코드인가 | `.text` 크기 0 | 코드가 없나 — 헤더만 include한 TU도 0이다 | #397 |
| 바이트코드가 섞였나 | `__gnu_lto_slim` 마커 | slim LTO인가 — fat 오브젝트에는 안 붙는다 | #397 |
| 이 아카이브를 쓸 수 있나 | 로컬 링크 성공 | **같은 툴체인끼리** 되나 — 원래 된다 | #397 |
| 심볼을 소비자에게서 숨겼나 | `-fvisibility=hidden` | 공유 오브젝트 내보내기를 숨겼나 — 정적 링크는 평평한 이름공간이라 그대로 보인다 | #418 |
| 역호출이 관리 코드에 닿나 | 핸들러 실행 횟수 | 핸들러가 몇 번 돌았나 — 등록 전이면 당연히 0이다 | #402 |
| 이 최적화 수준에서 동작하나 | 링크 성공 | 링크되나 — JSDCE가 지운 선언은 **실행 중에** 터진다 | #398 |
| 이 검사가 나쁜 입력을 잡나 | 좋은 입력에서 통과 | 좋은 입력을 통과시키나 — 아무것도 안 잡는 검사도 통과시킨다 | #397 |
| Redis live 시험이 돌았나 | 환경 변수를 켠 뒤 Gradle 성공 | 그 task를 다시 실행했나 — 입력이 같으면 UP-TO-DATE로 이전 skip 결과를 재사용한다. `--rerun`으로 강제하고 XML의 skip 수를 본다 | #1881 |
| C++ 회귀 assertion이 결함을 잡나 | 기존 Release library에 연결한 test 실행 성공 | `NDEBUG`가 assertion을 제거했으면 검사 자체가 실행되지 않는다. Compile 명령의 `-UNDEBUG`와 수정 전 실패를 확인한다 | #1207 |
| 모든 handler worker가 Store 대기 중인가 | executor 생성 뒤 worker 수를 지정하고 검사 통과 | 이미 생성된 executor의 worker 수는 바뀌지 않는다. 초기화 전에 수를 지정하고, Store 완료를 보류한 상태의 수정 전 실패를 확인한다 | #1380 |
| 심볼 지역화가 먹혔나 | 내 머신에서 통과 | **내 binutils에서** 먹히나 — 2.42는 되고 릴리스를 만드는 2.38은 안 됐다 | #418 |
| ld64가 이 심볼을 강등하나 | 컴파일 시점 hidden visibility | 그 TU의 보통 심볼을 숨기나 — 템플릿·인라인 static은 vague linkage라 안 닿는다 | #418 |
| 지정한 수신 계약 두 class가 모두 실행됐나 | Gradle `--tests` 두 개와 `BUILD SUCCESSFUL` | 존재하는 filter 한 개의 test만 실행돼도 성공한다. XML의 class별 실행 수까지 확인한다 | #1270 |
| .NET source 전체의 format이 맞나 | `format.sh --check dotnet`의 종료 코드 0 | 존재하는 source root에서 찾은 파일만 맞는다. WSL 사본에 `engines/Server`가 없어도 `find` 오류 뒤에 통과했다. 검사 root의 존재와 파일 수를 함께 확인한다 | #1082 |
| Node Store의 owner lease 교체가 삭제를 막나 | repository contract test 통과 | generic write wrapper가 fixture의 존재하지 않는 API 호출에서 발생한 `TypeError`까지 `Conflict`로 바꾸면 lease 교체 없이도 통과한다. fixture는 domain repository로 lease를 교체하고 provider 오류 보존을 별도로 검사한다 | #1083 |
| 신규 .NET 회귀 시험이 실행됐나 | filter 명령의 종료 코드 0 | `EnableDefaultCompileItems=false` 프로젝트의 compile 목록에 시험 파일이 없으면 실행 대상이 0건이다. compile 포함 여부와 실제 실행 건수를 확인한다 | #1083 |
| Reject reason 오해석을 회귀 test가 잡나 | Server role fixture에서 not-required peer 수 0 | Role policy가 not-required 표시를 거부했는지만 확인한다. 결함 분기가 실제로 표시할 수 있는 Client role fixture에서 수정 전 실패를 확보한다 | #1082 |
| reply의 별도 deadline이 종료를 결정했나 | clock 진행 뒤 ProtocolErrors 증가 | 최초 admission 거절도 같은 counter를 증가시킨다. clock 진행 전후의 제출 횟수와 reply claim 종료를 각각 확인한다 | #1213 |
| 원본·병합 source를 실제로 다시 빌드했나 | source를 덮은 뒤 test 통과 또는 link 실패 | 보존된 수정 시각 때문에 MSBuild·Ninja가 이전 DLL·object를 재사용할 수 있다. source와 산출물의 해시·시각을 확인하고 변경된 source를 다시 빌드해 결과를 판정한다 | #1083 |
| Node Store fixture가 lease 교체를 검증하나 | 가공한 claim 결과를 돌려준 뒤 test 통과 | Store에 lease를 기록하지 않으면 교체 경로가 실행되지 않는다. 기존 repository로 실제 lease를 확정하고 Store 상태와 결과를 함께 검사한다 | #1083 |
| 종료 중 target CAS가 settlement를 마쳤나 | CAS 진입 횟수 1과 종료 성공 | CAS 제출만 확인하며 commit 확인과 definitive conflict를 구분하지 못한다. 검증 선행 시험은 실제 Store 결과를, 응답 유실 시험은 적용 성공 뒤 확인 read를 각각 검사한다 | #1082 |
| Prefix scan이 무관한 key를 작업 한도에서 제외하나 | 무관한 key 130개와 page limit 100에서 통과 | 작업 한도는 page limit보다 클 수 있다. 무관한 key 수가 실제 작업 한도를 넘는 입력에서 수정 전 실패와 수정 후 통과를 확인한다 | #1082 |
| CTest에 등록된 선택 케이스가 실행됐나 | 실행 파일의 종료 코드 0과 CTest 통과 | 선택한 케이스를 삭제해도 실행 파일이 시험 0건으로 성공할 수 있다. 등록 이름과 실행 파일의 케이스 목록을 함께 대조한다 | #1452 |
| Windows GoogleMock 실패가 test 결과에 반영되나 | 등록된 test 실행과 종료 코드 0 | gmock.dll과 gtest.dll의 registry가 다르면 필수 mock 호출을 생략해도 통과한다. 공용 harness의 registry를 통합하고 의도적인 mock 실패의 포착과 종료 코드 1을 확인한다 | #1571 |
| C++ perf 단위 테스트가 assertion을 실행하나 | `perf_multi_metrics_test` 통과 | Release 구성의 `NDEBUG`가 `assert`를 지워 검사 없이 통과한다. test target에서 `NDEBUG`를 해제하고 수정 전 실패를 먼저 확인한다 | #1421 |
| 두 Core 빌드의 처리량 차이가 코드 차이인가 | 번갈아 측정에서 한쪽이 7% 낮음 | 같은 머신에서 같은 코드도 회차마다 6~14% 다르게 측정된다. 두 `libzlink.so`의 `.text`를 먼저 대조하고, 같은 빌드 회차 간 차이보다 큰 차이만 회귀 후보로 둔다 | #1421 |
| gate 스크립트가 실패한 단계를 종료 코드로 알리나 | gate 종료 코드 0 | 실패 집계 뒤에 `touch`가 실행되면 실패가 있어도 0으로 끝난다. 실패한 단계를 넣은 results 파일로 종료 코드가 0이 아닌지 확인하고, 판정은 results 파일로도 대조한다 | 0.30.0 gate |
| bench 포트가 비어 있나 | LISTEN 목록에 없음 | TIME_WAIT 포트나 다른 loopback 주소의 listener는 LISTEN 조회에서 빠지지만 실제 bind는 실패한다. 실제 bind로 확인하고 TIME_WAIT·LISTEN·빈 포트 세 경우를 모두 시험한다 | #1390 |
| 엔진 E2E가 server 종료까지 검증하나 | connector client 성공과 CTest 통과 | 하네스가 server 종료를 기다리지 않고 Redis를 제거하면 `ForceStopped/TeardownFailed`도 통과한다. Client 결과와 server 종료 결과를 함께 확인한다 | #1646 |
| Configure 거부 뒤 source가 복원됐나 | `RelocationFailed` reason만 일치 | Source lease가 만료돼 `Error`가 되어도 같은 reason이다. `Blocked/RelocationFailed`와 source `Serving`을 함께 단언한다 | #1877 |
| Session route 준비 전 READY가 막혔나 | stage 호출 직후 future 미완료와 READY 부재 | target stage가 아직 실행되지 않았어도 같은 결과다. command 52 처리 완료를 기다린 뒤 route 요청과 READY를 검사하고, 대기 호출을 뺀 구현에서 실패하는지 확인한다 | #1775 |
| Descriptor 충돌이 startup을 실패시키나 | `app.run`의 0이 아닌 종료 코드 | Handler group 설정 누락도 같은 종료 코드를 반환한다. 주입한 게시 포트의 호출 횟수와 host의 Error 상태를 함께 단언해, 다른 초기화 오류로 통과하지 않도록 한다 | #1962 |
| 끝난 member의 capacity가 반납됐나 | provider `list_mesh_nodes`의 descriptor capacity가 0 | 게시한 descriptor의 초기값이 0인지에만 답한다. 실제 capacity record의 수정 전 사용량과 수정 후 반환·예약 수를 함께 확인한다 | #1872 |
| 완료 callback 안의 단언 실패가 시험에 전달되나 | callback 실행 뒤 시험 통과 | dispatcher가 `AssertionError`를 잡으면 내부 단언이 실패해도 통과한다. callback은 관찰값을 기록하고 시험 thread에서 단언하거나, 실패를 시험이 기다리는 future로 전달한다 | #1770 |
| Framework E2E에서 handler 오류가 없었나 | Kotlin match update의 종료 코드 0과 `Scenarios: 1/1 passed` | 같은 회차의 JoinSpot message-flow에는 `INTERNAL_FAILURE`와 `ConcurrentModificationException`이 기록됐다. Program 결과와 서버의 예기치 않은 handler 오류를 함께 확인한다 | #1882 |
| 일반 Actor dispatch가 실행 gate를 쓰나 | 소스 전체에서 executor 호출 문자열 검색 | 무관한 disconnect 전용 호출만 있어도 통과한다. 일반 coordinator 호출과 header forwarding을 확인하고, 일반 호출을 지워 옛 전용 호출만 남긴 입력과 forwarding을 지운 입력이 실패하는지 대조한다 | #1883 |
| 실패한 incarnation 정리가 보류 메시지 완료보다 먼저 끝났나 | source reply 수신 순서 일치 | 전송 지연이 target의 조기 완료를 가릴 수 있다. 실제 destructor를 보류하고 target queue의 retained record와 completion 미완료를 확인한 뒤 release·join한다 | #1961 |
| Fixture를 기존 dispatch 경로로 바꾼 뒤에도 원래 순서 결함을 잡나 | 같은 단언으로 main에서 통과 | observer의 handler terminal 대기를 빼면 replay 게시와 이동 종료 사이 순서를 고정하지 못한다. 메시지 수락 뒤에 같은 observer를 설치하고 수정 전 반복 실패를 확인한다 | #1786 · #1771 |
| 모든 template 실행 파일이 최종 package를 link했나 | 전체 link 명령에 최종 archive 경로가 있다 | 한 실행 파일만 최종 archive를 사용해도 통과한다. 실행 파일마다 link 인자를 정확히 비교하고, 하나만 이전 archive를 사용하는 입력도 거부하는지 확인한다 | #1786 · #1771 |
| GoogleTest 시험이 실제로 실행됐나 | 실행 파일이 exit 0 | GTEST_SKIP도 exit 0이다. skipped summary가 있는 출력과 정상 출력을 함께 넣어 판정을 확인하고, skip을 통과 수에서 제외한다 | #1786 · #1771 |
| 검증 source hash가 stage된 변경도 포함하나 | `git ls-files --modified`로 만든 hash가 같다 | Stage된 변경은 빠진다. `git diff HEAD`와 untracked 파일을 함께 수집하고, stage된 실제 source의 두 내용을 입력해 hash가 달라지는지 확인한다 | #1786 · #1771 |
| Host가 정상 정산했나 | GTest 단언 통과와 exit 0 | 종료 로그에 `force_stopping`이 있으면 정상 drain의 증거가 아니다. pending·active callback 정산과 정상 종료 transition을 함께 확인한다 | #1786 · #1771 |
| WSL 시험이 실패를 종료 코드로 알렸나 | PowerShell에서 띄운 inline `bash -c`의 `$?`·`$code`가 0 | PowerShell이 변수를 먼저 펼쳐 gtest 실패에도 바깥 명령이 0으로 보였다. `.sh` 파일이나 subprocess의 returncode로 판정하고 로그의 실제 시험 수를 함께 확인한다 | #2119 |
| 생성 codec이 올바른 frame을 받나 | GCC 빌드의 golden 시험 통과 | 함수 인자 평가 순서에 기대는 코드(`batch_end(b, at, batch_r16(b, at))`)는 GCC에서만 통과하고 Clang에서는 정상 frame을 거부했다. 생성 코드는 GCC와 Clang으로 함께 빌드해 시험한다 | #2119 |

## 4. 무엇이 실제로 답을 줬나

- **다른 툴체인으로 진짜 링크하기** — 정적 아카이브의 LTO 결함은 이것만 잡았다.
  `g++-11`·`g++-13`이 설치돼 있다.
- **다른 라이브러리 버전과 섞어 진짜 실행하기** — 정적 아카이브의 Boost 누출은 이것만
  잡았다. vcpkg가 설치한 것을 쓰면 된다.
- **진짜 런타임에서 진짜 실행하기** — emscripten 링크가 통과해도 Unity IL2CPP에서 멈추는
  것(`ConfigureAwait(false)`)은 실제 플레이어를 돌려야 나왔다.
- **대조 실험** — 가설이 둘이고 증상이 같으면, 둘을 가르는 최소 실험을 설계한다. 가설마다
  한 회차를 쓰지 않는다.

**로컬 통과는 릴리스 툴체인 통과가 아니다.** 릴리스 아카이브를 만드는 러너의 컴파일러·
binutils 버전을 확인하고, 다르면 그 버전으로 재현한다. #418은 로컬(binutils 2.42)에서
통과한 뒤 릴리스 러너(2.38)에서 세 플랫폼이 깨졌다.

## 5. 검증 층을 세울 때

층을 나눌 거라면 **각 층이 아래 층은 잡을 수 없는 것을 잡아야** 값어치가 있다. Unity
WebGL 어댑터가 그 예다 — 흉내 하네스가 경계 논리를, 진짜 emscripten이 링크·역콜백·힙·
누수를, 진짜 Unity가 IL2CPP와 동기화 컨텍스트를 잡았다. 각 층이 실제로 아래 층이 못 잡는
결함을 하나씩 찾았다.

위 층이 아래 층과 같은 것만 확인한다면 그 층은 비용일 뿐이다.

## 6. 검증할 수 없는 플랫폼

로컬에서 돌릴 수 없는 플랫폼(macOS·Windows·ARM)이 남는다면, **틀렸을 때 조용히 잘못된
산출물이 나가는 대신 빌드가 실패하게** 만든다. 산출물을 만드는 스크립트가 끝에서 스스로를
검사하면 된다. #418의 세 플랫폼 실패가 그렇게 드러났다 — 검사가 없었으면 boost 심볼을
그대로 내보내는 아카이브가 릴리스로 나갔을 것이다.

보고에는 **무엇이 미검증인지 명확히 적는다.** "될 것이다"로 적지 않는다.
