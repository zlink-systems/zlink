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
- Raw socket types, message and event types, options, result enums, and errors
- Public socket and message methods inherited from shared protocols
