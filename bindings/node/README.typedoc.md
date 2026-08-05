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
- Message and result types (`Message`, `Received`, `SendResult`)
- Constants and enums (`SocketType`, `SocketOption`)
- Internal symbols marked `@internal` are excluded

## Callback Handler Capacity

Native callback delivery uses fixed thread-safe-function slots. The binding
supports up to eight concurrently attached handlers for each callback family:
stream packet handlers, send-ready handlers, socket monitor handlers, and
timer fire handlers. Close the owning socket, monitor, or timer before
attaching more handlers in the same family.
