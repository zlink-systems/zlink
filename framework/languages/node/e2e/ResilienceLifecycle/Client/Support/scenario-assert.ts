export function ensure(condition: boolean, message: string): asserts condition {
  if (!condition) throw new Error(message);
}

export function countNewEvidence(after: readonly string[], before: readonly string[], prefix: string, marker: string): number {
  const beforeCount = before.filter((line) => line.includes(prefix) && line.includes(marker)).length;
  const afterCount = after.filter((line) => line.includes(prefix) && line.includes(marker)).length;
  return afterCount - beforeCount;
}

export function uniqueMarker(prefix: string): string {
  return `${prefix}-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}
