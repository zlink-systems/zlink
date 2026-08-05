[← 목차](README.ko.md)

# 5. Request Body

body 소스는 **상호 배타**다. `body`, `bodyStream`, `form`, `multipart` 중 하나만 쓸 수
있고 둘 이상 지정하면 예외로 실패한다.

## typed JSON

```kotlin
client.post("/games").body(CreateGameReq("ranked-match-0611")).await<CreateGameRes>()
```

`body(value)`는 값을 Jackson으로 직렬화하고 `content-type: application/json`을 설정한다.
`data class`가 그대로 직렬화된다.

## raw body

```kotlin
client.post("/raw").body("plain text payload", "text/plain").awaitRaw()
```

`body(content, contentType)`(2 인자)는 raw 본문과 명시적 content-type을 설정한다.

## form-urlencoded

```kotlin
client.post("/login").form("user", "aria").form("password", "secret value").awaitRaw()
// content-type: application/x-www-form-urlencoded, percent-encoding 적용
```

## multipart/form-data

```kotlin
client.post("/upload")
    .multipart("title", "patch notes")
    .multipartFile("file", "notes.txt", fileContent, "text/plain")
    .awaitRaw()
```

## streaming 업로드

`bodyStream(provider, contentType)`는 body를 chunk 단위로 chunked transfer-encoding으로
전송한다. provider는 `null`을 돌려주면 끝난다. streaming 요청은 rewind할 수 없으므로
**자동 retry에서 제외**된다.

```kotlin
val chunks = ArrayDeque(listOf("a".toByteArray(), "b".toByteArray()))
client.post("/upload-stream")
    .bodyStream({ if (chunks.isEmpty()) null else chunks.poll() }, "application/octet-stream")
    .awaitRaw()
```

[다음: Response 다루기 →](06-handling-responses.ko.md)
