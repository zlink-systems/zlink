[← Table Of Contents](README.en.md)

# 4. Making Requests

## HTTP Methods

```ts
client.get('/players/7281');
client.post('/games');
client.put('/games/42');
client.delete('/games/42');
client.patch('/games/42');
client.head('/games/42');
client.options('/games');
```

The path must start with `/`. If `baseUrl` has a path prefix, it's combined with the prefix (e.g.,
base `http://h/api` + path `/games` → `/api/games`).

## Query Parameters

`query(name, value)` adds a percent-encoded query parameter.

```ts
await client.get('/search').query('q', 'ranked match').query('limit', '20').submitRaw();
// → /search?q=ranked%20match&limit=20
```

## Headers

Per-request headers are added with `header(name, value)`. They're merged with the client's default
headers, and for the same name, the per-request value wins.

```ts
await client.get('/players/7281').header('x-trace-id', 'abc-123').submitRaw();
```

## Per-Request Timeout

```ts
await client.get('/slow').timeout(10000).submitRaw();
```

[Next: Request Body →](05-request-body.en.md)
