# zlink Node Binding API Reference

This reference is generated from the TypeScript source in `bindings/node/src/`.

## Prerequisites

```bash
npm install --save-dev typedoc
```

## Generate

```bash
cd bindings/node
npx typedoc
```

Generated HTML entrypoint:

```text
bindings/node/typedoc/html/index.html
```

## Scope

- Public exports from `src/index.ts`
- Socket types and domain objects
- Message and result types (`Message`, `Received`, `SubmitError`)
- Constants and enums (`SocketType`, `SocketOption`)
- Internal symbols marked `@internal` are excluded

## Callback Handler Capacity

Send completion and request reply callbacks are installed once per socket and
use N-API thread-safe functions only to deliver completion data into
JavaScript. The callback does not submit, wait, retry, or own a binding queue.
There is no send-ready or publisher-admission callback surface. Stream packet,
socket monitor, and timer callbacks retain their existing delivery limits.
