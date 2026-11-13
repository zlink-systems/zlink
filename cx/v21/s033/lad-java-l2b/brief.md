Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4352 Java ladder L2 correction — use the production header encode path. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-java, branch exp/4350-java-ladder, HEAD 5d74ddac1e (L2). Commit locally on top; never push, never open a PR.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (applies here) and /root/jobs/fx/cx/v21/s033/lad-java-l2/report.md.

## Why
L2 request-backpressure 1K lost 9.28% vs L1 (127,665 / 139,752 = 0.914). The JFR cost sits in `ZLinkChannelEnvelope.encodeHeader(Header)` called from FrameworkStack.java:141-151, which passes no PreparedNames, so channel/message/content-type names are encoded on every request. The production request path does not do that: ZLinkChannelCallRuntime.java:480-500 calls `ZLinkChannelEnvelope.encode(create(...), ..., channelNames, names)` with PreparedNames prepared once. L2 must restore the envelope header the way the Framework actually sends it, not a slower path.

## Do
1. Change the L2 client (FrameworkStack.java:141-151 and the RawStack L2 operation) to prepare `ZLinkChannelEnvelope.PreparedNames` once per channel/message/content type and encode each request header through the same overload and arguments the production call path uses (ZLinkChannelCallRuntime.java:480-500; trace which overload/encodeHeaderBytes(header, names) it ends in, file:line). The server side: check whether the production receive path parses the header with anything cheaper than the `decodeHeader` call at ZLinkFrameworkBenchServer.java:201; if it does, use the same call; if not, keep it and say so with file:line. No product runtime code changes. Commit.
2. Fresh build (build && verify JAR newer than sources && measure). Measure L2 vs binding in one lock session: request-backpressure 1K 5 runs, send-saturation 1K 5 runs (L2 G5 failed at 11.68% last time), request-backpressure 4K 3 runs. Progress line before each measurement as common-rules says.
3. Step loss = 1 − (L2÷binding)/(L1÷binding), L1 ratios from lad-java-l1/report.md. Pass ≤ 7%.
4. If request-backpressure 1K still loses more than 7%: profile it again (paired JFR, same method as lad-java-l2/raw/profile/reqbp-1K) and write the remaining cost with file:line and a proposal inside ZLinkChannelEnvelope (e.g. the two-pass size+write in encodeHeaderBytesUncached:568-579). Do not implement product changes.

## Output
/root/jobs/fx/cx/v21/s033/lad-java-l2b/report.md (Korean, short tables, every number cites its raw file): what changed (file:line, commit), the production call path it now matches, the 3-cell table (medians, L2÷binding, L1÷binding, step loss, G5), and the remaining-cost proposal if any. Time cap 3 h.
