[한국어](README.ko.md) | English

[Core specification](../spec/core/README.en.md) · [Core guide](../guide/01-overview.en.md)

# ZLink Core reference

The writing rules follow the
[reference-writing guide](../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only). That guide was written for fluent builder chains in a managed-language framework;
Core is a flat C ABI with no builder and no terminal, so this document states the mapping once,
here, rather than improvising it per entry.

## Entry-unit mapping for a C API

| Framework reference section | Core reference section | Meaning here |
|---|---|---|
| Heading | Heading | One exported function, or a tightly-coupled function pair/family that only makes sense called together (e.g. `zlink_ctx_set`/`zlink_ctx_get`, `zlink_poller_add`/`_modify`/`_remove`) |
| Code example | Code example | A minimal C call showing the function signature in use |
| **옵션 (Options)** | **Parameters** | The function's parameters, applicable flag values, and any option-struct fields it reads or writes — not a builder chain, since there is none |
| **완료 결과 (Completion result)** | **Return and errno** | The return value's meaning per outcome, and the `errno`/typed-result values Core sets. Core has no async completion — every call here is synchronous from the caller's perspective, even when it starts asynchronous I/O internally |
| **선택 기준 (When to use)** | **When to use** | Unchanged in spirit |

Core's failure model differs from framework's single `FrameworkException.kind`: each API family
returns its own typed result enum (`zlink_config_result_t`, `zlink_connect_result_t`,
`zlink_submit_result_t`, etc.), and `zlink_errno()` carries the same-thread detailed cause. The
"Errors, results, and version" category below is this reference's counterpart to framework's
error-kind correspondence table — read it once, then each entry's "Return and errno" section
only needs to name the values that entry actually produces.

## Locale convention

Unlike the framework interface files, every Core spec document already exists in both `en` and
`ko`. Core's original locale is English — en is authoritative, ko is the translation — the
reverse of the framework interface convention. So:

- Write `.en.md` first, `.ko.md` second.
- A reference file's spec citation links to the **same-locale** spec file (`habitat.en.md` →
  `../spec/core/01-context.en.md`; `habitat.ko.md` → `../spec/core/01-context.ko.md`). No
  "(Korean-only)" note anywhere in this tree, because there is no Korean-only source here.

## Category

Core's public surface is far more granular than framework's curated 8 categories — it is a raw
C ABI with ~90 exported functions across 9 common-contract chapters and 8 socket types. Two spec
chapters have no entry points of their own and are not reference categories:
[Public-contract governance](../spec/core/00-public-contract-governance.en.md) (documentation
policy, not API) and [Runtime boundary](../spec/core/09-runtime-boundary.en.md) (a scope
statement — see [Core internals](../internals/architecture.en.md) instead if you need the
capability boundary). [Events](../spec/core/05-events.en.md) is a catalog that relates the three
event families to each other rather than exposing functions itself; its content is folded into
the "When to use" prose of Polling and Socket monitor below instead of becoming its own category.

| Category | Status | Corresponding spec |
|---|---|---|
| [Context](01-context.en.md) | Drafted | 01-context |
| [Message](02-message.en.md) | Drafted | 02-message |
| [Socket lifecycle](03-socket-lifecycle.en.md) | Drafted | socket/README §Functions (create/bind/connect/disconnect/close) |
| [Socket options and identity](04-socket-options.en.md) | Drafted | socket/README §Socket Options, §Dedicated Functions |
| [Raw receive](05-raw-receive.en.md) | Drafted | socket/README §Receive Model Summary, 03-errors §4 |
| [PAIR](06-pair.en.md) | Drafted | socket/01-pair |
| [PUB](07-pub.en.md) | Drafted | socket/02-pub |
| [SUB](08-sub.en.md) | Drafted | socket/03-sub |
| [XPUB](09-xpub.en.md) | Drafted | socket/04-xpub |
| [XSUB](10-xsub.en.md) | Drafted | socket/05-xsub |
| [DEALER](11-dealer.en.md) | Drafted | socket/06-dealer |
| [ROUTER](12-router.en.md) | Drafted | socket/07-router |
| [STREAM](13-stream.en.md) | Drafted | socket/08-stream |
| [Socket monitor](14-socket-monitor.en.md) | Drafted | 07-monitoring, 05-events |
| [Polling and pollers](15-polling.en.md) | Drafted | 06-polling, 05-events |
| [Timers](16-timers.en.md) | Drafted | 08-utilities §Timers |
| [Utilities](17-utilities.en.md) | Drafted | 08-utilities §Atomic Counter, §Stopwatch, §Miscellaneous |
| [Errors, results, and version](18-errors.en.md) | Drafted | 03-errors, 04-errno-map |

Filenames follow `NN-slug.en.md`/`NN-slug.ko.md` in the numbering above once drafted; the table
only links a category once its file exists, so this table never carries a dangling link.

This document tree is wired into `mkdocs.yml` nav.
