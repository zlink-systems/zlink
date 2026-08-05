import { AnnouncementModel } from '../../entities/announcement/model';
import { PlayerModel } from '../../entities/player/model';
import type {
  JoinWorldRes,
  MoveRejectedNotify,
  WorldAnnounceNotify,
  ZoneChangedNotify,
  ZoneStateNotify,
} from '../../shared/api/contracts';
import { Packets } from '../../shared/api/contracts';
import { StreamClient } from '../../shared/api/stream';
import { MAX_STEP_PER_AXIS } from '../../shared/config/world';

export class GameController {
  readonly player = new PlayerModel();
  readonly announcements = new AnnouncementModel();
  readonly stream: StreamClient;

  constructor(endpoint: string) {
    this.stream = new StreamClient(endpoint);
    this.stream.on<ZoneStateNotify>(Packets.ZoneStateNotify, (message) => this.player.applyZoneState(message));
    this.stream.on<ZoneChangedNotify>(Packets.ZoneChangedNotify, (message) => this.player.applyZoneChange(message));
    this.stream.on<MoveRejectedNotify>(Packets.MoveRejectedNotify, (message) => this.player.applyRejection(message));
    this.stream.on<WorldAnnounceNotify>(Packets.WorldAnnounceNotify, (message) => this.announcements.apply(message));
  }

  async join(playerId: string): Promise<void> {
    if (playerId.trim().length === 0) throw new Error('Player ID is required.');
    await this.stream.connect();
    const reply = await this.stream.request<JoinWorldRes>(Packets.JoinWorldReq, { playerId: playerId.trim() });
    this.player.applyJoin(reply);
  }

  move(dx: number, dy: number): void {
    if (!this.player.joined.value) return;
    // This computes command input only. Position signals change exclusively when the server
    // replies by push, so rejected or delayed movement is never rendered optimistically.
    this.stream.send(Packets.MoveMsg, {
      x: this.player.x.value + Math.max(-MAX_STEP_PER_AXIS, Math.min(MAX_STEP_PER_AXIS, dx)),
      y: this.player.y.value + Math.max(-MAX_STEP_PER_AXIS, Math.min(MAX_STEP_PER_AXIS, dy)),
    });
  }

  close(): Promise<void> {
    return this.stream.close();
  }
}
