export function ensure(condition: boolean, message: string): asserts condition {
  if (!condition) throw new Error(message);
}

export function containsRequestMarkersInOrder(
  evidence: readonly string[],
  requestId: string,
  markers: readonly string[],
  message: string
): void {
  let cursor = -1;
  for (const marker of markers) {
    const index = evidence.findIndex((line, lineIndex) =>
      lineIndex > cursor && line.includes(`request=${requestId}`) && line.includes(marker));
    ensure(index >= 0, `${message} Missing marker '${marker}'.`);
    cursor = index;
  }
}
