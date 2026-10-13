[English](README.md) | 한국어

# 라이선스 정책

> 저장소 계층마다 적용되는 라이선스와 허용 범위, 정본 텍스트 위치를
> 정리한 문서다.

## 1. 세 계층, 한 저장소

| 계층 | 디렉토리 | 라이선스 | 정본 텍스트 |
|------|----------|----------|--------------|
| 엔진 | `core/`, `bindings/`(언어별 네이티브 바인딩) | MPL-2.0 | [`/LICENSE`](../../LICENSE) |
| 프레임워크 | `framework/**`(SPOT/actor, channel messaging, STREAM, drain, location store 통합) | zlink License 1.0 | [영문 정본](../../framework/LICENSE) · [한국어 번역](../../framework/LICENSE.ko.md) |
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
배포하는 건 명시적으로 허용한다 — 이 조항 덕분에 `framework`(zlink License 1.0)가
`core`/`bindings`(MPL-2.0)를 링크해도 전체 결합물이 MPL-2.0이 될 필요가
없다.

`bindings/`(core를 감싸는 언어별 네이티브 바인딩 계층)도 같은 이유로
MPL-2.0에 남는다: 독자적인 제품·SaaS 표면이 없는 인프라라서, 최대한
퍼미시브하게 두는 게 전략적 비용 없이 채택만 극대화한다.

## 3. `framework`가 zlink License 1.0을 따르는 조건

`framework/`는 zlink License 1.0을 따릅니다. 라이선스 정본은
[`framework/LICENSE`](../../framework/LICENSE)입니다.

라이선스 3.4절에 따라 1.0.0 전 버전은 머신 수와 관계없이 무료입니다.
1.0.0부터는 서버 한 대를 무료로 사용할 수 있고, 추가 머신마다 유료
라이선스가 필요합니다. 이미 출시한 0.30.0 이하 릴리스는 FSL-1.1-ALv2를
유지합니다.

## 4. `http-client`가 Apache-2.0을 따르는 이유

전 언어의 `framework/` 아래 있는 `http-client` 패키지는 각 플랫폼에서 흔히
쓰는 HTTP 클라이언트 라이브러리를 감싼 얇은 래퍼다 — `System.Net.Http`(.NET BCL),
`undici`(Node.js 공식 HTTP client), `java.net.http`(JDK 표준
라이브러리), Boost.Beast(C++, 이미 `core/external/boost/`에 벤더링돼
있음, Boost Software License 1.0). 넷 다 `PackageReference`/`dependencies`/
`api(...)`로 **의존성으로 호출**할 뿐 소스를 복사한 게 아니다 — 그래서
libzmq를 fork한 `core`와 달리, `http-client` 자체의 래퍼 코드에는 라이선스
상속 제약이 전혀 없다.

각 `http-client` 패키지는 표준 SPDX `Apache-2.0` 표현식을 사용하며,
실제 표기 위치는 §5와 같습니다. C++ 패키지에는 Apache-2.0 전문이
포함되고, 저장소의 정본은
[`framework/languages/cpp/http-client/LICENSE`](../../framework/languages/cpp/http-client/LICENSE)입니다.

## 5. 생태계별 실제 표기 위치

| 생태계 | `framework`(zlink License 1.0) | `http-client`(Apache-2.0) |
|--------|---------------------------------|------------------------------|
| dotnet | `Directory.Build.props`가 `PackageLicenseFile=LICENSE`를 설정하고, `Directory.Build.targets`가 packable `.nupkg`에 `framework/LICENSE`를 포함한다 | `Zlink.HttpClient.csproj`가 `PackageLicenseExpression=Apache-2.0`을 설정한다 |
| java/kotlin | 루트 `build.gradle.kts`의 Maven `pom.licenses` 블록이 zlink License 1.0 이름과 `framework/LICENSE` URL을 지정한다 | 같은 블록이 `zlink-http-client`와 `zlink-http-client-kotlin`을 Apache-2.0으로 선언한다 |
| node | 각 패키지가 `"license": "SEE LICENSE IN LICENSE"`를 사용하며 패키지 최상위 `LICENSE`에 정본 본문을 포함한다 | `http-client/package.json`은 Apache-2.0을 직접 선언하고 Apache 라이선스 전문을 포함한다 |
| cpp | 소스 archive는 `framework/LICENSE`를 포함한다. Conan은 `licenses/LICENSE`로 복사하고, vcpkg는 `"license": null`과 copyright 파일로 선언한다. 소스 헤더는 `LicenseRef-zlink-License-1.0`을 사용한다 | `framework/languages/cpp/http-client/LICENSE`에 Apache-2.0 전문을 두고 소스 헤더는 `SPDX-License-Identifier: Apache-2.0`을 사용한다 |
| Unity | `package.json`은 `"license": "Refer to LICENSE.md file"`을 사용하고 패키지 최상위 `LICENSE.md`에 정본 본문을 포함한다 | — |
