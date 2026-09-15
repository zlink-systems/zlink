**English** | [한국어](./README.ko.md)

# ZLink Framework

This directory is not zlink `core` or a language `binding` itself, but the
`ZLink Framework` workspace that sits one layer above the bindings.

`ZLink Framework` is, in character, a framework adapter layer.

The goals of this layer are:

- Narrow the axes that Framework integrates directly to four: `ROUTER <->
  ROUTER`, `SPOT`, `PUB/SUB`, and `STREAM`.
- Attach zlink-based server-to-server messaging naturally to existing
  application frameworks such as `ASP.NET Core`, `Spring`, and `NestJS`.
- Let framework users work in familiar terms — handler, client, event, DI —
  instead of raw sockets or low-level discovery configuration.
- Make direct channel calls possible by `channel_name` alone, without the
  separate gateway or dedicated load balancer that existing web server
  environments usually place in front.
- Re-wrap the foundational capabilities that `core/` and `bindings/` already
  provide — Discovery, Registry topology lookup, `SPOT` request/reply — into a
  framework-friendly API.
- Build a structure that can grow into a separate library or package on top of
  the `core` contract and the `bindings` contract, without changing either
  contract directly.

Documentation entry points:

- [ZLink Framework documentation](doc/README.en.md) — the full entry point
- [Common spec](doc/framework/common/README.en.md) — the language-neutral
  formal contract
- [Per-language documents](languages) — official languages: `.NET`, `C++`,
  `Java/Kotlin`, `Node.js`

`.NET`, `C++`, `Java/Kotlin`, and `Node.js` are the four official Framework
languages; see [doc/README.en.md](doc/README.en.md) for the current
documentation status of each one.
