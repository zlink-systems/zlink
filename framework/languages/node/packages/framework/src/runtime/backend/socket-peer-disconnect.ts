import { isBindingNotFound } from './binding-error-results';

export function disconnectSocketPeer(
  socket: { disconnectRid(routingId: unknown): void },
  routingId: unknown
): void {
  try {
    socket.disconnectRid(routingId);
  } catch (error) {
    // An absent RID has already completed the physical close. The existing
    // logical lifecycle still owns local cleanup and unbind.
    if (!isBindingNotFound(error)) throw error;
  }
}
