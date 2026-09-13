# Node.js Framework perf

Implements the 21 default cells from the common Framework perf README. The shared
matrix and nanosecond histogram bounds are loaded from `framework/perf-contract/`.
Framework messages use public typed JSON DTOs; no codecs, binding source adapters
or private runtime APIs are used.

After the Framework workspace packages have been built, run a role with:

```sh
node framework/languages/node/perf/main.js --config role.json
```

The shared runner starts CS clients with the same entry point:

```sh
node framework/languages/node/perf/main.js --endpoint-config endpoints.json --client-index 0
```

Node 22 or later supplies the platform WebSocket API for CS connectors. Service
connections use TCP. Each role exposes separate admin and application listeners:
`GET /perf/ready`, `GET /perf/stats`, `POST /perf/reset`,
`POST /app/perf/prepare`, and `POST /app/perf/start`. Preparation is called once
after all infrastructure is ready. CS clients use newline JSON commands on stdin.

Run the application accounting checks without native transport or a perf run:

```sh
node --test framework/languages/node/perf/tests/accounting.test.js
```

Delivery evidence consists of compact per-stream sequence ranges. A coordinator
intersects receiver evidence with source window admission/publish evidence. The
role snapshot leaves unaggregated delivery ratios null. Worker call, task, dispatch and continuation spans use the same process monotonic
clock. Cross-process one-way delivery timing remains null without verified clock
alignment.
