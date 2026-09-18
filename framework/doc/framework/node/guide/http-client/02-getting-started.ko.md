[← 목차](README.ko.md)

# 2. 시작하기

## 패키지 참조

이 패키지는 npm registry에 배포한다.

```bash
npm install @zlink-systems/http-client
```

```jsonc
// package.json
"dependencies": {
  "@zlink-systems/http-client": "0.16.0"
}
```

```ts
import { ZLinkHttpClient } from '@zlink-systems/http-client';
```

## 첫 요청

```ts
const client = ZLinkHttpClient.create('http://127.0.0.1:18080').build();
try {
  const player = await client.get('/players/7281').async<PlayerProfile>();
  console.log(player.body.name);
} finally {
  await client.close();
}
```

- `create(baseUrl)`로 builder를 시작하고 `.build()`로 client를 만든다.
- client는 재사용 가능하다. 보통 한 번 만들어 오래 사용한다.
- `close()`로 내부 dispatcher(connection pool)를 정리한다.

## 한 줄 요청

단발 요청은 `build()`를 생략하고 builder에서 바로 메서드를 호출할 수 있다.

```ts
const res = await ZLinkHttpClient.create('https://game-api.example.internal')
  .post('/games')
  .body({ name: 'ranked-match-0611' })
  .async<CreateGameRes>();
```

반복 호출한다면 client를 한 번 만들어 재사용하는 편이 connection pool 재사용 측면에서
유리하다.

[다음: Client 구성 →](03-client-configuration.ko.md)
