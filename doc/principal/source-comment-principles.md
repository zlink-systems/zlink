# zlink Source Comment Principles

> This document defines the common policy for public API comments and internal
> source comments in zlink. The syntax differs by language, but the contract
> information exposed by comments must stay consistent.

---

## Core Rule

Public API comments are contract text. They describe only the public behavior a
caller needs in order to use the API correctly. They must not turn private
implementation details or current optimizations into public guarantees.

Comments should not repeat code in prose. If a name, parameter, or type already
states the obvious, do not restate it. Use comments for contracts that callers
can easily get wrong: lifetime, ownership, errors, waits, cancellation, and
callbacks.

## Public API Comments

Publicly visible types, functions, methods, properties, and fields use the
standard API reference comment form for the language.

| Language | Public API comment form |
|----------|-------------------------|
| C/C++ | Header comments or Doxygen-style comments |
| C#/.NET | XML documentation comments |
| Java | Javadoc |
| Node/TypeScript | TSDoc |
| Python | Docstrings |
| Go | godoc comments |
| Rust | rustdoc comments |

Public API comments should prioritize these contract details:

- Ownership, disposal, or transfer of payloads, buffers, handles, and messages.
- Borrowed views versus copied buffers.
- Blocking, non-blocking, timeout, and cancellation behavior.
- Callback registration, removal, invocation timing, and reentrancy.
- Meaning of boolean return values, result enums, error codes, and exceptions.
- Staged operation-builder flow where the next valid call is part of the
  contract.
- Failures or exceptions callers can avoid by following the contract.

## Comment Shape

- Write public API reference comments in English so generated API references and
  external users read the same text.
- Keep summaries short and caller-focused.
- Use language-specific detail sections such as remarks, details, or notes only
  for contract details that do not fit in one summary.
- Prefer precise ownership words: owns, borrows, copies, transfers, disposes.
- Do not repeat the method name in prose unless it clarifies overload behavior.
- Do not describe current optimizations, temporary bypasses, or private data
  structures as public guarantees.

## Internal Comments

Internal comments are for decisions that are hard to infer from code alone.

- Explain complex concurrency, lifetime, error masking, or protocol-boundary
  decisions that are easy to break accidentally.
- Do not narrate what each line of code does.
- Keep implementation explanations in runtime comments or `doc/internals/`, not
  in public API comments.
- Temporary comments must include the removal condition or related issue. Do not
  leave comments that only say `TODO: fix later`.

## Separation From Guides

API reference comments are not tutorials. They should explain the exact contract
of one type or member.

Usage patterns, examples, motivation, and design background belong in
`doc/guide/` or the relevant language guide. If a public member needs long
background explanation, keep the source comment short and explain the context in
a guide.

## Review Checklist

When a public API or public contract changes, review the source comments with
the code:

- Can a caller tell who owns returned resources, messages, and buffers?
- Is the borrowed-view versus copied-buffer boundary clear?
- Are timeout, cancellation, callback, and blocking behaviors described?
- Are false, result enum, error code, and exception meanings described?
- Does the comment avoid presenting runtime internals as public contract?
- Did long usage guidance move to a guide?
- Does the language-specific API documentation build or warning gate pass?
