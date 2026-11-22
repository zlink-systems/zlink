// Readable progress lines for the console. E2e IDs end in a run UUID; the first eight hex digits
// are enough to tell runs apart on screen, and the file logs keep the full IDs.
const runId = /([0-9a-f]{8})-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/g;

export function shortId(id: string): string {
  return id.replace(runId, '$1');
}

// "playA" reads as "Play node A".
export function serverTitle(nodeId: string): string {
  const play = /^play([A-Z])$/.exec(nodeId);
  return play ? `Play node ${play[1]}` : nodeId;
}

export function tableLog(tableId: string, message: string): void {
  console.log(`${shortId(tableId)}: ${message}`);
}
