[한국어](EXAMPLES.ko.md)

[Guide index](README.en.md) · [Style conventions](STYLE.en.md)

# Example Code Maintenance Convention — Preventing Drift

A guide's code examples tend to drift from the real API over time. This
document defines the convention that keeps an example **tied to a
compiled-and-run sample so it doesn't rot**.

> Principle: guide code isn't maintained by hand alone. Tie it to something
> that breaks the build when it breaks, so "the documentation goes quietly
> wrong" never happens.

## 1. Single Source Of Truth — `bindings/<lang>/samples/`

Each language already has runnable samples.

- `bindings/dotnet/samples/` · `bindings/cpp/samples/` ·
  `bindings/java/samples/` · `bindings/node/samples/`, and so on.
- Each binding has a sample runner (`run_samples.sh`, and so on), and some
  are verified by CI smoke tests.

A guide example treats these samples as its **single source of truth**.

## 2. Two Levels (Applied In Realistic Stages)

### Level A — 1:1 Correspondence (Current Baseline)

Each major example in a guide is put in **1:1 correspondence** with a
sample that does the same thing, and the guide names the sample file
explicitly.

- Example: the binding guide's `02-messaging` PAIR example ↔
  `samples/PairRecv`.
- Because the sample is verified by CI smoke, a build break in the sample
  signals when the API changes.
- The guide code and the sample **can differ in presentation, but must call
  the same public API**.

### Level B — Named Snippet Extraction (Target)

A named snippet region is placed inside the sample file, and the guide
extracts and embeds that region.

```csharp
// #region guide:pair-send
client.Send().Message(Message.From("PING")).Submit();
// #endregion
```

- At build time, a `#region guide:<name>` block is injected into the
  guide's matching code block.
- This means the guide code is **auto-generated from the sample**, making
  drift structurally impossible.
- The current documentation build uses the `--8<--` snippet-path directive
  (a quoted `path:section` argument); automatic `#region guide` extraction needs an extraction
  script and a CI step, so it's left as follow-up work.

## 3. Verification (Regression)

- Verify by building and running through the sample runner — if a sample
  the guide references breaks, it's caught (some through CI smoke).
- (Recommended) add a link check to the documentation regression tests
  that verifies a sample file a guide code block references actually
  exists.
- No invented APIs: a method written in the guide must actually exist in
  that binding's `Contracts/`/source
  ([Style conventions §7](STYLE.en.md)).

## 4. Author Checklist

When adding or editing a code example in a guide:

- [ ] Does the called API actually exist in that language's public contract (not invented)?
- [ ] Is there a matching sample (`samples/...`)? If not, was one added?
- [ ] Are the values realistic (production-like ports, symbols, amounts, and so on)?
- [ ] Is it consistent with the same scenario in the core guide ([Shared scenarios](scenarios.en.md))?

---

> See also: [Style conventions](STYLE.en.md) · [Shared scenarios](scenarios.en.md).
