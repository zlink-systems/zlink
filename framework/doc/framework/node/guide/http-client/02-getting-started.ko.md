[← 목차](README.ko.md)

# 2. 시작하기

## 패키지 참조

이 패키지는 현재 저장소 workspace 안에서만 사용하는 private package다. 아래 의존성은 일반 npm
registry 설치 예제가 아니라 이 저장소의 workspace 또는 같은 버전의 local artifact를 사용하는
내부 소비자 설정이다.

```jsonc
// package.json
"dependencies": {
  "@zlink-systems/http-client": "0.3.0"
}
```

```ts
import { ZLinkHttpClient } from '@zlink-systems/http-client';
```

## 첫 요청

```ts
const client = ZLinkHttpClient.create('http://127.0.0.1:18080').build();
try {
  const player = await client.get('/players/7281').submit<PlayerProfile>();
  console.log(player.body.name);
} finally {
  await client.close();
}
```

- `create(baseUrl)`로 builder를 시작하고 `.build()`로 client를 만든다.
- client는 재사용 가능하다. 보통 한 번 만들어 오래 쓴다.
- `close()`로 내부 dispatcher(connection pool)를 정리한다.

## 한 줄 요청

단발 요청은 `build()`를 생략하고 builder에서 바로 메서드를 호출할 수 있다.

```ts
const res = await ZLinkHttpClient.create('https://game-api.example.internal')
  .post('/games')
  .body({ name: 'ranked-match-0611' })
  .submit<CreateGameRes>();
```

반복 호출한다면 client를 한 번 만들어 재사용하는 편이 connection pool 재사용 측면에서
유리하다.

[다음: Client 구성 →](03-client-configuration.ko.md)
