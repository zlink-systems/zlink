import { Injectable } from '@nestjs/common';
import { createHash } from 'node:crypto';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkActorRelocationAdapter,
  ZLinkSpotRelocationAdapter,
  ZLinkEntrySpotActorRequestHandler,
  ZLinkSpotActorRequestHandler,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import { ZLinkPacket, ZLinkSpotActorRequest } from '@zlink-systems/framework';
import {
  zlinkEntrySpotActorRequestHandler,
  zlinkSpotActorRequestHandler
} from '@zlink-systems/nestjs';

export const Config6ActorType = 'Config6Actor';
export const Config6UserSpotType = 'Config6UserSpot';

export class Config6Actor implements ZLinkActor {
  readonly context!: ZLinkActorContext;
  state = '';

  constructor(context: ZLinkActorContext) {
    Object.defineProperty(this, 'context', { value: context });
  }
}

@Injectable()
export class Config6ActorAdapter implements ZLinkActorRelocationAdapter<Config6Actor> {
  async capture(actor: Config6Actor, signal: AbortSignal): Promise<Uint8Array> {
    signal.throwIfAborted();
    return new TextEncoder().encode(actor.state);
  }

  async restore(actor: Config6Actor, payload: Uint8Array, signal: AbortSignal): Promise<void> {
    signal.throwIfAborted();
    actor.state = new TextDecoder().decode(payload);
  }
}

@Injectable()
export class Config6ActorFactory implements ZLinkActorFactory {
  async create(context: ZLinkActorContext): Promise<Config6Actor> {
    if (String(context.actorId).includes('factory-fail')) {
      throw new Error('injected Config 6 Actor factory failure');
    }
    return new Config6Actor(context);
  }
}

@Injectable()
export class Config6EntrySpot implements ZLinkEntrySpot<Config6Actor> {
  readonly context!: ZLinkEntrySpotContext<Config6Actor>;

  async onCreateActor(actor: Config6Actor, request: ZLinkMessage): Promise<{ accepted: boolean }> {
    actor.state = request.decode<{ readonly state?: string }>(Object as never).state ?? '';
    return { accepted: true };
  }

  async onJoinedActor(_actor: Config6Actor): Promise<void> {}
  async onLeaveActor(_actor: Config6Actor): Promise<void> {}
  async onDisconnectActor(_actor: Config6Actor): Promise<void> {}
}

@Injectable()
export class Config6UserSpot implements ZLinkSpot<Config6Actor> {
  readonly context!: ZLinkSpotContext<Config6Actor>;
  state = '';
  stateBytes: Uint8Array = new Uint8Array();
  stateChecksum = createHash('sha256').update(this.stateBytes).digest('hex');

  async onCreate(request: ZLinkMessage): Promise<{ accepted: boolean }> {
    const value = request.decode<{
      readonly failFactory?: boolean;
      readonly state?: string;
      readonly stateLength?: number;
      readonly fillByte?: number;
    }>(Object as never);
    if (value.failFactory === true) {
      throw new Error('injected Config 6 User Spot factory failure');
    }
    if (value.stateLength !== undefined) {
      if (!Number.isSafeInteger(value.stateLength) || value.stateLength < 0) {
        throw new RangeError('stateLength must be a non-negative safe integer.');
      }
      const fillByte = value.fillByte ?? 0x5a;
      if (!Number.isInteger(fillByte) || fillByte < 0 || fillByte > 255) {
        throw new RangeError('fillByte must be an integer in 0..255.');
      }
      this.setState(Buffer.alloc(value.stateLength, fillByte));
    } else {
      this.setState(new TextEncoder().encode(value.state ?? ''));
    }
    return { accepted: true };
  }

  setState(payload: Uint8Array): void {
    this.stateBytes = payload;
    this.state = payload.byteLength <= 1024
      ? new TextDecoder().decode(payload)
      : '';
    this.stateChecksum = createHash('sha256').update(payload).digest('hex');
  }

  async onActorJoin(_actorId: string, _request: ZLinkMessage): Promise<{ accepted: boolean }> {
    return { accepted: true };
  }

  async onJoinedActor(_actor: Config6Actor): Promise<void> {}
  async onLeaveActor(_actor: Config6Actor): Promise<void> {}
  async onDisconnectActor(_actor: Config6Actor): Promise<void> {}
}

@Injectable()
export class Config6UserSpotAdapter implements ZLinkSpotRelocationAdapter<Config6UserSpot> {
  async capture(spot: Config6UserSpot, signal: AbortSignal): Promise<Uint8Array> {
    signal.throwIfAborted();
    return spot.stateBytes;
  }

  async restore(spot: Config6UserSpot, payload: Uint8Array, signal: AbortSignal): Promise<void> {
    signal.throwIfAborted();
    spot.setState(payload);
  }
}

@ZLinkPacket('Config6JoinReq')
export class Config6JoinReq {
  constructor(readonly spotId: string) {}
}

@ZLinkPacket('Config6ProbeReq')
export class Config6ProbeReq {}

export interface Config6ProbeRes {
  readonly actorId: string;
  readonly actorState: string;
  readonly spotId: string;
  readonly spotState: string;
  readonly spotStateLength: number;
  readonly spotStateChecksum: string;
  readonly nodeRid: string;
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  actor: () => Config6Actor,
  entrySpot: () => Config6EntrySpot,
  packetName: 'Config6JoinReq'
})
export class Config6JoinHandler implements
  ZLinkEntrySpotActorRequestHandler<Config6EntrySpot, Config6Actor, Config6JoinReq, { accepted: true }> {
  @ZLinkSpotActorRequest('Config6JoinReq')
  async handle(
    _spot: Config6EntrySpot,
    actor: Config6Actor,
    _context: ZLinkMessageContext,
    request: Config6JoinReq
  ): Promise<{ accepted: true }> {
    actor.context.joinSpot(request.spotId, {}).timeout(10_000).defer();
    return { accepted: true };
  }
}

@Injectable()
@zlinkSpotActorRequestHandler({
  actor: () => Config6Actor,
  spot: () => Config6UserSpot,
  packetName: 'Config6ProbeReq'
})
export class Config6ProbeHandler implements
  ZLinkSpotActorRequestHandler<Config6UserSpot, Config6Actor, Config6ProbeReq, Config6ProbeRes> {
  @ZLinkSpotActorRequest('Config6ProbeReq')
  async handle(
    spot: Config6UserSpot,
    actor: Config6Actor
  ): Promise<Config6ProbeRes> {
    return {
      actorId: actor.context.actorId,
      actorState: actor.state,
      spotId: String(spot.context.spotId),
      spotState: spot.state,
      spotStateLength: spot.stateBytes.byteLength,
      spotStateChecksum: spot.stateChecksum,
      nodeRid: String(spot.context.nodeRid)
    };
  }
}
