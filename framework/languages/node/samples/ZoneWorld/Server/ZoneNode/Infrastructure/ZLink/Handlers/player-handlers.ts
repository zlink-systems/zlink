import { Inject, Injectable } from '@nestjs/common';
import {
  ZLINK_SPOT_OUTBOUND,
  zlinkEntrySpotActorRequestHandler,
  zlinkEntrySpotActorSendHandler,
  zlinkSpotActorRequestHandler,
  zlinkSpotActorSendHandler
} from '@zlink-systems/nestjs';
import {
  EnterWorldRes,
  EnterZoneReq,
  MessageFollowProbeRes,
  MoveRejectedNotify,
  PacketNames,
} from '../../../../../Shared/contracts';
import { MoveRejectReasons, ZoneIds, zoneOf } from '../../../../../Shared/spec';
import type { ZoneId } from '../../../../../Shared/spec';
import { nextBotStep } from '../../../Domain/bot-patrol';
import { validateMove } from '../../../Domain/move-policy';
import type {
  BotTickMsg,
  EnterWorldReq,
  JoinWorldReq,
  MessageFollowProbeReq,
  MessageFollowProbeMsg,
  MoveMsg
} from '../../../../../Shared/contracts';
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
    actor.beginPendingJoin(request.isBot ? 'bot' : 'world');
    try {
      actor.context.joinSpot(
        targetZone,
        new EnterZoneReq(actor.actorId, request.x, request.y, request.isBot, true)
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
      request.x,
      request.y,
      null
    );
  }
}

@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => ZoneEntrySpot,
  actor: () => PlayerActor,
  packetName: PacketNames.joinWorldReq
})
class EntryJoinWorldHandler {
  async handle(
    _spot: ZoneEntrySpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    request: JoinWorldReq
  ): Promise<void> {
    assertPlayerId(request.playerId, actor.actorId);
    if (Object.values(ZoneIds).includes(actor.context.spotId as ZoneId)) {
      await actor.sendJoinResult(null);
      return;
    }
    const targetZone = zoneOf(actor.x, actor.y);
    console.log(`join world handler actor=${actor.actorId} current=${String(actor.context.spotId)} target=${targetZone}`);
    actor.beginPendingJoin('world');
    try {
      actor.context.joinSpot(
        targetZone,
        new EnterZoneReq(actor.actorId, actor.x, actor.y, false, true)
      ).timeout(10_000).defer();
    } catch (error) {
      actor.completePendingJoin();
      throw error;
    }
    console.log(`join world deferred actor=${actor.actorId} target=${targetZone}`);
    actor.zoneId = targetZone;
  }
}

@Injectable()
class PlayerMovement {
  constructor(
    @Inject(ZLINK_SPOT_OUTBOUND) private readonly spotOutbound: ZLinkSpotOutbound
  ) {}

  async move(actor: PlayerActor, x: number, y: number): Promise<void> {
    const previousZone = actor.zoneId;
    const decision = validateMove(actor, x, y, () => false);
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
    actor.beginPendingJoin(actor.isBot ? 'bot' : 'move');
    try {
      actor.context.joinSpot(
        targetZone,
        new EnterZoneReq(actor.actorId, x, y, actor.isBot, false)
      ).timeout(10_000).defer();
    } catch (error) {
      actor.completePendingJoin();
      throw error;
    }
    console.log(`zone change scheduled player=${actor.actorId} from=${previousZone} to=${targetZone}`);
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

/**
 * Answers the runner-only Message Follow probe on whichever node currently
 * owns the actor. Echoing the probe id and payload proves the followed
 * request kept its payload and reply correlation (ZW-B6).
 */
@Injectable()
@zlinkSpotActorRequestHandler({
  spot: () => ZoneSpot,
  actor: () => PlayerActor,
  packetName: PacketNames.messageFollowProbeReq
})
class PlayerMessageFollowProbeHandler {
  async handle(
    _spot: ZoneSpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    request: MessageFollowProbeReq
  ): Promise<MessageFollowProbeRes> {
    console.log(
      `message-follow probe handled actor=${actor.actorId} probe=${request.probeId} payload=${request.payload}`
    );
    return new MessageFollowProbeRes(request.probeId, request.payload);
  }
}

/**
 * Records the one-way half of the Message Follow probe. A one-way send has no
 * reply that could prove target execution, so the runner reads this log line.
 */
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => ZoneSpot,
  actor: () => PlayerActor,
  packetName: PacketNames.messageFollowProbeMsg
})
class PlayerMessageFollowProbeSendHandler {
  async handle(
    _spot: ZoneSpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    message: MessageFollowProbeMsg
  ): Promise<void> {
    console.log(
      `message-follow probe one-way handled actor=${actor.actorId} probe=${message.probeId} payload=${message.payload}`
    );
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => ZoneSpot,
  actor: () => PlayerActor,
  packetName: PacketNames.botTickMsg
})
class PlayerBotTickHandler {
  constructor(private readonly movement: PlayerMovement) {}

  async handle(
    _spot: ZoneSpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    _message: BotTickMsg
  ): Promise<void> {
    if (!actor.isBot || actor.hasPendingJoin) return;
    const next = nextBotStep(actor.x, actor.y, actor.dirX, actor.dirY);
    await this.movement.move(actor, next.x, next.y);
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
  PlayerMessageFollowProbeHandler,
  PlayerMessageFollowProbeSendHandler,
  PlayerMoveHandler,
  PlayerMovement,
};
