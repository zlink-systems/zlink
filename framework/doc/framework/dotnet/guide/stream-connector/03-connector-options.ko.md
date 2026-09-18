---
title: "Connector 옵션 · C#/.NET"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/stream-connector/03-connector-options.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# Connector 옵션

<!-- framework-adapter-nav:start -->
[목차](README.ko.md) | [이전: 설치와 첫 연결](02-getting-started.ko.md) | [다음: packet 송신](04-sending.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/stream-connector/03-connector-options.ko.md) · **C#/.NET** · [Java](../../../java/guide/stream-connector/03-connector-options.ko.md) · [Kotlin](../../../kotlin/guide/stream-connector/03-connector-options.ko.md) · [Node/TypeScript](../../../node/guide/stream-connector/03-connector-options.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    connector를 만들 때 정하는 값과 그 기본값을 알고, 잘못된 구성이 언제 거부되는지 안다.

option은 connector를 만들 때 한 번 전달한다. 만들어진 connector는 그 값을 복사해 두고, 읽기
표면으로 지금 사용 중인 값을 그대로 돌려준다. 실행 중에 바꿀 수 있는 값은 진단 수준이다.

## 1. option을 정하는 자리

지정하지 않은 항목은 기본값을 사용한다. 최소 구성은 endpoint 하나다.

```csharp
var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
{
    Endpoint = new Uri("wss://game.example.com:443/stream"),
    ConnectTimeout = TimeSpan.FromSeconds(5),
    RequestTimeout = TimeSpan.FromSeconds(10),
    Reconnect = new ZlinkStreamReconnectOptions { MaxAttempts = 5 },
    DispatchMode = ZlinkStreamDispatchMode.Manual
});
```

## 2. 기본값

| 항목 | 기본값 |
|---|---|
| connect timeout | 5초 |
| request timeout | 30초 |
| 대기 표면 timeout | 5초 |
| heartbeat | 켜짐 — 주기 1초, timeout 5초 |
| 자동 재연결 | 켜짐 — 초기 지연 250ms, 최대 지연 5초, backoff 계수 2.0, 최대 시도 3회 |
| 수신 callback 실행 시점 | 직접 pump하는 쪽 |
| codec | JSON |
| 압축 | Lz4 |
| 송신·수신 payload 한도 | 각 64KB |
| TLS 인증서 검증 | 켜짐 |
| 진단 수준 | 오류만 기록 |

언어마다 이름은 달라도 기본값은 같다. 게임 엔진에서도 데스크톱 도구에서도 같은 구성이 같게
동작해야 하기 때문이다.

## 3. endpoint와 transport

endpoint scheme이 transport를 정한다. `ws://` endpoint 하나만 지정한 구성은 추가 설정 없이
WebSocket으로 연결한다.

transport를 따로 지정하는 option은 scheme을 덮어쓰는 자리가 아니라 **두 값이 맞는지 확인하는
자리**다. 지정한 transport가 endpoint scheme과 어긋나면 구성 오류로 실패한다. 브라우저 계열에서
`tcp://`나 `tls://`를 지정한 경우도 같다.

## 4. timeout

timeout은 각각 다른 작업을 제한한다.

- connect timeout — 한 번의 연결 시도가 끝나야 하는 시간
- request timeout — 응답이 도착해야 하는 시간. 호출마다 덮어쓸 수 있다
- 대기 표면 timeout — 특정 packet이 도착하기를 기다리는 시간. 호출마다 덮어쓸 수 있다

request timeout이 지나면 그 request만 실패하고 연결은 그대로 유지된다. 늦게 도착한 응답은
이미 제거된 request를 다시 완료시키지 않는다.

## 5. heartbeat

heartbeat는 연결이 살아 있는지 주기적으로 확인한다. 켜져 있으면 지정 주기마다 control packet을
보내고, 지정 timeout 동안 들어오는 frame이 하나도 없으면 연결이 끊긴 것으로 처리한 뒤 재연결
정책을 적용한다.

heartbeat를 꺼도 서버가 보낸 ping에는 응답한다. 끈 것은 이쪽에서 보내는 주기이지 응답 의무가
아니다.

## 6. 자동 재연결

자동 재연결은 기본으로 켜져 있다. 시도 사이의 지연은 초기 지연에서 시작해 시도마다 backoff 계수를
곱하고 최대 지연에서 멈춘다. 실제로 기다리는 시간은 그 값의 50%에서 100% 사이에서 고른다 — 서버가
한 번 끊겼을 때 연결되어 있던 client가 모두 같은 시점에 다시 연결하면 그 순간 서버의 부하가 가장
크기 때문이다.

연결이 복구될 때까지 계속 시도하는 client는 큰 수를 적는 대신 **무제한**을 지정한다. 그래야 시도
횟수가 유한한 구성과 구분된다.

```csharp
Reconnect = new ZlinkStreamReconnectOptions { MaxAttempts = null }   // null이 무제한이다
```

재연결 동작과 끊김 handler는 [연결 생명주기](06-lifecycle.ko.md)가 다룬다.

## 7. 수신 callback 실행 시점

기본 설정에서 수신 callback은 receive 경로에서 바로 실행되지 않고 내부 큐에 들어가며,
application이 pump를 호출한 실행 문맥에서 실행된다. 게임 엔진 객체를 main thread 밖에서 다룰 수
없기 때문에 이 값이 기본이다.

즉시 실행으로 바꾸면 pump 없이 receive 경로에서 실행된다. 이때 느린 handler는 receive 경로를
막으므로 뒤따르는 수신 처리가 그만큼 늦어진다. CLI나 도구처럼 main thread 제약이 없는 곳에서
사용한다.

두 설정의 차이와 등록·해제는 [packet 수신](05-receiving.ko.md)이 다룬다.

## 8. codec과 압축

payload를 bytes로 바꾸는 codec은 connector를 만들 때 하나를 주입하고, 지정하지 않으면 JSON을
사용한다. 메시지 타입마다 codec을 등록하거나 호출마다 codec을 고르는 표면은 없다. payload
타입에서 packet 이름을 정하는 규칙도 같은 자리에서 주입한다.

압축 알고리즘도 connector를 만들 때 한 번 정한다. 기본값을 Lz4로 두었다고 해서 모든 packet이
압축되지는 않는다. **압축을 명시한 송신만 압축한다.** 압축을 끈 구성에서 압축을 명시하면 그
호출이 실패하고, 압축된 frame을 받으면 압축 해제 오류로 거부한다.

서버가 압축해 보낸 packet은 handler를 호출하기 전에 connector가 압축을 해제한다.

## 9. payload와 metadata 한도

송신과 수신 payload 한도는 각각 64KB이며 option으로 조절한다. 압축한 frame을 받으면 wire의 압축된
payload와 압축을 해제한 결과를 각각 수신 한도와 비교한다. 64KB보다 큰 payload를 주고받는
application은 이 값을 명시적으로 키운다.

metadata의 한도는 전체 1024 bytes이고 **option으로 조절하지 않는다.** metadata는 trace id·locale·
tenant id처럼 작은 값을 위한 자리이며, 큰 데이터는 payload로 보낸다.

## 10. TLS 인증서 검증

TLS와 WSS는 인증서 chain과 host 이름을 검증한다. 검증을 생략하는 option이 있지만 기본값은 꺼짐이며,
테스트의 자체 서명 인증서에만 사용한다. 운영 구성에서 이 값을 켜면 서버 인증서를 신뢰하지 않고
통과시킨다.

## 11. 진단 수준

진단 수준은 connector가 흐름 추적 정보를 얼마나 만들고 검증할지 정한다. 기본값은 오류만 기록하는
수준이다. 가장 낮은 수준으로 두면 보내는 frame에 흐름 식별자를 만들지 않고, 받은 frame의 흐름
필드도 길이만 확인한 뒤 값 검증과 전달을 생략한다. request의 상관관계 식별자는 수준과 무관하게
유지되므로, 낮춰도 응답이 어긋나지 않는다.

이 값은 connector를 다시 만들지 않고 실행 중에 읽고 바꾼다. 바꾼 값은 그 뒤의 처리 지점부터
적용되며 이미 만들어진 frame에는 소급 적용하지 않는다.

```csharp
connector.SetDiagnosticsLevel(ZlinkStreamDiagnosticsLevel.Off);
var level = connector.DiagnosticsLevel;
```

바꾸는 표면은 완료를 기다리지 않는다. 값 하나를 바꾸는 작업이라 기다릴 완료가 없고, 수신 callback
안에서 호출해도 자기 완료를 기다리는 순환이 생기지 않는다.

## 12. 검증 시점

**option은 전 항목을 검증한다.** 일부만 확인하면 나머지 항목의 잘못된 값이 조용히 무시되고,
호출자는 구성 실수와 연결 실패를 구분하지 못한다.

검증은 그 언어가 실패를 알릴 수 있는 가장 이른 곳에서 이뤄진다. 실패를 돌려줄 통로가 있는 언어는
connector를 만들 때 검증하고, 생성 표면에 그 통로가 없는 언어는 연결을 시도할 때 검증한다.
어느 쪽이든 **연결이 이뤄지기 전에** 거부하며, 검증을 통과하지 못한 구성으로는 연결되지 않는다.

| 위반 | 실패 |
|---|---|
| 값 하나가 허용 범위를 벗어남(음수 timeout, 0인 payload 한도 등) | 검증 실패 |
| 항목 사이가 맞지 않음(endpoint scheme과 transport 충돌, 환경이 지원하지 않는 transport, 압축을 끈 구성에 압축 codec 지정) | 구성 오류 |

두 오류를 받는 방법과 코드를 읽는 방법은 [오류 처리](07-error-handling.ko.md)가 다룬다.
