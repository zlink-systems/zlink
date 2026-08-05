[← Table Of Contents](README.en.md)

# 2. Getting Started

## Package Reference

This package is currently a private package used only within this repository's workspace. The
dependency below is not a general npm registry install example — it's an internal consumer setup
that uses this repository's workspace or a local artifact of the same version.

```jsonc
// package.json
"dependencies": {
  "@zlink-systems/http-client": "0.3.0"
}
```

```ts
import { ZLinkHttpClient } from '@zlink-systems/http-client';
```

## First Request

```ts
const client = ZLinkHttpClient.create('http://127.0.0.1:18080').build();
try {
  const player = await client.get('/players/7281').submit<PlayerProfile>();
  console.log(player.body.name);
} finally {
  await client.close();
}
```

- Start a builder with `create(baseUrl)` and build the client with `.build()`.
- The client is reusable. Typically you create it once and use it for a long time.
- `close()` cleans up the internal dispatcher (connection pool).

## One-Line Request

For a one-off request, you can skip `build()` and call methods directly on the builder.

```ts
const res = await ZLinkHttpClient.create('https://game-api.example.internal')
  .post('/games')
  .body({ name: 'ranked-match-0611' })
  .submit<CreateGameRes>();
```

If you call it repeatedly, creating the client once and reusing it is better for connection pool
reuse.

[Next: Client Configuration →](03-client-configuration.en.md)
