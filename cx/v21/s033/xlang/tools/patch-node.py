from pathlib import Path
p=Path('/root/worktree/zlink-xlang/framework/bench/grpc/node/client/bench-core.js')
s=p.read_text()
anchor='async function requestWorkers('
helper='''// Measurement-only open-loop pacing; the aggregate rate covers all submit workers.
const xlangRate = Number(process.env.XLANG_RATE || 0);
let xlangNext = 0n;
async function xlangPace(pump) {
  if (!xlangRate) return;
  const now = header.nowNs();
  if (xlangNext === 0n) xlangNext = now;
  const ticket = xlangNext;
  xlangNext += BigInt(Math.floor(1e9 / xlangRate));
  while (header.nowNs() < ticket) {
    pump?.poll(0);
    const leftMs = Number(ticket - header.nowNs()) / 1e6;
    if (leftMs >= 1) await delay(Math.floor(leftMs));
    else await sleepImmediate();
  }
}

'''
assert helper not in s
s=s.replace(anchor,helper+anchor)
s=s.replace('while (header.nowNs() < deadline) {','while (header.nowNs() < deadline) {\n        if (xlangRate) await xlangPace(transport.requestCompletionPump);')
p.write_text(s)
