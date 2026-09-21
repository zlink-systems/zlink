---
title: "Service Wire Schema Dialect"
---

# Service Wire Schema Dialect

[Channel·Transport topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 06. Service Wire Protocol](06-wire-protocol.en.md)

> **What this chapter answers** — how to read
> `framework/runtime/protocol/service-wire-v1.schema.json`: what each top-level key declares, how
> the ten `types[].kind` values lay out bytes, and when and how encoders and decoders evaluate
> keywords such as `$ref`, `$bound`, `when`, and `constraints`.
>
> **Contract ownership** — the schema owns the values; this chapter owns how to read them. This
> chapter lists no command table, field list, numeric bound, or enum value — open the schema for
> those. Conversely, a new top-level key, a new `kind`, or a new keyword in the schema is a dialect
> change and updates this chapter in the same change. A key or keyword that exists in the schema
> but not in this chapter is a defect of this chapter, not of the schema.
>
> **Related contracts** — [06. Service Wire Protocol](06-wire-protocol.en.md) (frame layout,
> command list, decode bounds, verification requirements) ·
> [Message model §5](../00-foundation/05-message-model.en.md#5-the-framework-json-v1-typed-payload-profile)
> (typed payload JSON) · tool usage in
> [`framework/runtime/protocol/README.ko.md`](../../../../../../runtime/protocol/README.ko.md)

| Section | Covers |
|---|---|
| [1. Dialect and Consumers](#1-dialect-and-consumers) | dialect identity, the three consumers of the schema, the status of sentence-like string values |
| [2. Top-Level Keys](#2-top-level-keys) | the 28 keys — two dialect identifiers and four groups (protocol constants, layout declarations, semantic declarations, profile declarations) |
| [3. Rules Common to Every Layout](#3-rules-common-to-every-layout) | byte order, integer encodings, `$ref`·`$bound`, size bounds and allocation order, the kind of failure |
| [4. The Ten `types[].kind` Values](#4-the-ten-typeskind-values) | declared keys, byte layout, and decode failure conditions per kind |
| [5. Field Keywords](#5-field-keywords) | evaluating `constant`·`minimum`·`maximum`·`when`·`otherwise`·`required`·`constraints` |
| [6. `commands[]` and `flags[]`](#6-commands-and-flags) | command entry keys, flag declarations, payload policy, why direction is not in the schema |
| [7. Durable Formats and the Logical Stream](#7-durable-formats-and-the-logical-stream) | header·checksum·fixture declarations of `durableFormats[]` and `relocationLogicalStreamFormat` |
| [8. Semantic Declarations](#8-semantic-declarations) | what `semanticContexts`, `semanticConstraints`, and `relocationStateMachine` pin down |
| [9. Profiles and Owning Clauses](#9-profiles-and-owning-clauses) | which spec clause each of the 15 profiles pins in machine-readable form |
| [10. Verification Requirements](#10-verification-requirements) | what confirms a dialect change |

## 1. Dialect and Consumers

`schemaDialect` is `zlink-service-wire-schema-v1` and `schemaVersion` is `1`. These two values
identify the reading rules described in this chapter. When a reading rule changes, the dialect value
changes; when only values change (a new command, an adjusted bound, an added field), the dialect
stays the same.

Three consumers read the schema.

| Consumer | What it reads | Where |
|---|---|---|
| Validator | The whole schema. Checks reference integrity, length capacity, closed unions, TLV order, durable checksums, and literal equality of semantic declarations | `validate-service-wire-schema.mjs` |
| Generator | The whole schema the validator accepted. Lowering turns this chapter's reading rules into a language-neutral operation IR once; each language renderer emits only that operation's syntax. Constant tables, codecs and the fixture index are produced from one manifest, with drift checks | `service-wire-lowering.mjs`, `render-service-wire-*.mjs`, `generate-service-wire-{assets,fixtures,codecs}.mjs`, `service-wire-output-manifest.mjs` |
| Runtime codec | The layouts in `types`, `commands`, `durableFormats`, and `relocationLogicalStreamFormat`. Generated or handwritten, it produces and reads bytes by the rules in this chapter | each language runtime |

Only a schema that the validator accepts is the reference. Codecs and fixtures built from a
rejected schema are not the contract.

**Sentence-like string values.** Profiles and semantic declarations contain string values that join
a sentence with hyphens, such as
`source-memory-payload-is-the-only-handoff-source-for-work-and-timers-accepted-before-capture`.
These are not free prose. They are **literals** that the validator compares byte for byte against
its expected values, and the rule a literal stands for is stated in prose by the owning clause in
[§9](#9-profiles-and-owning-clauses). Changing such a value therefore changes the owning clause, and
changing the owning clause changes the value — a change on one side alone is rejected by the
validator or in review.

## 2. Top-Level Keys

Apart from the two identifiers `schemaDialect` and `schemaVersion` ([§1](#1-dialect-and-consumers)), the 26 keys play four roles.

**Protocol constants** — values shared by every record.

| Key | Declares |
|---|---|
| `protocol` | `name`, `magic` (byte array), `wireMajor`, `byteOrder`, `headPrefixBytes`, `requiredCapability`. The head prefix in [06 §2](06-wire-protocol.en.md#2-record-framing-and-decode) uses these values as they are |
| `bounds` | a list of named positive integer limits, `{ "name", "value" }`. Layouts point at them with `$bound` ([§3](#3-rules-common-to-every-layout)) |
| `flags` | the bit declarations of the head prefix flags byte ([§6](#6-commands-and-flags)) |
| `reservedCommandRanges` | closed ranges `{ "first", "last" }` of command IDs that are never assigned |

**Layout declarations** — the arrangement of bytes.

| Key | Declares |
|---|---|
| `types` | the list of named layouts. Every layout has `name` and `kind`, and the kind determines the remaining keys ([§4](#4-the-ten-typeskind-values)) |
| `commands` | the list of command entries. `body` is the layout of the bytes after the head prefix ([§6](#6-commands-and-flags)) |
| `durableFormats` | header·body·checksum declarations of the four envelopes that persist in a store ([§7](#7-durable-formats-and-the-logical-stream)) |
| `relocationLogicalStreamFormat` | the declaration of the relocation logical stream that flows directly from source to target ([§7](#7-durable-formats-and-the-logical-stream)) |

**Semantic declarations** — rules outside the bytes, pinned so that a machine can read them.

| Key | Declares |
|---|---|
| `semanticContexts` | name, type, and origin of discriminator values that come from outside the layout ([§8](#8-semantic-declarations)) |
| `semanticConstraints` | a list of literal tuples for lifecycle·fence·ordering rules, one per kind ([§8](#8-semantic-declarations)) |
| `relocationStateMachine` | commit order, transitions, and per-command rules of relocation phases ([§8](#8-semantic-declarations)) |

**Profile declarations** — machine-readable pins of rules that other spec clauses own in prose.
There are 15, and the table in [§9](#9-profiles-and-owning-clauses) connects each to its owning
clause.

## 3. Rules Common to Every Layout

**Byte order.** The only value `protocol.byteOrder` and every `durableFormats[].byteOrder` accept
is `big-endian`. There is no place to declare another byte order. The byte-order rule of the wire
itself is owned by [06 §2](06-wire-protocol.en.md#2-record-framing-and-decode).

**Integer encodings.** The `encoding` of an `integer` kind is one of five — `u8`, `u16`, `u32`,
`u64`, `i64` — and all are fixed width. This dialect has no variable-length integers. Lengths,
counts, discriminators, and field IDs all point with `$ref` at one of these five; a durable checksum
states the same width directly as `encoding: "u32-big-endian"` ([§7.1](#71-durableformats)).

**`$ref`.** Names a `types[].name`. A reference to an undeclared name makes the validator reject
the schema. The bytes at a `$ref` are exactly the named layout — no header or length prefix is
added (when a length is needed, the referenced layout declares it through its own kind).

**`$bound`.** Names a `bounds[].name`. It appears in place of a number in `maximumBytes`,
`maximumItems`, `maximumEncodedBytes`, and `maximum`. Every declared bound must be referenced at
least once; the validator rejects an unreferenced bound. This keeps one limit from being written as
two different numbers in two places.

**Size bounds.** `maximumEncodedBytes` is the encoded bound of the whole layout, and the validator
computes that the sum of the component bounds per kind does not exceed it. `runtimeMaximumBytes` and
`runtimeMaximumEncodedBytes` declare per-topology negotiated bounds (`$negotiatedBound`); a
negotiated value never exceeds `absoluteMaximum` (a `$bound`). The order in which bounds are checked
and how an excess is rejected are owned by [06 §2](06-wire-protocol.en.md#2-record-framing-and-decode).

**The kind of failure.** Every condition this chapter calls a "failure" is a protocol error under
[06 §2](06-wire-protocol.en.md#2-record-framing-and-decode). An encoder does not produce bytes for
input that breaks the same declaration.

**Encoder and decoder read the same declaration.** No keyword exists "for the encoder only" or
"for the decoder only". The encoder produces bytes as declared, and the decoder rejects bytes that
differ from the declaration. This chapter states the two sides separately per keyword not because
the rules differ but because the place where a violation surfaces differs.

## 4. The Ten `types[].kind` Values

Declared keys and byte layout per kind. "Failure" is the protocol error of
[§3](#3-rules-common-to-every-layout).

### 4.1 `integer`

| Key | Meaning |
|---|---|
| `encoding` | one of `u8`·`u16`·`u32`·`u64`·`i64` |
| `minimum`, `maximum` | the value range, as a number or `$bound`. Written only when narrower than the encoding's own range |

The layout is one big-endian integer of the encoding's width. Failure: too few bytes, a value
outside the range.

### 4.2 `enum`

| Key | Meaning |
|---|---|
| `encoding` | an integer encoding |
| `values` | a list of `{ "name", "value" }`. Neither names nor values repeat |

The layout is one integer of the encoding's width, and only the values in `values` are valid.
Failure: too few bytes, an undeclared value. Names never go on the wire — the generated constant
tables and fixtures use them.

### 4.3 `length-prefixed-bytes`

| Key | Meaning |
|---|---|
| `lengthType` | the integer layout of the length prefix (`$ref`) |
| `minimumBytes`, `maximumBytes` | the length range. `maximumBytes` is usually a `$bound` |
| `runtimeMaximumBytes` | a per-topology negotiated bound ([§3](#3-rules-common-to-every-layout)) |
| `zeroLengthMeaning` | `absent`. Written only on optional layouts where length 0 means "no value" |

The layout is the length prefix followed by exactly that many raw bytes. Failure: a short prefix, a
length outside the range, too few bytes. Without `zeroLengthMeaning`, length 0 is an empty value,
not an absent one.

### 4.4 `length-prefixed-text`

Adds the following to the keys of `length-prefixed-bytes`.

| Key | Meaning |
|---|---|
| `encoding` | `utf-8` |
| `nul` | `forbidden`. The text carries no NUL byte |

The layout is the same. Failure adds an invalid UTF-8 sequence and a NUL byte. The length counts
bytes, not code points.

### 4.5 `struct`

| Key | Meaning |
|---|---|
| `fields` | the field list. Declaration order is byte order ([§5](#5-field-keywords)) |
| `constraints` | constraints across fields: `not-both-zero` (`fields`), `field-less-than-or-equal` (two fields of the same struct) |
| `maximumEncodedBytes` | the encoded bound |
| `trailingBytes` | `forbidden`. Rejects leftover bytes in a struct whose length is fixed from outside |
| `presence`, `scope`, `storage`, `metadataMeaning`, `queueMeaning` | literals stating where this layout appears and what it means. No effect on byte layout; referenced by the rules in [§8](#8-semantic-declarations) and [§9](#9-profiles-and-owning-clauses) |

The layout is the `fields` concatenated in declaration order. There are no separators, padding, or
alignment. A field whose `when` is false does not exist in the bytes ([§5](#5-field-keywords)).
Failure: any field's failure, a constraint violation, leftover bytes under `trailingBytes: forbidden`.

### 4.6 `vector`

| Key | Meaning |
|---|---|
| `countType` | the integer layout of the count prefix |
| `maximumItems` | the count bound, as a number or `$bound` |
| `item` | the item layout (`$ref`) |
| `constraints` | `sorted`, `unique`. `sorted` always carries a `comparison` — `canonical-authority-key-bytes`, `utf-8-bytes`, `wire-value`, `wire-value-then-utf-8-bytes`, `unsigned-wire-value`. `unique` names what is compared with `field` (one) or `fields` (several), and its `comparison` is optional |
| `participantIdDerivation` | a literal stating how an item's participant identifier is obtained. No effect on layout |

The layout is the count prefix followed by the item repeated count times. `sorted` requires
ascending order under the declared comparison; `unique` requires that no two items compare equal.
Both exist for canonical encoding, so a decoder **rejects** an unsorted or duplicated vector rather
than repairing it while reading. Failure: a short count, a count over the bound, an item failure, an
order or duplicate violation.

### 4.7 `versioned-vector`

| Key | Meaning |
|---|---|
| `layout` | exactly three entries — a version field with `constant`, a count field whose `counts` names the repeated field, and a repeated field with `kind: "repeat"`, `countFrom`, and `item` |
| `constraints`, `maximumEncodedBytes`, `trailingBytes` | as for `vector` and `struct` |

The layout is version, count, then the repeated items. Failure: a version mismatch, a `vector`
failure, leftover bytes. Layouts that occupy a whole frame of their own, such as `metadata-frame`,
use this kind.

### 4.8 `versioned-length-delimited`

| Key | Meaning |
|---|---|
| `version` | an integer field with `constant` |
| `length` | an integer layout with `covers: "body"`. The length covers the whole body |
| `body` | a field list, as `struct.fields` |
| `maximumEncodedBytes`, `runtimeMaximumEncodedBytes`, `trailingBytes` | as above |
| `constraints` | the terminal envelope shape constraints — `terminal-success-shape`, `terminal-failure-shape`, `existing-has-no-application-payload` |
| `correlationFields`, `description` | a literal naming the correlation identifier fields, and a description. No effect on layout |

The layout is version, length, body. The decoder reads the length, checks the bound, then reads
exactly that many body bytes. A body shorter or longer than the length is a failure. Because the
length field fixes this kind's boundary against the bytes that follow, layouts carried in another
frame — such as the typed payload envelope — use it.

### 4.9 `conditional-union`

| Key | Meaning |
|---|---|
| `discriminators` | the list of values that select a case. Each entry's `source` gives the origin — the string `"wire"` (a value read immediately before this layout, whose layout is the entry's `$ref`), the object `{ "enclosingField": "<field>" }` (a field already read from the struct containing this union), the object `{ "context": "<name>" }` (a name in `semanticContexts`) |
| `bodyLengthType`, `bodyLengthCovers` | a length prefix before the selected case and its coverage (`selected-case`). A union without them places the case directly, with no length prefix |
| `cases` | a list of `{ "when": { discriminator: value, … }, "fields": [...], "constraints": [...] }`. Every discriminator is assigned exactly once. `constraints` is optional and applies the same kinds as a struct's `constraints` (§4.5) to that case's fields |
| `otherwise` | when no case matches — `protocol-error` (reject) or an explicit field list `{ "fields": [...] }` (the current schema uses only the empty list) |
| `maximumEncodedBytes`, `trailingBytes` | as for `struct` (§4.5) |
| `presence`, `release` | literals stating when the union appears and when it is released. No effect on layout |

The layout is the `source: wire` discriminators, the body length (if declared), then the fields of
the selected case. `enclosingField` and `context` discriminators occupy no bytes. Failure: a
discriminator failure, no matching case under `otherwise: protocol-error`, a length inconsistent
with the case, leftover bytes.

### 4.10 `tlv32`

| Key | Meaning |
|---|---|
| `totalLengthType` | the integer layout of the total length prefix |
| `fieldIdType`, `fieldLengthType` | the integer layouts of each field's ID and length prefix |
| `fields` | a list of `{ "id", "name", "$ref", "required", … }` |
| `encodingOrder` | `ascending-field-id`. Fields appear only in ascending ID order |
| `duplicateField` | `protocol-error`. Two fields with the same ID are rejected |
| `unknownField` | `skip-after-length-validation`. An unknown ID is skipped after its length is checked |
| `maximumEncodedBytes`, `trailingBytes` | as above |

The layout is the total length followed by repeated (field ID, field length, field value). This is
the only kind that can omit or add fields, so extensible descriptors use it. Failure: a field
extending past the total length, an order violation, a duplicate ID, a missing `required: true`
field, a value failure of a known field. An unknown field is not a failure — the decoder skips its
length.

## 5. Field Keywords

The keywords used by field entries in `struct.fields`, `versioned-length-delimited.body`,
`conditional-union.cases[].fields`, and `commands[].body`.

| Keyword | Meaning | Encoder | Decoder |
|---|---|---|---|
| `name` | the field name, unique within its field list | — | — |
| `$ref` | the layout ([§3](#3-rules-common-to-every-layout)) | writes with that layout | reads with that layout |
| `constant` | the value this field must carry (a version, a magic, …) | writes that value | any other value is a failure |
| `minimum`, `maximum` | a narrower range that applies to this field alone when `$ref` is an integer | rejects a value outside the range | a value outside the range is a failure |
| `when` | the condition under which this field exists: `fieldPresent: "<earlier field>"`, `fieldEquals: { "name": "<earlier field>", "value": … }`, or `allFlagsSet: [...]`·`anyFlagsSet: [...]` over the head prefix flags. It references **only earlier fields and flags** | writes nothing when the condition is false | reads nothing when the condition is false. The condition is decided from values already read |
| `otherwise` | when `when` is false — `forbidden` | rejects a supplied value when the condition is false | (no bytes are consumed when the condition is false, so a violation surfaces as a failure of the following field) |
| `required` | `tlv32` only. `true` means the field must be present | never emits without a required field | absence is a failure |
| `id` | `tlv32` only. The field ID | — | — |
| `constraints` | a constraint on one field. Currently `contains-protocol-required-capability` (a text vector must contain `protocol.requiredCapability`) | rejects violating input | a violation is a failure |
| `description` | a descriptive string stating what the value means. No effect on layout or validation | — | — |

That `when` references only earlier fields and flags is a property of the dialect — one forward
decode pass decides every condition, and no second pass or backtracking is needed.

## 6. `commands[]` and `flags[]`

### 6.1 Command Entries

| Key | Meaning |
|---|---|
| `id` | a non-zero u8 value. Unique and outside `reservedCommandRanges` |
| `name` | a unique name, used by the generated constant tables |
| `domain` | `application` or `infrastructure`. The dispatch difference between the two is owned by [06 §3](06-wire-protocol.en.md#3-command-space) |
| `allowedFlags`, `requiredFlags` | the flags this command may set and must set. `requiredFlags ⊆ allowedFlags` |
| `flagConstraints` | relations between flags — `all-or-none` (the listed flags are all set or all clear), `implies` |
| `body` | the field list of the bytes right after the head prefix ([§5](#5-field-keywords)) |
| `payload` | `forbidden`·`optional`·`required`. Whether a typed payload envelope frame follows |
| `payloadType` | the layout (`$ref`) of that frame when `payload` is not `forbidden` |
| `semanticConstraints` | the names of semantic rules attached to this command ([§8](#8-semantic-declarations)) |

**Direction is not in the schema.** Which side may send which command, and on which topology it is
allowed, is a runtime rule owned by [06 §3](06-wire-protocol.en.md#3-command-space),
[§4](06-wire-protocol.en.md#4-admission-and-connection-fence), and
`relocationStateMachine.commandRules`. Reading the bytes of a command and deciding whether this
connection may accept the command it read are different layers.

**Reply relations are not in the entry either.** Which reply answers which request is declared by
the correlation fields of the body (`correlationFields` in
[§4.8](#48-versioned-length-delimited)) and by `relocationStateMachine`. There is no generic key
such as `replyTo`.

### 6.2 Flag Declarations

| Key | Meaning |
|---|---|
| `name` | the flag name |
| `bit` | the bit within the flags byte. A power of two, unique |
| `frame` | the layout (`$ref`) of the frame that follows when this flag is set |
| `whenSet`, `whenClear` | `frame-required`·`frame-forbidden`. The frame must be present when set and absent when clear |
| `controls` | a literal stating that the flag switches part of the command body (a tail, an extension) rather than a frame. Body fields make it their `when` condition |

An undeclared bit, a bit outside `allowedFlags`, a missing `requiredFlags` bit, a
`flagConstraints` violation, and a frame count that differs from `whenSet`·`whenClear` are all
failures under [§3](#3-rules-common-to-every-layout). How flags determine the frame count of a record
is owned by [06 §2](06-wire-protocol.en.md#2-record-framing-and-decode).

## 7. Durable Formats and the Logical Stream

### 7.1 `durableFormats[]`

Envelopes that persist in a store beyond a process lifetime. Each entry declares the following.

| Key | Meaning |
|---|---|
| `name` | the format name |
| `magic`, `formatVersion` | the magic bytes and version at the front of the envelope |
| `flags`, `flagsType` | the flags value and its integer layout |
| `byteOrder` | big-endian |
| `bodyLengthType`, `body`, `maximumEncodedBytes` | the body length prefix, the body layout (`$ref`), and the bound |
| `checksum` | `algorithm` (`crc32c-castagnoli`), `encoding`, `coverage` (for example `magic-through-body`), `position` (`trailing`) |
| `goldenFixture` | the path of the fixture that pins this format's accepted and rejected bytes |
| `providerInterpretation` | `opaque-bytes`. The store provider does not interpret these bytes |

The layout is magic, formatVersion, flags, body length, body, checksum, and the checksum covers the
range given by `coverage`. The decoder verifies magic, version, length, and checksum — **all of
them** — before reading the body. Any single mismatch is a failure, and there is no partial
recovery. What each format means inside the store (when it is written and when it is deleted) is
owned by [06 §7](06-wire-protocol.en.md#7-durable-authority-and-explicit-creation) and
[§9](06-wire-protocol.en.md#9-maintenance-capture-and-relocation-envelope).

### 7.2 `relocationLogicalStreamFormat`

The declaration of the relocation payload that travels directly from source memory to the target.
`body` (`$ref`) is the layout of the whole logical stream, and `generatedObjectTree` points with
`$ref` at the sub-trees the runtime handles separately (application state, saved work, timers).
`maximumBytes` is the logical bound and `goldenFixture` pins the bytes. `chunkSplit` states that a
chunk may end at any byte boundary, including inside a frozen record. `replay` is the generated
decoder's incremental contract, and the five fields of the `replay` object define these values: the
decoder is fed the chunks in order and decodes without allocating an input buffer for the complete
encoded stream (`wholeStreamInputAllocation`), returns success only on the final chunk
(`completion`), and ends with the failure kind named by `incompleteFinalChunk` when the root is
unfinished at the final chunk or by `bytesAfterRoot` when bytes remain after the root. The chunk
commands, the checksum convention, and the procedure by which the target feeds chunks are owned by
[06 §9](06-wire-protocol.en.md#9-maintenance-capture-and-relocation-envelope).

## 8. Semantic Declarations

### 8.1 `semanticContexts`

Declarations of values that come from outside the layout. Each entry has `name`, the value's type
(`valueType` with `$ref`, or `valueKind: boolean`), and `source` (a literal stating where the value
is obtained). A `conditional-union` discriminator with `source: context` references this name. It is
the mechanism for selecting a case by a value that is not in the bytes but that the decoder already
knows (for example, the kind of the original operation). The decoder must hold the context value
**before reading the bytes**; without it, the union cannot be read.

### 8.2 `semanticConstraints`

A list of objects, one per kind. `implies` is a generic relation with `contextEquals` under
`if`/`then`; every other kind pins one bundle of rules (authority state combinations, ownership of
saved work, terminal ownership of a relay, admission fences, …) as tuples of field names and
literal values.

The validator compares each kind's object **literally** against the expected value in its own code.
The list is therefore not documentation but build-time checked data — when a rule changes, the
validator's expectation and the schema change together, and a schema changed on one side alone does
not pass. By contrast, only one tuple, `terminal-failure-integrity` (the allowed combinations of
terminal result, failure code, and payload), is lowered into a runtime decoder check. Its `fields` is
the only list of the wire owners that receive the check: every owner that declares both
`terminalResult` and `failureCode` (a type's `fields` or `body`, a command's `body`, or one named
conditional-union case; a `layout` and an `otherwise` fallback may not carry the pair) — the
validator checks that the two sets are equal, and the lowering attaches the check only to the owners
in this list. For the other
kinds, the owning clause in [§9](#9-profiles-and-owning-clauses) states the rule, the runtime
implements it from that clause, and the schema preserves it in a form a machine can compare.

### 8.3 `relocationStateMachine`

Declares `phaseType` (the `$ref` of the phase enum), `authorityCommitOrder` (per phase, what must
have finished before it and whether a relocation record exists), `transitions` (allowed transitions),
and `commandRules` (per command, the phases and roles that may send it). The meaning of the
transitions is owned by [06 §10](06-wire-protocol.en.md#10-relocation-actor-membership-and-ready)
and [Location runtime](../05-location-relocation/01-location-runtime.en.md); this declaration pins
those rules so the validator can check them against the command declarations.

## 9. Profiles and Owning Clauses

Profile keys pin, as machine-readable literals, rules that other clauses own in prose. To learn what
a value means, read the owning clause; when the owning clause changes, the profile changes with it
([§1](#1-dialect-and-consumers)).

| Profile key | Owning clause |
|---|---|
| `frameworkJsonV1Profile` | [Message model §5](../00-foundation/05-message-model.en.md#5-the-framework-json-v1-typed-payload-profile) |
| `frameworkMultipartV1Profile` | [06 §2 "Framework multipart application profile"](06-wire-protocol.en.md#2-record-framing-and-decode) |
| `authorityKeyFormat` | [06 §1 "Location Store Authority Key Format"](06-wire-protocol.en.md#1-schema-and-generation-boundary) |
| `authorityStoreAccessProfile` | [Location runtime §6.1](../05-location-relocation/01-location-runtime.en.md#61-read-and-cas) |
| `descriptorEnumerationProfile` | [Location runtime §4](../05-location-relocation/01-location-runtime.en.md#4-finding-running-nodes-and-their-capabilities) · [§6.2](../05-location-relocation/01-location-runtime.en.md#62-reading-multiple-pages-as-a-list-from-the-same-point-in-time) |
| `authorityStoreGenerationProfile` | [Location runtime §3.1–§3.3](../05-location-relocation/01-location-runtime.en.md#3-values-distinguishing-re-creation-of-the-same-id-from-an-owner-change) |
| `authorityStoreDurabilityProfile` | [Location runtime §3.3](../05-location-relocation/01-location-runtime.en.md#33-values-stored-in-the-current-location-record) · [§11](../05-location-relocation/01-location-runtime.en.md#11-cleaning-up-store-records-when-a-host-shuts-down) |
| `ownerLeaseAuthorityProfile` | [Location runtime §4.1](../05-location-relocation/01-location-runtime.en.md#41-validating-a-target-descriptors-owner-lease) · [§5](../05-location-relocation/01-location-runtime.en.md#5-blocking-a-previous-owners-new-work-when-the-store-connection-drops) |
| `relocationRetentionPolicy` | [Redis Relocation Store §4](../05-location-relocation/03-relocation-store-redis.en.md#4-result-per-operation) · [§5](../05-location-relocation/03-relocation-store-redis.en.md#5-cancellation-errors-and-result-reconstruction) · [§6](../05-location-relocation/03-relocation-store-redis.en.md#6-payload-publication-and-cleanup) |
| `relocationStorageProfile` | [Redis Relocation Store §2](../05-location-relocation/03-relocation-store-redis.en.md#2-public-spi-and-responsibility-boundary)–[§6](../05-location-relocation/03-relocation-store-redis.en.md#6-payload-publication-and-cleanup) · [06 §9](06-wire-protocol.en.md#9-maintenance-capture-and-relocation-envelope) |
| `relocationTransferChecksumProfile` | [06 §9 "CRC-32C convention and capability"](06-wire-protocol.en.md#9-maintenance-capture-and-relocation-envelope) |
| `maintenanceAdmissionProfile` | [Host relocation flow §4](../05-location-relocation/05-host-relocation-flow.en.md#4-conditions-checked-before-selecting-a-target) · [§5](../05-location-relocation/05-host-relocation-flow.en.md#5-selecting-a-target-matching-the-mode) |
| `terminationResultProfile` | [06 §11](06-wire-protocol.en.md#11-request-terminal-identity) |
| `livenessProfile` | [06 §5](06-wire-protocol.en.md#5-service-liveness) · [Transport liveness §3](05-transport-liveness.en.md#3-routemesh-and-clientserver) |
| `fanoutLivenessProfile` | [06 §5](06-wire-protocol.en.md#5-service-liveness) · [Transport liveness §4](05-transport-liveness.en.md#4-classic-fanout) |

`durableFormats`, `relocationLogicalStreamFormat`, and `relocationStateMachine` are layout and
semantic declarations rather than profiles; their owning clauses are given in
[§7](#7-durable-formats-and-the-logical-stream) and [§8](#8-semantic-declarations).

## 10. Verification Requirements

A dialect change (a new top-level key, a new `kind`, a new keyword, or a change to a keyword's
evaluation rule) provides all of the following.

- The validator interprets the key, kind, or keyword, and `--self-test` actually rejects a mutation
  that breaks the rule ([06 §1 "Validator"](06-wire-protocol.en.md#1-schema-and-generation-boundary)).
- The corresponding section of this chapter is updated in the same change. A key, kind, or keyword
  present in the schema but absent from this chapter is a defect of this chapter.
- A new layout kind pins both accepted and rejected bytes in a golden fixture
  ([06 §12](06-wire-protocol.en.md#12-verification-requirements)).

A value change (a new command, field, bound, or enum) is not a dialect change and follows only the
requirements of [06 §12](06-wire-protocol.en.md#12-verification-requirements).

---

[Channel·Transport topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 06. Service Wire Protocol](06-wire-protocol.en.md)
