# Core ROUTER callback dispatch 재연결 handover 버그

## 상태

- 확인일: 2026-07-13
- 상태: core 9.0.4에서 수정 완료
- 수정: commit `414554f24`에서 같은 방향 재연결을 수정했고, commit `5a237d93c`에서
  양방향 연결 수렴 회귀를 수정했다. commit `52fe28fa1`에서 비동기 handshake 중 연결 방향이
  사라지는 callback dispatch 경로를 수정했다.
- 배포: tag `core/v9.0.4`의 GitHub release 생성 진행 중
- 발견 경로: Java framework `AutomaticTurnDispatch` Config 8의 서버 종료·재기동 검증
- 영향 범위: socket message callback dispatch가 설정된 ROUTER가 같은 routing ID로 재연결하는 경로

이 문서는 확인된 core 버그의 원인, 기존 수정의 한계와 최종 수정 내용을 정리한다. Java/Kotlin framework는
재시도나 구동 순서 조정으로 문제를 우회하지 않고 framework가 소유한 ROUTER에 handover 정책을
내부적으로 적용한다.

## 현상

ROUTER peer가 종료된 뒤 같은 endpoint와 routing ID로 다시 시작하면 TCP 연결은 다시 설정되지만,
요청이 새 pipe로 전달되지 않는다. 기존 pipe가 제거될 때까지 약 30초가 지난 뒤에야 요청이 처리된다.

Java Config 8에서는 다음 순서로 재현됐다.

1. `Session` ROUTER와 `play-a` ROUTER 사이에서 요청과 응답을 여러 번 주고받는다.
2. `play-a`를 종료한다.
3. 같은 endpoint와 `play-a` routing ID로 서버를 다시 시작한다.
4. `Session`에서 `play-a`로 route request를 보낸다.
5. 새 TCP 연결이 설정되어 있어도 요청이 처리되지 않고 30초 timeout 경계까지 지연된다.

애플리케이션의 서버 구동 순서를 바꾸거나 재시도 횟수를 늘려 해결할 문제가 아니다. framework가
사용하는 callback dispatch 경로에서도 core가 새 pipe를 즉시 채택해야 한다.

## 원인

첫 번째 문제는 `core/src/runtime/sockets/router/router_admission.cpp`에 있던
`router_t::duplicate_pipe_should_replace(...)`에 있었다. commit `414554f24`는 같은 방향의 duplicate가
항상 최신 pipe를 채택하도록 고쳤다.

같은 방향으로 만들어진 기존 pipe와 새 pipe의 routing ID가 같을 때 현재 구현은 다음 조건을 적용한다.

```cpp
if (!socket_msg_dispatch_active ())
    return true;
return existing_outpipe_.pipe
       && existing_outpipe_.pipe->get_msgs_read () == 0
       && existing_outpipe_.pipe->get_msgs_written () == 0;
```

callback dispatch가 없으면 새 pipe를 채택한다. callback dispatch가 있으면 기존 pipe가 메시지를 한 번도
처리하지 않았을 때만 새 pipe를 채택한다. 정상 트래픽을 처리한 framework ROUTER의 기존 pipe는 항상
두 번째 조건에서 거부된다.

과거 메시지 처리 횟수는 기존 pipe가 현재 유효하다는 증거가 아니다. 서버 재시작으로 만들어진 새
pipe도 불필요한 중복 연결과 같은 방식으로 거부되기 때문에 handover 의미가 깨진다.

추가 검증 중 기존 pipe와 새 pipe의 생성 방향이 다른 로그도 확인됐다.

```text
router identify_peer: keep existing duplicate rid=play-a existing_local=0 new_local=1
router xsend_routed: no out pipe rid_size=6 rid=play-a
```

재기동 전 연결은 remote에서 시작한 pipe로 기록되어 있고, 재기동 뒤 새 연결은 local에서 시작한
pipe로 들어온다. 그러나 이 로그만 보고 교차 방향에서도 무조건 새 pipe가 승리하도록 바꾸면 안 된다.
양쪽 ROUTER가 서로 connect하는 정상 토폴로지에서는 두 connector가 상대 pipe를 계속 교체하게 되어
연결이 수렴하지 않는다. 실제로 9.0.2의 commit `8923225a0`이 이 결정을 제거했고, 장시간 `play-b`를
먼저 구동하지 않은 Kotlin 전체 runner의 `ATD-D2`에서 route request가 30초 뒤 timeout 되는 회귀가
발생했다.

9.0.3 검증에서 더 근본적인 두 번째 문제가 확인됐다. peer routing ID가 pipe attach 시점에 아직
도착하지 않으면 ROUTER는 pipe를 anonymous 상태로 보관한다. 이후 routing ID를 읽는 일반 수신 경로와
callback dispatch 경로는 원래 pipe가 local에서 시작됐는지에 관계없이
`locally_initiated=false`로 admission을 다시 실행했다. 또한 callback dispatch가 attach보다 먼저
routing ID frame을 처리할 수 있어 anonymous 등록과 route 채택이 서로 경쟁했다.

이 때문에 같은 방향 재연결도 반대 방향 연결로 기록될 수 있었고, routing ID 비교로 정한 안정된
방향과 실제 transport 방향이 달라졌다. 새 pipe가 잠시 등록된 뒤 상대 ROUTER가 연결을 종료하고,
요청을 보내는 ROUTER에는 해당 routing ID의 출력 pipe가 남지 않았다. 애플리케이션이 서버 시작
순서를 바꾸거나 30초를 기다려도 원인을 제거할 수 없는 core 상태 관리 결함이다.

Java 전체 runner에서도 같은 재기동 단계가 차단됐다. 2026-07-13 실행 로그
`framework/languages/java/e2e/AutomaticTurnDispatch/logs/20260713-151721-1434501/`에서는
ATD-A1~E2, E4, E5와 종료 대기가 통과한 뒤 `play-a`를 같은 routing ID로 재기동했다. 재기동
프로세스와 HTTP endpoint가 준비된 뒤에도 route/Spot readiness가 16회 연속 실패했다. runner의
재시도 상한이 약 20분이어서 원인 로그를 확보한 뒤 실행을 중단했다. 이 결과는 일반 시나리오나
애플리케이션 구동 순서 문제가 아니라 동일 routing ID 재기동 경로에 국한된다는 점을 다시 확인한다.

## 기존 테스트가 놓친 이유

수정 전에는 다음 기존 테스트만 일반 handover와 connect routing ID 중복 정책을 검증했다.

- `core/build/bin/test_connect_rid`: 7개 테스트 통과
- `core/build/bin/test_router_handover`: 일반 handover 2개 테스트 통과

하지만 이 테스트들은 framework와 같은 socket message callback dispatch를 활성화한 상태에서,
메시지를 처리한 기존 pipe를 둔 채 같은 방향·같은 routing ID로 재연결하는 조합을 검증하지 않는다.
따라서 `socket_msg_dispatch_active()` 이후의 문제 분기가 검증 범위에서 빠져 있다.

## 적용한 수정

handover 정책이 활성화된 상태에서 같은 방향·같은 routing ID의 재연결이 들어오면, 기존 pipe의 과거
메시지 처리 횟수만으로 새 pipe를 거부하면 안 된다. 서버 재시작 후 새 연결이 기존 연결을 즉시
대체할 수 있도록 pipe 생명주기와 중복 연결 판단을 분리해야 한다.

다음 두 대안을 비교했다.

1. handover가 활성화된 duplicate는 생성 방향과 관계없이 항상 최신 pipe로 교체한다.
2. 같은 방향의 재연결은 최신 pipe로 교체하고, 교차 방향 중복은 두 routing ID로 한 방향을 결정한다.

두 번째 안을 적용했다. 첫 번째 안은 9.0.2에서 실제로 적용했지만, 양쪽 peer가 서로 connect하면 종료된
connector가 재연결될 때마다 최신 pipe가 바뀌어 안정된 경로가 형성되지 않았다. 두 번째 안은 서버
재시작처럼 같은 방향에서 만들어진 새 pipe를 즉시 채택하면서도, 양방향 연결은 양쪽 peer가 동일하게
계산한 한 방향으로 수렴한다. 이 결정은 framework 내부에 있으므로 애플리케이션 개발자가 구동 순서를
선택할 필요가 없다.

따라서 `duplicate_pipe_should_replace(...)`는 같은 방향이면 과거 메시지 통계와 관계없이 새 pipe를
채택한다. 방향이 다르면 local routing ID와 peer routing ID를 바이트 순서로 비교해 한 방향만
채택한다. handover 비활성 상태에서는 기존처럼 새 pipe를 거부한다.

9.0.4에서는 아직 routing ID가 없는 pipe를 `pipe -> locally_initiated` 맵으로 보관한다. 일반 수신과
callback dispatch가 나중에 routing ID를 채택할 때 이 값을 그대로 사용한다. attach 시 peer 식별,
anonymous 등록과 receive queue 연결은 같은 dispatch lock 안에서 처리해 callback이 중간 상태를
관찰하지 못하게 했다. 공개 API나 애플리케이션 설정은 추가하지 않았다.

수정 뒤 같은 방향 재기동은 즉시 교체되고, `existing_local`과 `new_local`이 다른 중복 연결은 결정된
한 방향을 유지한다. framework에서 sleep, 재시도 횟수 증가, 서버 구동 순서 고정으로 우회하지 않는다.

Java framework는 ROUTER 생성 시 binding의 공개 옵션을 사용해 handover를 활성화한다. 이 설정은
framework 내부 연결 정책이므로 애플리케이션 개발자가 서버 구동 순서나 core 옵션을 알 필요가 없다.

## 회귀 테스트 요구 사항

core integration test에는 다음 조건을 포함하는 시나리오가 필요하다.

1. ROUTER에 socket message callback dispatch를 등록한다.
2. 같은 routing ID의 peer와 연결해 최소 한 번 요청과 응답을 처리한다.
3. 기존 pipe가 유지된 상태에서 같은 방향과 routing ID의 새 pipe를 만든다.
4. 새 pipe가 채택되어 새 peer가 메시지를 수신하는지 확인한다. 새 pipe의 attach는 비동기로
   일어나므로, 고정 대기 후 한 번만 보내는 대신 5초 상한 안에서 메시지를 반복 전송하며 새
   peer의 수신 여부를 확인한다. 이렇게 하면 attach 시점이 늦어지는 부하 환경에서도 테스트가
   admission 판정만 검증한다.
5. 수정 전 조건을 복원하면 새 pipe가 계속 거부되어 모든 반복 전송이 기존 peer로만 전달되고,
   이 테스트가 5초 상한을 소진한 뒤 실패하는지 확인한다.
6. 양쪽 peer가 동시에 연결을 시작한 정상 중복 연결은 기존 결정 규칙에 따라 한 pipe로 수렴하는지
   확인한다.
7. callback dispatch를 먼저 활성화하고 peer를 나중에 시작해 routing ID가 비동기로 도착하더라도,
   원래 연결 방향이 유지되고 반대 방향 duplicate가 기존 경로를 교체하지 않는지 확인한다.

같은 endpoint에서 실제 서버를 종료하고 같은 routing ID로 재기동하는 전체 생명주기는 Java Config 8의
`ATD-E3`에서 검증한다. core 테스트는 admission 조건을 직접 고정하고, E2E는 실제 배포 구성을 검증한다.

수정 후 다음 명령을 모두 실행해 검증한다.

```bash
cmake --build core/build
core/build/bin/test_connect_rid
core/build/bin/test_router_handover
cd framework/languages/java/e2e/AutomaticTurnDispatch
./run_e2e.sh all
```

완료 조건은 다음과 같다.

- `test_router_handover`의 기존 시나리오와 callback dispatch 재연결 회귀 시나리오가 모두 통과한다.
- `test_connect_rid`의 기존 7개 시나리오가 모두 통과한다.
- Java `AutomaticTurnDispatch ./run_e2e.sh all`에서 `ATD-A1`부터 `ATD-E5`까지 통과한다.
- `ATD-E3`에서 종료 대기와 동일 routing ID 재기동 복구가 30초 handshake timeout을 기다리지 않고
  완료된다.
- Kotlin `AutomaticTurnDispatch ./run_e2e.sh ATD-E3`와 Kotlin `ObservabilityOps ./run_e2e.sh OBS-C2`가
  서버 구동 순서를 조정하지 않고 통과한다.

## 검증 결과

2026-07-13에 다음 검증을 완료했다.

- core 9.0.4 Release 전체 CTest 114개가 통과했다. `test_router_handover`는 callback dispatch가
  활성화된 상태에서 peer가 나중에 시작하는 비동기 handshake와 반대 방향 duplicate를 포함한
  5개 시나리오가 통과했다. `test_connect_rid`의 7개 시나리오도 통과했다.
- Java `AutomaticTurnDispatch` 전체 selector가
  `logs/20260713-205516-3215299/`에서 통과했다. `ATD-E3`는 같은 routing ID로 서버를 재기동한 뒤
  handshake timeout을 기다리지 않고 요청을 처리했다.
- Kotlin `AutomaticTurnDispatch ATD-E3`는 `logs/20260713-205642-3225711/`에서 통과했다.
- Kotlin `ObservabilityOps OBS-C2`는 `logs/20260713-210155-3254245/`에서 통과했다.
- Kotlin `AutomaticTurnDispatch` 전체 selector는 core 9.0.4 local package를 사용한
  `logs/20260713-224238-3691348/`에서 통과했다. 늦게 시작한 `play-b`를 사용하는 `ATD-D2`와
  종료·재연결을 확인하는 `ATD-D3`도 같은 실행에서 통과했다.
- `core/v9.0.4` tag를 commit `52fe28fa1`에 생성했고 GitHub release build를 시작했다.

framework와 sample에는 재연결 대기나 서버 구동 순서를 지정하는 옵션을 추가하지 않았다.
