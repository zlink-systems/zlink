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

The current public contract does not install completion handlers. Managed send
uses `submit()`/`submit_sync()`, request uses the matching terminals returning
reply parts, and STREAM, socket-monitor, and timer input is caller-driven pull.
If a public poller owns `PollEventFlag.PollCompletion`, its owner keeps calling
`wait()` so native completions are drained and settled.
