import { randomUUID } from 'node:crypto';
import { Names } from '../shared/constants';
import type { MatchRequest, MatchResponse } from '../shared/lobby-messages';

export class InvalidMatchRequest extends TypeError {}

interface Waiting {
  playerId: string;
  resolve(value: MatchResponse): void;
  reject(error: unknown): void;
}

export class Matchmaker {
  private readonly queues = new Map<string, Waiting>();
  private readonly participants = new Set<string>();

  constructor(
    private readonly prepare: (roomId: string, members: string[]) => Promise<MatchResponse>
  ) {}

  async match(request: MatchRequest, signal: AbortSignal): Promise<MatchResponse> {
    if (
      typeof request.playerId !== 'string' ||
      !request.playerId.trim() ||
      typeof request.mode !== 'string' ||
      !request.mode.trim()
    )
      throw new InvalidMatchRequest('Player and mode required');
    if (this.participants.has(request.playerId))
      throw new InvalidMatchRequest('Player already has a pending match request');
    signal.throwIfAborted();
    this.participants.add(request.playerId);
    const partner = this.queues.get(request.mode);
    let waiting: Waiting | undefined;
    let cancel: (() => void) | undefined;
    try {
      if (partner) {
        this.queues.delete(request.mode);
        try {
          const roomId = Names.roomPrefix + randomUUID().replaceAll('-', '');
          const assignment = await this.prepare(roomId, [partner.playerId, request.playerId]);
          if (assignment.roomId !== roomId)
            throw new Error('Prepared Room identity differs from the match.');
          partner.resolve(assignment);
          signal.throwIfAborted();
          return assignment;
        } catch (error) {
          partner.reject(error);
          throw error;
        }
      }
      return await new Promise<MatchResponse>((resolve, reject) => {
        waiting = { playerId: request.playerId, resolve, reject };
        this.queues.set(request.mode, waiting);
        cancel = () => reject(signal.reason);
        signal.addEventListener('abort', cancel, { once: true });
      });
    } finally {
      this.participants.delete(request.playerId);
      if (this.queues.get(request.mode) === waiting) this.queues.delete(request.mode);
      if (cancel) signal.removeEventListener('abort', cancel);
    }
  }
}
