import { ConnectError, ConnectResult } from '@zlink-systems/zlink';

export function isBindingNotFound(error: unknown): boolean {
  return error instanceof ConnectError && error.result === ConnectResult.NotFound;
}
