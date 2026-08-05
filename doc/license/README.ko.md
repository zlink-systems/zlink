[English](README.md) | 한국어

# 라이선스 정책

> 이 저장소가 왜 세 가지 라이선스를 쓰는지, 각각 무엇을 허용하는지, 정본 텍스트는
> 어디에 있는지 정리한 문서다.

## 1. 세 계층, 한 저장소

| 계층 | 디렉토리 | 라이선스 | 정본 텍스트 |
|------|----------|----------|--------------|
| 엔진 | `core/`, `bindings/`(언어별 네이티브 바인딩) | MPL-2.0 | [`/LICENSE`](../../LICENSE) |
| 프레임워크 | `framework/**`(SPOT/actor, channel messaging, STREAM, drain, location store 통합) | FSL-1.1-ALv2(Functional Source License) | [`/framework/LICENSE`](../../framework/LICENSE) |
| http-client | 전 언어(dotnet/java/kotlin/node/cpp)의 `framework/` 아래 언어별 `http-client` 패키지 | Apache-2.0 | 생태계별 표기 위치는 §5 참고, 전문은 [`framework/languages/cpp/http-client/LICENSE`](../../framework/languages/cpp/http-client/LICENSE)에도 |

이 분리는 우연이 아니라 의도적이다. 각 계층은 "무엇을 지켜야 하고 무엇은 지킬
필요가 없는가"라는 질문에 서로 다르게 답한다.

## 2. `core`/`bindings`가 MPL-2.0인 이유

`core`는 [libzmq](https://github.com/zeromq/libzmq) v4.3.5에서 fork로
출발했다(무엇을 좁히고 왜 그랬는지는 `core/doc/guide/design-rationale.ko.md`
참고). 별개로, libzmq 프로젝트 자체의 공개된 이력으로는: v4.3.5는 libzmq가
LGPLv3+static-linking exception에서 MPL-2.0으로 재라이선싱을 **완료한 바로 그
버전**이다. `core`의 라이선스는 이 계보를 그대로 이어받은 것이다: MPL-2.0
파일은 MPL-2.0으로 남아야 하고, 이 프로젝트에는 남의 저작물을 임의로 더
제한적인 라이선스로 바꿀 권리가 없다.

MPL-2.0은 **파일 단위의 약한 카피레프트**다. MPL이 적용된 파일은 MPL-2.0으로
유지돼야 하지만, 다른 라이선스의 코드와 묶어서 "더 큰 작업물(Larger Work)"로
배포하는 건 명시적으로 허용한다 — 이 조항 덕분에 `framework`(FSL)가
`core`/`bindings`(MPL-2.0)를 링크해도 전체 결합물이 MPL-2.0이 될 필요가
없다.

`bindings/`(core를 감싸는 언어별 네이티브 바인딩 계층)도 같은 이유로
MPL-2.0에 남는다: 독자적인 제품·SaaS 표면이 없는 인프라라서, 최대한
퍼미시브하게 두는 게 전략적 비용 없이 채택만 극대화한다.

## 3. `framework`가 FSL-1.1-ALv2인 이유

`framework`는 진짜 차별화된 독자 가치가 있는 곳이다 — SPOT/actor, channel
messaging, STREAM, graceful drain, location store 기반 토폴로지. 누군가
경쟁 관리형 서비스("ZLink Framework Cloud")로 세울 만한 대상이 바로 이
계층이다. 목표는: 널리 채택될 만큼은 열어 두되, 그 특정 시나리오가 실제로
벌어지면 이익을 확보하는 것이었다.

**FSL이 실제로 막는 것.** 일반적인 사용에는 아무 제약이 없다. **Permitted
Purpose**(Competing Use가 아닌 모든 목적)에 한해 사용·복사·수정·2차 저작물
작성·재배포가 전부 자유다. Competing Use는 소프트웨어를 대체하거나, 이미
이 소프트웨어로 제공 중인 다른 제품/서비스를 대체하거나, 실질적으로 동일한
기능을 제공하는 상업적 제품/서비스로 **남에게 제공하는 것**을 뜻한다 —
단순히 똑같이 호스팅한 서비스를 세우는 경우만이 아니다. `framework/LICENSE`가
명시적으로 나열하는 Permitted Purpose는 네 가지뿐이다: 내부 사용, 비상업
교육, 비상업 연구, 라이선스 사용자에게 제공하는 전문 서비스. 자기 제품에
임베드하는 건 이 명시적 목록에는 없다 — 대신 부정형 정의로 허용된다. 자기
게임 서버나 백엔드를 만들어 배포하는 것 자체가 Competing Use가 아니기
때문이다(그 제품의 기능이 프레임워크 자체를 대체하는 게 아니므로). 실제로
작동하는 건 이 부정형 판단 기준이다: 금지되는 건 **프레임워크 자체(또는
기능적으로 동등한 것)를 경쟁 제품·서비스로 만드는 것**이다.

**Change Date(전환 시점).** `framework`의 각 릴리스는 발행일부터 시작하는
**독립된 2년 카운트다운**을 갖는다. 2년이 지나면 그 특정 버전은 자동으로
Apache License 2.0으로 전환되고, 그 시점부터는 경쟁 관리형 서비스를 포함해
누구나 그 버전을 뭐든지 할 수 있다. 카운트다운이 릴리스마다 따로 도니까
**최신 코드는 항상 보호 구간 안**에 있고, 오래된 릴리스는 순차적으로 완전히
풀린다. 이 조항은 선택이나 철회가 불가능하다 — 부여는 취소 불가능하고
자동으로 실행된다.

**BSL 대신 FSL을 고른 이유.** 둘 다 "결국 완전히 오픈되는" source-available
라이선스로 목표는 같다. BSL 1.1은 빈칸("Additional Use Grant")이 있는
템플릿이라 채택하는 회사마다 직접 문구를 채운다 — 실제로 MariaDB의 grant,
Akka의 매출 기준 grant, HashiCorp의 grant가 전부 조금씩 다르다. 그래서
독자는 프로젝트마다 정확한 문구를 매번 확인해야 한다. FSL은 제약
문구("Competing Use" 정의)를 **모든 채택자에게 고정**한다 — 채택자가
정할 수 있는 건 전환 후 어느 퍼미시브 라이선스로 바꿀지뿐이다. 혼자
유지보수하는 프로젝트 입장에서는, 직접 작성하거나 관리할 커스텀 조항이
없고 어디서 쓰이든 법적 해석이 동일하다는 점이 결정적이었다.

**Licensor.** `ZLink Systems`(`framework/LICENSE`의 Notice 절 참고).

## 4. `http-client`가 FSL이 아니라 Apache-2.0인 이유

전 언어의 `framework/` 아래 있는 `http-client` 패키지는 각 플랫폼에서 흔히
쓰는 HTTP 클라이언트 라이브러리를 감싼 얇은 래퍼다 — `System.Net.Http`(.NET BCL),
`undici`(Node.js 공식 HTTP client), `java.net.http`(JDK 표준
라이브러리), Boost.Beast(C++, 이미 `core/external/boost/`에 벤더링돼
있음, Boost Software License 1.0). 넷 다 `PackageReference`/`dependencies`/
`api(...)`로 **의존성으로 호출**할 뿐 소스를 복사한 게 아니다 — 그래서
libzmq를 fork한 `core`와 달리, `http-client` 자체의 래퍼 코드에는 라이선스
상속 제약이 전혀 없다.

더 중요한 건: FSL이 방어하는 위협 자체가 여기엔 해당하지 않는다는 점이다.
"HttpClient-as-a-Service"로 경쟁하는 사람은 없다 — client 라이브러리는
남의 제품 안에 끼워 쓰는 것이지, 그 자체로 호스팅해서 파는 제품이 아니다.
여기에 FSL을 걸면 채택 장벽(법무팀이 검토해야 하는 비표준 라이선스)만
늘고 대응하는 이익은 없다. 그래서 `http-client`는 표준 SPDX `Apache-2.0`
표기를 쓰며, 표기 위치는 생태계마다 §5와 같이 다르다. 그리고 어차피
`framework`의 FSL도
2년 뒤 Apache-2.0으로 전환되므로, `http-client`가 처음부터 Apache-2.0인
건 시간이 지나면 `core`/`bindings`를 제외한 나머지 전체가 **같은 최종
라이선스로 수렴**한다는 뜻이기도 하다.

## 5. 생태계별 실제 표기 위치

| 생태계 | `framework`(FSL) | `http-client`(Apache-2.0) |
|--------|---------------------|------------------------------|
| dotnet | `Directory.Build.props`/`.targets`가 `PackageLicenseFile=LICENSE`를 설정하고 packable한 각 `.nupkg`에 `framework/LICENSE`를 번들링하도록 구성돼 있다. **아직 미검증**: 이 저장소의 로컬 SDK(8.0.128)에서는 `dotnet pack`이 NU5030으로 실패하며, 격리된 최소 재현 프로젝트에서도 동일하게 재현됨 — 설정 오류가 아니라 SDK/환경 버그로 보이나(`Directory.Build.targets`의 주석 참고), FSL `.nupkg`를 실제로 배포하기 전 반드시 릴리스 툴체인/CI에서 정상 작동을 확인해야 한다 | `Zlink.HttpClient.csproj`가 `PackageLicenseExpression=Apache-2.0`으로 오버라이드하며, 이 경로는 NU5030에 걸리지 않고 정상 pack된다 |
| java/kotlin | 루트 `build.gradle.kts`의 `subprojects{}` Maven `pom.licenses` 블록 | 같은 블록이 `zlink-http-client`/`zlink-http-client-kotlin`만 Apache-2.0으로 예외 처리 |
| node | 각 패키지 `package.json`이 `"license": "SEE LICENSE IN LICENSE"`, 이제 각 패키지 디렉터리에 실제 `LICENSE` 파일도 있어 이 표기가 실제로 가리키는 대상이 존재한다 | `http-client/package.json`이 `"license": "Apache-2.0"` 직접 표기, `LICENSE` 파일도 Apache-2.0 전문으로 채워져 있다 |
| cpp | 트리 루트의 `framework/LICENSE`가 지배(패키지별 매니페스트 없음), 추적되는 FSL 소스 파일마다 `SPDX-License-Identifier: FSL-1.1-ALv2` 헤더도 붙어 있다 | `framework/languages/cpp/http-client/LICENSE`에 Apache-2.0 전문을 직접 배치, 소스 파일에는 `SPDX-License-Identifier: Apache-2.0` 헤더 |
