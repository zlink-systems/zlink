/** Location Store §6: caller validation is terminal; provider failures may lose a result. */
export function isIndeterminateLocationStoreFailure(error: unknown, signal?: AbortSignal): boolean {
  return !(error instanceof TypeError || error instanceof RangeError) && signal?.aborted !== true;
}
