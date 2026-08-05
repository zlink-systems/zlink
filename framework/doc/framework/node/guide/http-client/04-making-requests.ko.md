[← 목차](README.ko.md)

# 4. Request 만들기

## HTTP 메서드

```ts
client.get('/players/7281');
client.post('/games');
client.put('/games/42');
client.delete('/games/42');
client.patch('/games/42');
client.head('/games/42');
client.options('/games');
```

path는 반드시 `/`로 시작한다. `baseUrl`에 경로 prefix가 있으면 prefix와 결합된다
(예: base `http://h/api` + path `/games` → `/api/games`).

## query 파라미터

`query(name, value)`는 percent-encoding된 query 파라미터를 추가한다.

```ts
await client.get('/search').query('q', 'ranked match').query('limit', '20').submitRaw();
// → /search?q=ranked%20match&limit=20
```

## 헤더

요청별 헤더는 `header(name, value)`로 추가한다. client 기본 헤더와 합쳐지며, 같은
이름이면 요청별 값이 우선한다.

```ts
await client.get('/players/7281').header('x-trace-id', 'abc-123').submitRaw();
```

## 요청별 timeout

```ts
await client.get('/slow').timeout(10000).submitRaw();
```

[다음: Request Body →](05-request-body.ko.md)
