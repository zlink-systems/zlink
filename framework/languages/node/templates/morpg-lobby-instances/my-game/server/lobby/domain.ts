import { Game, Rejection } from '../shared/constants';
import { PartyView, type PartyJoin } from '../shared/messages';
export class PartyState {
  partyId = '';
  members: string[] = [];
  ready: string[] = [];
  join(id: string, request: PartyJoin): boolean {
    if (
      !request.partyId.trim() ||
      this.members.includes(id) ||
      this.members.length >= Game.maximumPartyMembers ||
      (request.create
        ? this.members.length !== 0
        : this.partyId !== request.partyId || this.members.length === 0)
    )
      return false;
    this.partyId = request.partyId;
    this.members.push(id);
    return true;
  }
  markReady(id: string): void {
    if (!this.ready.includes(id)) this.ready.push(id);
  }
  leave(id: string): void {
    this.members = this.members.filter((m) => m !== id);
    this.ready = this.ready.filter((m) => m !== id);
  }
  view(): PartyView {
    return new PartyView(
      this.partyId,
      [...this.members],
      this.members.map((id) => this.ready.includes(id))
    );
  }
  entryRejection(id: string): string | null {
    return this.members[0] !== id
      ? Rejection.notLeader
      : this.members.some((m) => !this.ready.includes(m))
        ? Rejection.notReady
        : null;
  }
}
