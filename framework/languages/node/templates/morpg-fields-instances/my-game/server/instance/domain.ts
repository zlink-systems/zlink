import { Game, dungeonId } from '../shared/constants';
import { JoinDungeon, PrepareDungeon } from '../shared/messages';
export class DungeonState {
  partyId = '';
  members: string[] = [];
  stage = 0;
  prepare(request: PrepareDungeon, target: string): void {
    if (
      this.partyId ||
      !request.partyId.trim() ||
      !request.members.length ||
      request.members.length > Game.maximumPartyMembers ||
      new Set(request.members).size !== request.members.length ||
      request.members.some((id) => !id.trim()) ||
      dungeonId(request.partyId) !== target
    )
      throw new Error('Invalid dungeon party preparation');
    this.partyId = request.partyId;
    this.members = [...request.members];
  }
  admit(id: string, join: JoinDungeon, target: string): boolean {
    return join.dungeonId === target && join.partyId === this.partyId && this.members.includes(id);
  }
  advance(): void {
    if (!Number.isSafeInteger(this.stage + 1)) throw new Error('Stage exhausted');
    this.stage++;
  }
}
