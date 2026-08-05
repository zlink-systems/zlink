# ZLink Framework Node.js Public Contract

This directory owns the **formal public contract** the Node.js
framework must provide. Package exports, public declarations, and
contract tests must follow this contract.

| No. | Document | Scope |
|---|------|------|
| `01` | [System Structure](01-system-structure.en.md) | Package structure/deployment, NestJS module registration, DI, lifecycle including Instance Spot, and startup validation |
| `interfaces` | [Public Interface Table Of Contents](interfaces/README.ko.md) | Per-category TypeScript declarations, Location Store, maintenance, and automatic routing ID allocation |

**The meaning and behavioral rules of a feature are owned by the
[common spec](../../../README.en.md).** This directory only fixes the
**exact public API** that meaning takes in this language.

Host relocation must always specify a mode. Planned maintenance only
uses the same application version as source, and rolling update only
uses a target that exactly matches the caller-specified higher
application version. Host termination is a separate `shutdown()`
operation.

## Cancellation Argument

The Node.js public interface **doesn't automatically add a
cancellation argument to a regular handler.** A long-running operation
that the caller must be able to interrupt, such as waiting on a request,
connecting, or terminating, uses an optional `AbortSignal` following
Node convention. The exact interface signature fixes what it applies
to.
