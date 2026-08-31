# Diagram tooling

Scripts that turn [archify](https://github.com/tt-a1i/archify) architecture IRs into the
clean, uniform, theme-aware SVG diagrams embedded in the guide docs. The authoring rules
and full workflow live in
[`doc/principal/documentation/diagram-authoring-guide.ko.md`](../../doc/principal/documentation/diagram-authoring-guide.ko.md).

## Prerequisites

- **archify** checked out locally. Point `ARCHIFY_DIR` at the dir containing `bin/archify.mjs`.
- **playwright** with chromium (`npm i playwright && npx playwright install chromium`), used for
  content-fit cropping and light/dark screenshot verification.

## Scripts

| Script | Role |
| --- | --- |
| `build-diagram.mjs` | `render` an IR, insert ×N "stacked cards", strip viewer chrome (toolbar / PATH·MAP·LENS), crop the viewBox to content. Output path must be **absolute**. |
| `pad-viewbox.mjs` | Normalize every diagram's viewBox width to a common value (content centered) so all render at the same scale. Run after all diagrams are built. |
| `shot.mjs` / `shot-dark.mjs` | Screenshot a file/URL in light / dark for eyeball verification (validator passing ≠ no crossings). |
| `shot-region.mjs` | Screenshot scrolled to a heading, for in-page checks. |

## Typical run

```sh
export ARCHIFY_DIR=/path/to/archify/archify
D=framework/doc/framework/common/diagrams
# 1) build each diagram (absolute out path)
node scripts/diagrams/build-diagram.mjs architecture "$PWD/$D/01-delivery-existing.architecture.json" "$PWD/$D/01-delivery-existing.html"
# 2) once all are built, unify widths (common width >= widest diagram)
node scripts/diagrams/pad-viewbox.mjs 1010 "$PWD/$D"/01-*.html
# 3) verify light + dark
node scripts/diagrams/shot.mjs "file://$PWD/$D/01-delivery-existing.html" /tmp/l.png 1010 640
node scripts/diagrams/shot-dark.mjs "file://$PWD/$D/01-delivery-existing.html" /tmp/d.png 1010 640
```
