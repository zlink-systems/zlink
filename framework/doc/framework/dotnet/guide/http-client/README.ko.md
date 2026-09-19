# C#/.NET HTTP Client

`Zlink.HttpClient`의 사용자 가이드다. 서버 framework 안의 handler와 mesh 밖의 도구가 외부 HTTP API를 zlink
call builder 형식으로 부르게 한다. 다섯 언어가 같은 의미론을 가지며, 이 가이드의 01~11장은 다섯 언어가
공유하는 공통 소스에서 생성된다.

| 순서 | 문서 | 내용 |
|----|------|------|
| 1 | [HTTP Client 개요](01-overview.ko.md) | 언제 쓰고 무엇을 맡지 않는가 |
| 2 | [설치와 첫 요청](02-getting-started.ko.md) | 패키지 설치, client 하나, 첫 typed 응답 |
| 3 | [요청 만들기](03-making-requests.ko.md) | method, path, query, header, 요청별 timeout |
| 4 | [요청 본문](04-request-body.ko.md) | JSON·form·multipart와 상호 배타 규칙 |
| 5 | [응답 받기](05-handling-responses.ko.md) | typed·raw·body만, 압축 응답 |
| 6 | [인증·TLS·Proxy](06-auth-tls-proxy.ko.md) | Basic/Bearer, 신뢰 인증서, mTLS, proxy |
| 7 | [Streaming](07-streaming.ko.md) | download sink와 chunked 업로드 |
| 8 | [Client와 요청의 생애](08-client-lifecycle.ko.md) | builder → client → 종결자, 옵션 표, 실행 모델 |
| 9 | [Redirect·Retry·Cookie](09-redirect-retry-cookie.ko.md) | 추적 규칙, 백오프, cookie jar 의미론 |
| 10 | [응답 처리 규칙](10-response-rules.ko.md) | status별 경로, decode, 크기 상한, 압축 해제 |
| 11 | [오류 처리](11-error-handling.ko.md) | error kind 다섯 개와 재시도 판단 |

파일 번호는 언어에 상관없이 같은 장을 가리키는 식별자다.

## 관련 문서

- 서버 가이드: [C#/.NET 서버 가이드](../server/README.ko.md)
- Stream Connector 가이드: [C#/.NET Stream Connector](../stream-connector/README.ko.md)
