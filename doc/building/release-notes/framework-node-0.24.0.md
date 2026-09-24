[한국어](./framework-node-0.24.0.ko.md) | [English](./framework-node-0.24.0.md)

# ZLink Node.js Framework 0.24.0 Release Notes

Framework 0.24.0 uses binding 1.6.0 and Core 1.6.0. Each Framework language release is versioned independently.

## Contract Changes

- Fixed RIDs can be used with both manual topology and automatic discovery, regardless of Object role. Setting a fixed RID together with an automatic RID prefix causes a startup configuration error. A restart fails immediately with a conflict while the previous owner lease is active; the same RID can be used after that lease expires. (#1045, #1056)

## Common Changes

- Core 1.5.0 and later fix failures to load `libzlink` with a late `dlopen()` in hosts with limited static TLS space. The Framework uses this Core version through binding 1.6.0. (#1041)
- Core 1.6.0 fixes ROUTER refusing to issue a reply token when the sender disconnects right after a request is received, which ended the receive path with an error (the rare cause of a C++ process abort). (#1051, #1062)
- Quickstart and tutorial listeners are limited to `127.0.0.1`. The tutorial explains how to remove its keys when reusing Redis and how to restart after a forced stop by waiting for the previous owner lease to expire or removing the relevant keys. (#1035, #1043, #1045, #1047, #1049)
- The engine server example is organized as the `EngineLobby.sln`, `Server`, and `Shared` projects. (#1036, #1039)

## Fixes

- The Godot C# and C++ examples now place their Windows DLLs, use the corrected C++ build settings, and document execution so they communicate with the engine server in Godot 4.4.1. (#1037, #1040)
- The Unity (Windows native and WebGL) and Cocos Creator examples' manifest, scene, received-value logging, and README were fixed so that they communicate with the engine server in Unity 6000.0.83f1 and Cocos Creator 3.8.8. (#1037, #1064, #1066)
- Corrected the earlier Framework version in the Node tutorial's package table and the HttpClient tutorial's expected lookup result to the actual response, `fetch speedy-p2`. (#1035, #1043, #1013)

## Node.js Changes

- The in-memory and Redis Location Stores now report a new RID claim that conflicts with a valid previous owner as `rejectedConflict` instead of `ignoredStale`. (#1045, #1047)
- Fixed a route from the same generation being reactivated after an Instance Spot closes, which caused relocation to end with `RelocationFailed`. (#1050, #1053)
- `@zlink-systems/stream-connector` uses `Array.from` instead of `Set`/`Map` spread, fixing receive handlers that did not run under bundlers with loose transforms (Cocos Creator). The Unity WebGL UPM package includes the assets' `.meta` files. (#1037, #1064, #1066)

## Install

```bash
npm install @zlink-systems/framework@0.24.0 @zlink-systems/http-client@0.24.0
```

The release tag is [`framework-node/v0.24.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.24.0).
