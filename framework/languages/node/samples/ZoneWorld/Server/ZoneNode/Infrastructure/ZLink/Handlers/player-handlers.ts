import { Inject, Injectable } from '@nestjs/common';
import {
  ZLINK_SPOT_OUTBOUND,
  zlinkEntrySpotActorRequestHandler,
  zlinkSpotActorRequestHandler,
  zlinkSpotActorSendHandler
} from '@zlink-systems/nestjs';
import {
  EnterWorldRes,
  EnterZoneMsg,
  BotTickRes,
  JoinWorldRes,
  MoveRejectedNotify,
  PacketNames,
} from '../../../../../Shared/contracts';
import { MoveRejectReasons, nodeOf, ZoneIds, zoneOf } from '../../../../../Shared/spec';
import type { ZoneId } from '../../../../../Shared/spec';
import { nextBotStep } from '../../../Domain/bot-patrol';
import { validateMove } from '../../../Domain/move-policy';
import { NodeRuntimeState } from '../../../Domain/node-runtime-state';
import type { BotTickReq, EnterWorldReq, JoinWorldReq, MoveMsg } from '../../../../../Shared/contracts';
import type {
  ZLinkMessageContext,
  ZLinkSpotOutbound
} from '@zlink-systems/framework';
import { PlayerActor } from '../Actors/player-actor';
import { ZoneEntrySpot } from '../Spots/zone-entry-spot';
import { ZoneSpot } from '../Spots/zone-spot';
import { UpdateZonePositionMsg } from './zone-runtime-handlers';

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  entrySpot: () => ZoneEntrySpot,
  actor: () => PlayerActor,
  packetName: PacketNames.enterWorldReq
})
class EntryEnterWorldHandler {
  async handle(
    _spot: ZoneEntrySpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    request: EnterWorldReq
  ): Promise<EnterWorldRes> {
    actor.dirX = request.dirX;
    actor.dirY = request.dirY;
    const targetZone = zoneOf(request.x, request.y);
    actor.beginPendingJoin();
    try {
      actor.context.joinSpot(
        targetZone,
        new EnterZoneMsg(actor.actorId, request.x, request.y, request.isBot, null)
      ).timeout(10_000).defer();
    } catch (error) {
      actor.completePendingJoin();
      throw error;
    }
    actor.x = request.x;
    actor.y = request.y;
    actor.zoneId = targetZone;
    actor.isBot = request.isBot;
    return new EnterWorldRes(
      targetZone,
      nodeOf(targetZone),
      request.x,
      request.y,
      null
    );
  }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  entrySpot: () => ZoneEntrySpot,
  actor: () => PlayerActor,
  packetName: PacketNames.joinWorldReq
})
class EntryJoinWorldHandler {
  constructor(private readonly nodeState: NodeRuntimeState) {}

  async handle(
    _spot: ZoneEntrySpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    request: JoinWorldReq
  ): Promise<JoinWorldRes> {
    assertPlayerId(request.playerId, actor.actorId);
    if (Object.values(ZoneIds).includes(actor.context.spotId as ZoneId)) {
      return new JoinWorldRes(actor.actorId, actor.zoneId, nodeOf(actor.zoneId), actor.x, actor.y);
    }
    const targetZone = zoneOf(actor.x, actor.y);
    // The entry request must return a deterministic public result. The later
    // User Spot admission remains authoritative, but a deferred join cannot
    // carry that result back into this already-returning request.
    if (this.nodeState.isUnderMaintenance(nodeOf(targetZone))) {
      return new JoinWorldRes(
        actor.actorId,
        targetZone,
        nodeOf(targetZone),
        actor.x,
        actor.y,
        MoveRejectReasons.zoneMaintenance
      );
    }
    console.log(`join world handler actor=${actor.actorId} current=${String(actor.context.spotId)} target=${targetZone}`);
    actor.beginPendingJoin();
    try {
      actor.context.joinSpot(
        targetZone,
        new EnterZoneMsg(actor.actorId, actor.x, actor.y, false, null)
      ).timeout(10_000).defer();
    } catch (error) {
      actor.completePendingJoin();
      throw error;
    }
    console.log(`join world deferred actor=${actor.actorId} target=${targetZone}`);
    actor.zoneId = targetZone;
    return new JoinWorldRes(actor.actorId, targetZone, nodeOf(targetZone), actor.x, actor.y);
  }
}

@Injectable()
class PlayerMovement {
  constructor(
    private readonly nodeState: NodeRuntimeState,
    @Inject(ZLINK_SPOT_OUTBOUND) private readonly spotOutbound: ZLinkSpotOutbound
  ) {}

  async move(actor: PlayerActor, x: number, y: number): Promise<void> {
    const previousZone = actor.zoneId;
    const decision = validateMove(actor, x, y, (zoneId) => this.nodeState.targetUnavailable(zoneId));
    if (decision.kind === 'rejected') {
      await this.reject(actor, decision.reason);
      return;
    }
    const targetZone = zoneOf(x, y);
    if (!decision.zoneChanged) {
      actor.x = x;
      actor.y = y;
      const spotRid = actor.context.spotId;
      if (spotRid === undefined) throw new Error(`Player '${actor.actorId}' is not joined to a zone.`);
      await this.spotOutbound
        .sendToSpot(spotRid, new UpdateZonePositionMsg(actor.actorId, x, y))
        .submit();
      return;
    }
    actor.beginPendingJoin();
    try {
      actor.context.joinSpot(
        targetZone,
        new EnterZoneMsg(actor.actorId, x, y, actor.isBot, nodeOf(previousZone))
      ).timeout(10_000).defer();
    } catch (error) {
      actor.completePendingJoin();
      throw error;
    }
    console.log(`zone change scheduled player=${actor.actorId} from=${previousZone} to=${targetZone}`);
    actor.x = x;
    actor.y = y;
    actor.zoneId = targetZone;
  }

  private async reject(actor: PlayerActor, reason: typeof MoveRejectReasons[keyof typeof MoveRejectReasons]): Promise<void> {
    if (actor.isBot) {
      actor.dirX *= -1;
      actor.dirY *= -1;
      console.log(`bot direction reversed bot=${actor.actorId} x=${actor.x} y=${actor.y}`);
      return;
    }
    await actor.push(new MoveRejectedNotify(reason, actor.x, actor.y));
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => ZoneSpot,
  actor: () => PlayerActor,
  packetName: PacketNames.moveMsg
})
class PlayerMoveHandler {
  constructor(private readonly movement: PlayerMovement) {}

  async handle(
    _spot: ZoneSpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    message: MoveMsg
  ): Promise<void> {
    if (actor.actorId === 'player-a1') {
      console.log(
        `player move handler actor=${actor.actorId} spot=${String(actor.context.spotId)}`
          + ` from=${actor.x},${actor.y} to=${message.x},${message.y}`
      );
    }
    await this.movement.move(actor, message.x, message.y);
  }
}

@Injectable()
@zlinkSpotActorRequestHandler({
  spot: () => ZoneSpot,
  actor: () => PlayerActor,
  packetName: PacketNames.botTickReq
})
class PlayerBotTickHandler {
  constructor(private readonly movement: PlayerMovement) {}

  async handle(
    _spot: ZoneSpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    _message: BotTickReq
  ): Promise<BotTickRes> {
    if (!actor.isBot || actor.hasPendingJoin) return new BotTickRes();
    const next = nextBotStep(actor.x, actor.y, actor.dirX, actor.dirY);
    await this.movement.move(actor, next.x, next.y);
    return new BotTickRes();
  }
}

function assertPlayerId(requestPlayerId: string, actorId: string): void {
  if (!/^[a-z0-9-]{1,32}$/.test(requestPlayerId) || requestPlayerId !== actorId) {
    throw new Error(`Invalid player id '${requestPlayerId}'.`);
  }
}

export {
  EntryEnterWorldHandler,
  EntryJoinWorldHandler,
  PlayerBotTickHandler,
  PlayerMoveHandler,
  PlayerMovement,
};
