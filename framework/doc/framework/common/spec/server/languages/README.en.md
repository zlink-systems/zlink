# Framework Public Contract Per Language

This directory defines the exact form in which the Framework server
package's common behavior is provided in each language's public API. The
signatures recorded here are the formal contract the corresponding
language's implementation and contract tests must follow.

The client package's public interface isn't defined in this directory.
The Stream connector is owned by
[Per-Language Stream Connector Contract](../../stream-connector/README.en.md),
and the HTTP client is owned by
[Per-Language HTTP Client Contract](../../http-client/README.en.md).

Behavior common across languages is defined by the
[common spec](../../README.en.md), and the procedure for changing a
contract follows
[Public Contract Governance](../../00-public-contract-governance.en.md).

| Language | Public contract |
|------|-----------|
| `.NET` | [dotnet](dotnet/README.ko.md) |
| Java | [java](java/README.ko.md) |
| Kotlin | [kotlin](kotlin/README.ko.md) |
| Node.js framework | [node](node/README.ko.md) |
| C++ | [cpp](cpp/README.ko.md) |

The per-language specs aren't documents that copy each other's
signatures. They each fix the same common behavior as a public contract
that users of that language can use naturally.
