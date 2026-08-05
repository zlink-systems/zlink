import { computed, signal } from '@preact/signals';
import type {
  JoinWorldRes,
  MoveRejectedNotify,
  PlayerView,
  ZoneChangedNotify,
  ZoneStateNotify,
} from '../../shared/api/contracts';

export class PlayerModel {
  readonly id = signal('');
  readonly x = signal(0);
  readonly y = signal(0);
  readonly zoneId = signal('—');
  readonly nodeId = signal('—');
  readonly transferred = signal(false);
  readonly visiblePlayers = signal<readonly PlayerView[]>([]);
  readonly rejection = signal<MoveRejectedNotify | null>(null);
  readonly joined = computed(() => this.id.value.length > 0);

  applyJoin(reply: JoinWorldRes): void {
    if (reply.error !== undefined && reply.error !== null) throw new Error(reply.error);
    this.id.value = reply.playerId;
    this.zoneId.value = reply.zoneId;
    this.nodeId.value = reply.nodeId;
    this.x.value = reply.x;
    this.y.value = reply.y;
  }

  applyZoneState(state: ZoneStateNotify): void {
    this.visiblePlayers.value = state.players;
    const own = state.players.find((player) => player.playerId === this.id.value);
    if (own === undefined) return;
    this.x.value = own.x;
    this.y.value = own.y;
  }

  applyZoneChange(change: ZoneChangedNotify): void {
    if (change.playerId !== this.id.value) return;
    this.zoneId.value = change.zoneId;
    this.nodeId.value = change.nodeId;
    this.transferred.value = change.transferred;
  }

  applyRejection(rejection: MoveRejectedNotify): void {
    this.rejection.value = rejection;
    this.x.value = rejection.x;
    this.y.value = rejection.y;
  }
}
