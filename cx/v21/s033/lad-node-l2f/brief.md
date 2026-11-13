Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: Node ladder L2 reply envelope — review fixes. IMPLEMENTATION/TESTS ONLY, NO MEASUREMENT. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-node, branch exp/4350-node-ladder, tip 420196e7a1. L2 reply commit 696dab596a (lad-node-l2r/report.md). Commit locally only; never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh. Do NOT take /root/jobs/fx/cx/measure.lock, run no benchmark or load. Never delete raw files. Read /root/jobs/fx/cx/v21/s033/common-rules.md and ladder-steps.md first.

## Review findings on 696dab596a (shared/raw-wire.js encodeProductionProtobufChannelReply, zlink-framework-server/main.js)
Production reply (packages/framework/src/runtime/channels/channel-dispatchers.ts:409-412) is `record.reply(encodeChannelReplyParts(header, reply, this.codecs))`: a codec registry built once per dispatcher, and the encoded parts handed to reply() as they are. The bench adds per-reply work production does not do:
1. A new codecs Map and serializer object on every reply. Build the reply codec registry once at server start (as production registers codecs once) and reuse it.
2. Extra body copies: serialize() encodes into a zlink Message then copies it with Buffer.from(encoded.data()); and the returned parts[1] is copied again with zlink.Message.from(Buffer.from(parts[1])). Trace what production's protobuf/application codec returns and what record.reply accepts (MessageLike), and make the bench pass the parts the same way, with no copy production does not make. If one conversion is unavoidable because the bench sits on the raw socket instead of the mesh record, keep exactly one and say why with file:line.
3. Check that the receive side parses the request header only once (decodeProductionChannelHeader) and that the source completion parses the reply header once, matching channel-outbound-operations.ts:244-248,519-523.
Apply as a fixup into 696dab596a and replay L3/L4/L5~L7 on top (non-interactive; GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash is fine). Fix conflicts so each later step keeps its own feature. bash build.sh and npm test at L2 and at the tip; dry start/stop of the server with no load.

## Output
/root/jobs/fx/cx/v21/s033/lad-node-l2f/report.md (Korean, short): production file:line vs bench file:line for reply encode and completion parse, copies per reply before/after, old→new commit map, build/test evidence. Time cap 2 h.
