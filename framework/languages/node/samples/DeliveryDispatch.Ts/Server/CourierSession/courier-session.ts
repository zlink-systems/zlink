import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_MANAGER } from '@zlink-systems/nestjs';
import { SampleNames, SampleTimings } from '../../Shared/Configuration/sample-names';
import { ensureCourierActor, PacketNames } from '../../Shared/Contracts/messages';
import {
  ZLinkPacket,
  type ActorRef,
  type ZLinkActorManager,
  type ZLinkMessage,
  type ZLinkSession,
  type ZLinkSessionContext,
  type ZLinkSessionDispatchContext,
  type ZLinkSessionFactory
} from '@zlink-systems/framework';
import type { BindCourierSessionReq } from '../../Shared/Contracts/messages';

class CourierSession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (await this.context.handlers.tryHandle(dispatch, payload)) return;
    const actor = this.context.actors.bound.length === 1 ? this.context.actors.bound[0] : undefined;
    if (actor === undefined) throw new Error(`BindCourierSessionReq is required before '${dispatch.packetName}'.`);
    await actor.relay(payload);
  }
}

@Injectable()
@ZLinkPacket(PacketNames.bindCourierSession)
class BindCourierSessionHandler {
  constructor(
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actorManager: ZLinkActorManager
  ) {}

  async handle(context: ZLinkSessionContext, _dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    const request = payload.decode<BindCourierSessionReq>(Object as never);
    const actorRef = await this.findOrEnsureActor(request.courierId);
    const actor = await context.actors.bindOrGet(actorRef);
    console.error(`deliverydispatch courier-session: bound courier=${request.courierId} actor=${actorRef.actorId}`);
    await actor.relay(payload);
  }

  private async findOrEnsureActor(courierId: string): Promise<ActorRef> {
    const result = await this.actorManager
      .getOrCreate(courierId, SampleNames.courierActorType)
      .inMesh(SampleNames.courierMeshName)
      .request(ensureCourierActor(courierId))
      .timeout(SampleTimings.requestTimeout)
      .submit();
    if (result.status === 'rejected') throw new Error('Courier Actor creation was rejected.');
    return result.actor;
  }
}

class CourierSessionFactory implements ZLinkSessionFactory<CourierSession> {
  async create(context: ZLinkSessionContext): Promise<CourierSession> {
    return new CourierSession(context);
  }
}

export { BindCourierSessionHandler, CourierSession, CourierSessionFactory };
