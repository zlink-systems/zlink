# zlink Python Binding API Reference

This reference is generated from the Python source in `bindings/python/src/zlink/`.

## Prerequisites

```bash
pip install sphinx
```

## Generate

```bash
cd bindings/python
sphinx-build -b html docs docs/_build/html
```

Generated HTML entrypoint:

```text
bindings/python/docs/_build/html/index.html
```

## Scope

- All public symbols exported by `zlink.__init__`
- Socket types, domain objects, enums (including `CompletionKind`), and service
  wrappers
- Internal modules (`_ffi`, `_native`) are not exported and will not appear
