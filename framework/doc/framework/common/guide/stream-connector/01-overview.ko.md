# Stream Connector 개요

!!! info "이 장을 읽고 나면"

    mesh 밖의 client가 STREAM 서버에 접속할 때 어느 connector를 고를지 판단하고,
    connector가 어디까지 맡는지 안다.

STREAM 서버는 연결 하나를 session으로 다룬다. mesh 안의 node끼리 이름으로 호출하는 것과 달리
STREAM은 **연결 자체가 대상**이므로, 서버가 client에게 먼저 보낼 수도 있다. Stream Connector는
그 연결을 여는 client 쪽 라이브러리이며, 서버 session이 받는 것과 같은 packet을 client에서도 같은
모양으로 주고받게 한다.

이 장은 connector의 범위, 실행 환경별 선택, 배포 형태를 다룬다. 설치와 첫 연결은
[설치와 첫 연결](02-getting-started.ko.md)이 다룬다.

## 1. connector가 맡는 범위

packet은 **이름과 metadata를 담은 header에 payload를 붙여 보내는 전송 단위**다. connector는 이
packet을 만들고 해석하는 일, 연결을 유지하고 끊긴 연결을 되살리는 일을 맡는다. 채팅·전투·주문처럼
그 위에서 오가는 내용은 application이 정의한다. connector에는 도메인이 없다.

application code는 header bytes를 직접 만들거나 읽지 않는다. 공개 표면에는 임의 header를 다루는
자리가 없고, 이름·metadata·payload만 다룬다.

## 2. 서버 framework와의 경계

connector package는 서버 framework package에 의존하지 않는다. 반대 방향도 같아서 서버 framework
package는 connector를 참조하지 않는다. 양쪽이 공유하는 것은 wire 계약 하나이므로, client를
빌드하는 쪽은 서버 runtime을 내려받지 않는다.

connector의 의존성은 transport·codec·압축처럼 client 실행에 필요한 것으로 한정한다. 그래서 웹
브라우저나 게임 엔진처럼 서버 런타임을 올릴 수 없는 환경에서도 같은 protocol을 사용한다.

## 3. 실행 환경별 connector 선택

어느 connector를 사용하는지는 **언어가 아니라 엔진과 빌드 타깃**이 정한다. 같은 Unity project도
네이티브 빌드와 웹 빌드가 서로 다른 connector를 사용한다.

| 대상 | 네이티브 빌드 | 웹 빌드(브라우저·WASM) |
|---|---|---|
| Unity | `.NET` connector | TypeScript connector — C#이 jslib interop으로 JS 계층을 호출한다 |
| Godot | C++ connector(GDExtension) 또는 `.NET` connector(Godot C#) | TypeScript connector |
| Cocos | C++ connector(Axmol 어댑터) | TypeScript connector(Cocos Creator web) |
| Unreal | C++ connector(plugin) | 해당 없음 |
| 브라우저 웹 client | — | TypeScript connector |
| 데스크톱·서버 애플리케이션 | `.NET` · Java · C++ connector | — |

**웹으로 빌드하는 순간 언어와 무관하게 TypeScript connector를 사용한다.** 브라우저 샌드박스에서
OS 소켓을 열 수 있는 언어가 없기 때문이다.

게임 엔진은 엔진 객체를 main thread 밖에서 다룰 수 없다. 그래서 수신 callback을 언제 실행할지
정하는 설정의 기본값이 **직접 pump하는 쪽**이고, C++ connector core는 예외와 coroutine이 꺼진
빌드에서도 그대로 빌드된다.

## 4. endpoint와 transport

endpoint scheme이 transport를 정한다. 따로 지정하지 않으면 아래 대응을 사용한다.

| scheme | transport |
|---|---|
| `tcp://` | TCP |
| `tls://` | TLS over TCP |
| `ws://` | WebSocket |
| `wss://` | WebSocket over TLS |

브라우저 계열(웹, Cocos web, Unity WebGL, Godot Web)이 사용할 수 있는 transport는 `ws`와 `wss`뿐이다.
이 환경에 `tcp://`·`tls://` endpoint를 주면 연결을 시도하지 않고 구성 오류로 즉시 실패한다. 이는
구현의 제약이 아니라 플랫폼의 제약이다. 네이티브 빌드는 위 표의 transport를 모두 사용한다.

## 5. 주고받는 단위

connector가 다루는 packet에는 **종류**가 있다. 종류는 그 packet이 응답을 기다리는지, 응답 자체인지를
나타낸다.

| 종류 | 의미 |
|---|---|
| Send | 응답을 기다리지 않는 단방향 packet |
| Request | 응답을 기다리는 packet |
| Response | request의 성공 응답 |
| Error | request의 실패 응답, 또는 request와 무관한 stream 오류 |

응답은 request와 같은 sequence로 맞춰진다. 그래서 여러 request를 동시에 보내도 응답이 도착하는
순서와 무관하게 각각 완료된다. 응답에는 packet 이름이 실리지 않는다.

payload를 bytes로 바꾸는 codec은 connector를 만들 때 하나를 정하고, 기본값은 JSON이다. 송신·수신
payload 한도는 각각 64KB이며 option으로 조절한다. metadata는 trace id나 locale처럼 작은 값을 위한
자리이고 전체 1024 bytes를 넘을 수 없다.

## 6. 배포 산출물

| 대상 | 산출물 | 배포 채널 |
|---|---|---|
| 일반 C++ client | `zlink-stream-connector` | CMake · vcpkg · Conan |
| Unreal | `zlink-unreal-stream-connector` | source plugin |
| Godot(C++) | `zlink-godot-stream-connector` | source GDExtension |
| Cocos/Axmol | `zlink-axmol-connector` | source package |
| `.NET` 데스크톱·서버, Unity 네이티브, Godot C# | `Zlink.Stream.Connector` | NuGet |
| Java · Kotlin | `systems.zlink:zlink-stream-connector` | Maven |
| 브라우저 계열 | `@zlink-systems/stream-connector` | npm |
| Unity WebGL 어댑터 | `com.zlink.stream-connector.webgl` | UPM source package |

웹 계열은 브라우저·Cocos web·Unity WebGL·Godot Web이 모두 브라우저 런타임이므로 npm package 하나를
공유한다. 네이티브 엔진 어댑터는 엔진 빌드 시스템에 소스로 편입되는 관례를 따라 source로 배포한다.
Unity 네이티브와 Godot C#은 별도 package 없이 `.NET` connector를 그대로 사용한다.

## 7. 다음 장

- 설치하고 첫 packet을 주고받기 — [설치와 첫 연결](02-getting-started.ko.md)
- 기본값과 검증 시점 — [Connector 옵션](03-connector-options.ko.md)
- 연결 상태와 재연결 — [연결 생명주기](06-lifecycle.ko.md)
- 엔진과 빌드 대상별 통합 경로 — [게임 엔진 통합](12-engine-integration.ko.md)
