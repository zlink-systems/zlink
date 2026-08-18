# Endpoint 표기 정책

결정일 2026-08-18. 4개 언어(C++·Java·Node·.NET) 공통.

frozen 스펙 `framework/doc/framework/common/spec/server/10-network-listener-identity.md`는 endpoint의
**구성**(`BindHost + port`, `AdvertiseHost + 실제 bound port`)과 wildcard·port 0 금지를 규정하지만
**문자열 표기와 동등성 판정은 규정하지 않는다**. 같은 문서가 RID와 UUID에 대해서는 "36자 lowercase
canonical"을 못박은 것과 대비된다. 이 문서가 그 공백을 채운다.

## 1. 배경

endpoint 문자열은 리포 전역에서 **동등 비교·map 키·set 멤버십**으로 쓰인다(조사 시점 기준 4개 언어
합계 약 50개 지점). 표기가 어긋나면 다음이 발생한다.

- .NET `ZLinkClientServerClientRuntime`는 서버가 광고한 문자열이 클라이언트 기대와 구문상 다르면
  **admission을 거부**한다. 조용한 no-op이 아니라 연결 실패다.
- auto-connect의 self 감지(`isSelf`)와 **누가 먼저 거는지를 정하는 사전순 tie-break**가 표기에
  민감하다. 표기가 갈리면 양쪽이 동시에 걸거나 아무도 걸지 않는다.
- peer intent·location store descriptor·topology의 dedup이 실패해 중복 연결, 미정리 항목,
  불필요한 재연결이 생긴다.

## 2. 결정

### 2.1 DNS 이름 해석을 하지 않는다

`localhost`와 `127.0.0.1`을 같은 endpoint로 보지 않는다. 이름 해석은 다중 레코드·캐시·시점에 따라
달라져 동등성 판정의 근거가 될 수 없고, "같다"의 의미가 실행마다 달라지는 것이 dedup 실패보다 나쁘다.

대신 **설정·광고·연결에 같은 표기를 쓸 것을 요구한다.** 한 배포 안에서 같은 대상을 두 표기로 쓰면
서로 다른 endpoint로 취급된다.

### 2.2 결정적 문자열 정규화를 적용한다

| 항목 | 규칙 |
|---|---|
| scheme | 소문자 (`TCP://` → `tcp://`) |
| host | 소문자 (DNS는 대소문자를 구분하지 않는다) |
| IPv6 host | 대괄호 표기로 통일하고 zone id는 보존한다 |
| port | 10진수, 선행 0 제거 (`:0080` → `:80`) |
| 경로 | 후행 슬래시 제거 |
| 공백 | 앞뒤 공백 제거 |

정규화는 **손실이 없어야 한다.** 위 목록에 없는 요소(userInfo, query, fragment, IPv6 zone id)는
그대로 보존한다.

**authority를 갖는 scheme에만 적용한다.** `tcp`·`tls`·`ws`·`wss`처럼 `host:port`를 갖는 scheme이
대상이다. `ipc://`처럼 뒤가 **파일 경로**인 scheme은 scheme 소문자화만 하고 나머지 바이트는 그대로
둔다. 경로는 대소문자를 구분할 수 있고 후행 슬래시가 의미를 가질 수 있어, host 규칙을 적용하면
가리키는 대상이 바뀐다.

같은 이유로 **advertised endpoint를 만들 때의 host 치환도 authority scheme에서만 한다.**
`inproc://`처럼 host 자리가 사실은 불투명 식별자인 scheme에 `AdvertiseHost`를 대입하면 identity가
훼손된다.

### 2.3 쓰기 시점에 정규화한다

비교 시점마다 정규화하지 않고, **endpoint를 만들거나 외부에서 받아들이는 지점에서 한 번 정규화해
저장한다.** 이후 비교는 단순 문자열 동등으로 유지한다. 이유: 비교 지점이 약 50개인데 반해 구성·수용
지점은 훨씬 적고, 비교 지점을 하나라도 빠뜨리면 증상이 조용하다.

정규화 대상 지점:
- advertised endpoint 구성(스펙 10 §4)
- 애플리케이션이 설정으로 넘긴 bind·remote endpoint
- peer가 보내온 descriptor의 endpoint
- Location Store에서 읽은 row의 endpoint

### 2.4 IPv6 안전 파싱을 요구한다

`lastIndexOf(':')`로 포트를 분리하는 구현을 금지한다. IPv6 리터럴은 콜론을 여러 개 포함하므로
대괄호를 인식하는 파싱만 허용한다. (Java `ZLinkJavaRawMeshNode.advertisedEndpoint`가 이 방식이라
IPv6에서 깨진다.)

### 2.5 구성 로직 중복을 제거한다

한 언어 안에 advertised endpoint 구성이 여러 벌 있으면 하나로 합친다. 중복된 구현은 서로 다르게
동작해 왔다 — Node는 같은 정규식이 3벌, C++는 IPv6 처리가 있는 구현과 없는 원시 문자열 연결이
공존한다.

### 2.6 커넥터 scheme 대소문자

stream connector의 scheme→transport 매핑(커넥터 공통 스펙 §3.1)도 scheme을 소문자화한 뒤 판정한다.
현재 .NET만 그렇게 하고 Java·C++는 대소문자를 구분해 `TCP://`를 거부한다.

## 3. 적용 범위

정규화 유틸을 4개 언어에 각각 신설한다(공통 wire가 아니라 각 언어의 로컬 계약이므로 언어별 구현).
호스트 언어에 URI 파서가 있으면 그것을 쓰고(.NET `System.Uri`, Java `java.net.URI`,
Node `URL`), C++는 소형 파서를 직접 만든다.

구현 계약은 언어별 `framework/doc/framework/{cpp,java,node,dotnet}/internals/`에 기록하고 이 문서에서
링크한다.

## 4. 검증

- 정규화 함수의 라운드트립 테스트: 대소문자·IPv6·선행 0·후행 슬래시·공백 각 케이스.
- **정규화가 손실이 없음**을 pin하는 테스트(userInfo·query·zone id 보존).
- auto-connect tie-break가 표기 차이에 영향받지 않음을 pin.
- 교차 언어 하네스는 이미 같은 표기를 쓰므로 이 정책의 회귀를 잡지 못한다. 언어별 단위 테스트로 고정한다.
