[한국어](STYLE.ko.md)

[Guide index](README.en.md)

# Guide Writing Style Convention

This document is the convention followed when writing or editing
`doc/guide/` (the core guide plus the per-language binding guides). The
repository-wide rules live in [AGENTS.md](../../../AGENTS.md); this
document owns the concrete convention scoped to guides.

## 1. Directory Roles (What Goes Where)

| Directory | Reader | Contains | Does not contain |
|----------|------|---------|-----------|
| `doc/guide/` | Library users | Intent, usage, real-world examples | Internal implementation detail |
| `doc/spec/` | Binding developers | The public API contract | Usage tutorials |
| `doc/internals/` | Core maintainers | Internal structure and protocols | — |

When a guide needs to reference internal behavior, **link to internals**
instead. A user doesn't need to know about an internal socket or an inproc
endpoint.

## 2. The Golden Rule — Write A Concept Once

When the same concept is duplicated across multiple documents, drift
follows. Each document layer **writes only what it owns and links to the
rest**.

- **The canonical definition of a concept** (why DEALER, what SPOT is) is
  owned **once, by the core guide**.
- A **per-language binding guide** owns only that language's usage, type
  mapping, and language-specific rules, and links the concept back to core.
  Each section takes the shape
  **[one-line orientation] + [link to the core concept] + [that language's
  idiomatic code]**.
- Don't re-explain "what is DEALER?" in a binding guide — always link to
  core 03-3 instead.

## 3. Chapter Template

A document for each topic (a socket pattern, a capability) follows a
consistent flow.

```
Overview (one-line definition + when to use it) → Basic usage → Options → Usage patterns → Caveats/See also
```

A service or higher-level capability document uses the order **role
(what) → when (the decision criteria) → usage shape**. A guide needs
"when and why," not just "a list of features."

## 4. Binding Guide Document Structure (Shared Across Every Language)

Each language guide is a **single `index` document** that covers only that
language's surface (installation, types, ownership, the C-API-to-language
mapping table, distribution). The messaging, service, and operations
concepts are covered once by the core guide, and the language guide's "See
also" section links to the matching core document.

See the [binding guide README](../../../bindings/doc/guide/README.ko.md)
for the detailed structure.

## 5. Diagram Convention

- Draw a **stacked-layer diagram in ASCII** (for architecture layers, and
  so on), using `+---+` boxes.
- Draw a **flow/sequence diagram in Mermaid** (`flowchart`,
  `sequenceDiagram`).

## 6. Link/Path Rules

- A link from a binding guide (`doc/guide/bindings/<lang>/`) to a core
  chapter is at depth **`../../0X-...`** (`../0X` is wrong).
- A link within the same language guide is `./0X-...`.
- A link between core guide chapters is `./0X-...`.
- Every document is maintained as a `.md`/`.ko.md` pair with a mutual
  toggle link at the top.

## 7. Example Code

- An example must **match the real public API** — don't invent a method
  that doesn't exist. When in doubt, check that binding's contract/source.
- Use realistic, production-like values (ports, symbols, amounts, and so
  on).
- Put it in 1:1 correspondence with a runnable sample in
  `bindings/<lang>/samples/` where possible
  ([the example extraction convention](EXAMPLES.en.md)).

## 8. Notation

- Wrap a code identifier or constant in backticks (`` `zlink_send` ``).
- Use **bold** for emphasis (don't overuse it).
- Keep an English proper noun (SPOT, Actor, ROUTER), a code identifier, or
  server-developer jargon (socket/poller/dispatch/transport, and so on) in
  English as-is; write ordinary prose in Korean.
- On a term's first appearance, link it to the [glossary](glossary.en.md)
  or attach a one-line definition.
