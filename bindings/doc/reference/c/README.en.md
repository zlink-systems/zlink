[한국어](README.ko.md) | English

[C binding spec](../../spec/c/README.en.md)

# C bindings reference

**This binding has no reference tier of its own — it points to
[core's reference tree](../../../../core/doc/reference/README.en.md) instead.**

Every other bindings language in this tree (dotnet/cpp/java/node/rust/python/go) wraps
`core/include/zlink.h` behind a language-specific `Contracts`/`contracts` layer with its own
types, builders, and naming — that wrapper layer is what each of those languages' reference trees
documents. The C binding does not do this. Per the
[C binding spec](../../spec/c/README.en.md#public-contract-source):

> In C, the native ABI itself is the binding contract. `bindings/c` does not add a second
> contract/runtime layer on top of the core C API.

`core/include/zlink.h` **is** the C binding's public contract, verbatim — the same header core's
own 18-category reference tree
([`core/doc/reference/`](../../../../core/doc/reference/README.en.md)) already documents function
by function: `zlink_ctx_new`, `zlink_send_part`, `zlink_socket`, `zlink_poller_wait`, and every
other exported symbol. Writing a second `bindings/doc/reference/c/01-*.md` through `05-*.md` set
here would either duplicate that tree's content under different headings, or thin it out to a
cross-reference index — neither adds information the reader can't already get from
`core/doc/reference/` directly.

## What actually differs about `bindings/c`

The C binding spec (not this reference tree) is the place that documents the parts specific to
`bindings/c` itself, distinct from the ABI:

- [Repository structure](../../spec/c/README.en.md#repository-structure) — `bindings/c/include/`,
  `bindings/c/tests/`, `bindings/c/samples/`, `bindings/c/perf/` versus `core/include/`/`core/src/`.
- [Interface shape exceptions](../../spec/c/README.en.md#interface-shape-exceptions) — where the
  higher-level wrapper-binding conventions this reference tree's other languages follow (fluent
  builders, `Received.Reply()`, exceptions instead of result codes) do not apply to C at all.
- [Required feature coverage](../../spec/c/README.en.md#required-feature-coverage) and
  [Actor and Spot route results](../../spec/c/README.en.md#actor-and-spot-route-results) — review
  checklists specific to this binding's tests/samples, not additional public API surface.

None of these are entry-unit reference material in the sense this tree's other language
categories are — they are packaging, testing, and review-process rules layered on top of an ABI
that is already fully documented elsewhere.

## Locale convention

Every `bindings/doc/spec/<lang>/` document is English-original, Korean-translation, matching this
file.

---

See the [C binding spec](../../spec/c/README.en.md) and
[core's reference tree](../../../../core/doc/reference/README.en.md) for the actual API
documentation.
