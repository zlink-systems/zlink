# C++ cross-language smoke

C++ framework/connector 패키지를 이미 G7을 통과한 다른 언어(.NET, Node.js)의 실제
패키지와 맞대어 실행한다. 한 언어의 자체 E2E 성공은 cross-language 성공이 아니므로,
producer/consumer 방향을 각각 한 스테이지로 나눠 실제 wire로 검증한다.

## 실행

```bash
cd framework/languages/cpp/cross-language
ZLINK_CPP_BUILD_DIR=../build-redis-vcpkg ./run_cross_language_smoke.sh
```

- C++ 호스트 타깃: `cmake --build <build-dir> --target zlink_cpp_cross_language_host`
- .NET 피어: `framework/languages/dotnet/cross-language/Zlink.Framework.TestHost` (`dotnet run`)
- Node 피어: `node_peer_host.js` (Node workspace의 배포된 `packages/*/dist` 사용)
- 실행 로그를 남기려면 `ZLINK_CPP_CROSS_KEEP_RUN_DIR=1`

## 원격 Actor 생성 셀 (`remote-actor-create`, 기본 실행에서 제외)

```bash
ZLINK_CPP_CROSS_LANGUAGE_STAGE=remote-actor-create ./run_cross_language_smoke.sh
# 개별 셀: remote-actor-create-cpp-dotnet / -cpp-node / -cpp-java
# 대조군:  remote-actor-create-dotnet-java
```

user-spot-join 12칸 행렬은 원격 Actor 생성 경로를 지나가지 않는다(#550). 대상 host는
자기 User Spot을 만든 뒤 placement weight를 `0`으로 낮추므로, 요청하는 쪽에는 후보가
자기 자신만 남는다. 위 셀은 그 한 값만 뒤집는다 — 대상은 `--placement-weight 100`으로
남고 요청하는 쪽은 `--placement-weight 0`이 되므로, 생성 요청은 반드시 다른 언어 노드가
받는다. 두 host 모두 자기 framework Entry Spot을 Location Store로 배치하므로, weight는
구성 시점이 아니라 그 Spot이 만들어진 뒤 runtime에 적용한다.

이 셀들은 아직 통과하지 않으며, 그래서 기본 실행에 넣지 않았다. `#549` 수정으로 .NET
대상은 authority payload 거절 지점을 지나 Actor를 실제로 만들고, 그 뒤 예약 commit이
fence된다. 남은 원인은 C++이 아니라 상대 runtime이 소유한다(자세한 내용은
`run_cross_language_smoke.sh`의 `stage_cpp_source_dotnet_target_remote_actor_create`
위 주석과 `remote-actor-create-dotnet-java` 대조군을 참고한다). 그 원인이 정리되면 이
셀들을 기본 실행으로 옮긴다.

## 검증하는 행

| feature | producer | consumer | 확인 marker |
|---------|----------|----------|-------------|
| messaging | C++ | `.NET` / Node.js | request/reply + one-way send |
| messaging | `.NET` / Node.js | C++ | request/reply (+ Node는 one-way send) |
| flow-wire | C++ | `.NET` / Node.js | fanout `<topic>:<value>` |
| flow-wire | `.NET` / Node.js | C++ | fanout `<topic>:<value>` |
| codec | C++ | `.NET` | STREAM 프레임 + JSON payload |
| codec | `.NET` / Node.js | C++ | STREAM 프레임 + LZ4 압축 payload |

## 계약 메모

- packet identity: DTO의 `static constexpr packet_name`을 쓴다. 다른 언어는 타입 이름을
  기본 packet 이름으로 쓰므로 이름을 그 타입명에 맞춘다.
- payload: JSON 본문. 피어 언어의 필드 표기(`value`/`Value`)를 모두 수용한다.
- STREAM 압축: LZ4 **pickle** 프레이밍(`[헤더][size-diff LE][LZ4 block]`)이 공통 wire다.
  C++이 쓰던 raw `[u32][block]` 프레이밍은 상호운용되지 않아 pickle로 교체했다.
