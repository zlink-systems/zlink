import { Game } from '../shared/constants';
import type { PrepareRoom } from '../shared/messages';
import type { RoomSnapshot } from './domain';

export interface RosterSnapshot {
  matchId: string | null;
  members: string[];
}

export interface HttpRoomSnapshot {
  Game: RoomSnapshot;
  Roster: RosterSnapshot;
}

export class RoomRoster {
  private matchId: string | null = null;
  private members = new Set<string>();

  prepare(roomId: string, request: PrepareRoom): string {
    if (
      !roomId.trim() ||
      !request ||
      request.matchId !== roomId ||
      !Array.isArray(request.members) ||
      request.members.length !== Game.playersPerRoom ||
      request.members.some((member) => typeof member !== 'string' || !member.trim()) ||
      new Set(request.members).size !== request.members.length
    )
      throw new Error('Invalid Room roster preparation');

    if (this.matchId !== null) {
      if (
        this.matchId !== request.matchId ||
        this.members.size !== request.members.length ||
        request.members.some((member) => !this.members.has(member))
      )
        throw new Error('A prepared Room roster cannot be replaced');
      return this.matchId;
    }

    this.matchId = request.matchId;
    this.members = new Set(request.members);
    return this.matchId;
  }

  canAdmit(playerId: string): boolean {
    return this.matchId !== null && this.members.has(playerId);
  }

  capture(): RosterSnapshot {
    return { matchId: this.matchId, members: [...this.members].sort() };
  }

  restore(snapshot: RosterSnapshot): void {
    if (this.matchId !== null) throw new Error('Cannot restore an already prepared Room roster');
    if (!snapshot || !Array.isArray(snapshot.members))
      throw new Error('Missing Room roster members');
    if (snapshot.matchId !== null) {
      this.prepare(snapshot.matchId, {
        matchId: snapshot.matchId,
        members: snapshot.members
      });
    } else if (snapshot.members.length !== 0) {
      throw new Error('Unprepared Room snapshot has members');
    }
  }
}
