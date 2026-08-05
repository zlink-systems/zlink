# .NET API Reference Comments

This document defines how XML documentation comments are used for the .NET
binding public API reference. The language-independent policy is defined in
[`source-comment-principles.md`](../../../../doc/principal/source-comment-principles.md).

XML comments in `bindings/dotnet/src/Zlink/Contracts/` are contract text. Every
publicly visible type and member in that folder must have XML documentation.
The comments must describe the public behavior a caller needs to use the API
correctly, and they must stay aligned with `core/include/zlink.h` and the
binding contract documents.

## .NET Scope

Public contract members under `bindings/dotnet/src/Zlink/Contracts/` require
XML comments. Runtime implementation details belong in runtime comments or
`doc/internals/`, not in public XML comments.

The main `Zlink.csproj` must not suppress `CS1591`. A clean rebuild with XML
documentation warnings enabled is the completeness gate for the contract
assembly. Codec projects may keep their own policy because they are separate
packages.

## .NET Comment Shape

- Write XML comments in English.
- Keep summaries short and caller-focused.
- For simple enum values and DTO fields, a concise one-line summary is enough.
- Use `<remarks>` only for contract details that do not fit in one summary.
- Document timeout, cancellation, callback, ownership, disposal, and exception
  contracts with `<remarks>` or `<exception>` according to the common source
  comment policy.

## Separation From Guides

API reference comments are not tutorials. They should explain the exact contract
of one type or member. Usage patterns, examples, and motivation belong in
`doc/guide/`.

If a public member needs long background explanation, keep the XML comment short
and link from the guide or binding README to the appropriate guide page.

## Review Checklist

When a public contract changes, review the XML comments with the code:

- Does `dotnet build bindings/dotnet/src/Zlink/Zlink.csproj --no-restore -t:Rebuild`
  report zero XML documentation warnings?
- Does the comment satisfy the common public API source comment checklist?
- Does the text match the generated API reference target?
