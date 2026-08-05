# zlink vcpkg overlay port

This directory provides an overlay port for `zlink`.

## Usage

```bash
vcpkg install zlink --overlay-ports=./vcpkg/ports
```

## Release update steps

1. Create core release tag: `core/vX.Y.Z`
2. Update `vcpkg/ports/zlink/vcpkg.json` `version-string`
3. Ensure `vcpkg/ports/zlink/portfile.cmake` can resolve `REF core/v${VERSION}`
4. Validate locally:

```bash
vcpkg install zlink --overlay-ports=./vcpkg/ports
```
