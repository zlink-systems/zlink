# Node ladder L2 reply 복원

- Production reply encode: `channel-dispatchers.ts:409-412` → `channel-envelope.ts:217-237`; completion header parse: `channel-outbound-operations.ts:244-248,519-523` → `decodeChannelReply()` `channel-envelope.ts:321-330` → `decodeChannelHeader()` `:889-897`.
- Bench target reply: `zlink-framework-server/main.js:197-212`, codec adapter `shared/raw-wire.js:103-119`; source parse once: L2 commit `client/main.js:259-271`, parser adapter `shared/raw-wire.js:99-100`.
- L2 commit: `696dab596a`.
- Replayed commits: L3 `e97dd9a5e4 → d7f9a45a25`; L4 `33b6c9b030 → 11442781b8`; L5~L7 `3c50a02908 → 420196e7a1`.
- Evidence: `bash build.sh` 성공; `npm test` 22/22 통과. benchmark·measurement 미실행.
