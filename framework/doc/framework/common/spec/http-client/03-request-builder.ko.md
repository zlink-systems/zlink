# 3. Request 계약

> [공통 계약 목차](README.ko.md)

## 3.1 HTTP 메서드

7종: `get / post / put / delete / patch / head / options`. client 인스턴스와
client builder(one-shot) 양쪽에서 동일 이름으로 제공한다.

언어 편차: cpp는 키워드 회피로 `delete_`.

## 3.2 path와 query

- path는 `/`로 시작해야 한다. 아니면 `ProtocolError`.
- `query(name, value)`는 percent-encoding을 적용해 URL에 누적한다.
- baseUrl + path 결합과 query 직렬화 결과는 5개 언어에서 동일해야 한다
  (계약 테스트 축).

## 3.3 헤더

- `header(name, value)` — 요청별 헤더. **client `defaultHeader`보다 우선**한다.
- 헤더 name은 대소문자 무시로 비교한다.
- 래퍼는 다음을 자동 주입하며 사용자가 override할 수 있다:
  `user-agent: zlink-http-client/<버전>`, `accept: application/json`.

## 3.4 요청별 timeout

`timeout(시간)` — 이 요청에 한해 client 기본 timeout을 대체한다.
의미론은 [6장 §6.2](06-redirect-retry-cookie.ko.md)의 시도당 timeout과 동일.

## 3.5 body 소스 5종과 배타 규칙

| 소스 | 시그니처(개념) | content-type | retry |
| --- | --- | --- | --- |
| typed JSON | `body(dto)` | `application/json` 자동(명시 시 미덮음) | 가능 |
| raw | `body(content, contentType)` | 인자 그대로 | 가능 |
| streaming 업로드 | `bodyStream(provider, contentType)` | 인자 그대로, chunked 전송 | **제외** |
| form | `form(name, value)` 누적 | `application/x-www-form-urlencoded` | 가능 |
| multipart | `multipart(name, value)` / `multipartFile(name, filename, content, contentType)` 누적 | `multipart/form-data` + boundary | 가능 |

- **상호 배타**: 서로 다른 소스를 한 요청에 섞으면 `ProtocolError`
  ("single body source").
- typed JSON 직렬화는 언어 codec 계층에 위임한다: cpp `to_json`(nlohmann ADL),
  dotnet codec registry(기본 `System.Text.Json` Web), java/kotlin Jackson,
  node `JSON.stringify`.
- streaming provider는 pull형이다: chunk를 반환하고, 종료를 언어 관용의
  "없음" 값으로 알린다(cpp `std::nullopt`, dotnet/java/kotlin/node `null`).
- streaming 업로드는 rewind가 불가능하므로 retry와 redirect 재전송에서
  제외된다([6장](06-redirect-retry-cookie.ko.md)).
- `multipartFile`의 content는 현행 계약상 문자열이다. 바이너리 파일 업로드는
  `bodyStream`으로 우회한다(개정 후보 [R4](10-revision-candidates.ko.md)).

## 3.6 예시 (개념 표기)

```
client.post("/games")
      .header("x-request-id", "req-8f2c41")
      .query("region", "kr")
      .body(create_game_req)     // typed JSON
      .timeout(3s)
      .submit<create_game_res>() // C++·Java의 typed response terminator
                                 // Node는 async<create_game_res>() 사용
```
