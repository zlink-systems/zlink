import { Game, Names } from '../shared/constants';
import type { QueueRequest } from '../shared/messages';

export class MatchmakingState {
  waiting: [string, QueueRequest[]][] = [];
  roomSequence = 0;
  enqueue(request: QueueRequest): void {
    if (!request.mode.trim()) throw new Error('Mode required');
    if (this.waiting.some(([, queue]) => queue.some((p) => p.playerId === request.playerId)))
      return;
    let entry = this.waiting.find(([mode]) => mode === request.mode);
    if (!entry) {
      entry = [request.mode, []];
      this.waiting.push(entry);
    }
    entry[1].push(request);
  }
  pair(): QueueRequest[] {
    return (
      this.waiting
        .find(([, queue]) => queue.length >= Game.playersPerRoom)?.[1]
        .slice(0, Game.playersPerRoom) ?? []
    );
  }
  allocateRoom(): string {
    if (!Number.isSafeInteger(this.roomSequence + 1)) throw new Error('Room sequence exhausted');
    return Names.roomPrefix + ++this.roomSequence;
  }
  remove(pair: QueueRequest[]): void {
    const index = this.waiting.findIndex(([mode]) => mode === pair[0].mode);
    const queue = this.waiting[index][1];
    queue.splice(0, Game.playersPerRoom);
    if (!queue.length) this.waiting.splice(index, 1);
  }
}
