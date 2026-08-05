import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_MANAGER, ZLINK_CHANNEL_CLIENT } from '@zlink-systems/nestjs';
import { ZLinkPacket } from '@zlink-systems/framework';
import { SampleNames, SampleTimings } from '../../Configuration/sample-names';
import {
  AuthenticateRes,
  PacketNames,
  SupportChatRoles,
  authenticateUser,
  SupportUserActorCreateReq
} from '../../../Shared/Contracts/messages';
import type {
  AuthenticateReq,
  AuthenticateUserRes,
  SupportRole
} from '../../../Shared/Contracts/messages';
import type {
  ZLinkActorManager,
  ZLinkChannelClient,
  ZLinkMessage,
  ZLinkSession,
  ZLinkSessionActor,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext,
  ZLinkSessionFactory,
  ActorRef
} from '@zlink-systems/framework';

type SessionIdentity = {
  readonly actorId: string;
  readonly displayName: string;
  readonly role: SupportRole;
  readonly conversationActors: Map<string, string>;
};

@Injectable()
class SupportChatSessionRouter {
  private readonly identities = new WeakMap<ZLinkSessionContext, SessionIdentity>();

  constructor(
    @Inject(ZLINK_CHANNEL_CLIENT) private readonly channels: ZLinkChannelClient,
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager
  ) {}

  async authenticate(context: ZLinkSessionContext, payload: ZLinkMessage): Promise<void> {
    const request = payload.decode<AuthenticateReq>(Object as never);
    const authenticated = await this.channels
      .requestToChannel(SampleNames.apiChannel, authenticateUser(request.accessToken))
      .timeout(SampleTimings.requestTimeout)
      .submit<AuthenticateUserRes>();
    if (!authenticated.accepted || authenticated.actorId === undefined || authenticated.displayName === undefined || authenticated.role === undefined) {
      throw new Error(authenticated.reason ?? 'SupportChat authentication failed.');
    }
    const actorRef = await this.getOrCreateActor(
      authenticated.actorId,
      new SupportUserActorCreateReq(
        authenticated.actorId,
        authenticated.displayName,
        authenticated.role,
        authenticated.actorId
      )
    );
    await context.actors.bindOrGet(actorRef);
    this.identities.set(context, {
      actorId: authenticated.actorId,
      displayName: authenticated.displayName,
      role: authenticated.role,
      conversationActors: new Map()
    });
    context.client.reply(new AuthenticateRes(
      authenticated.actorId,
      authenticated.displayName,
      authenticated.role
    )).submit();
  }

  async relayIdentity(context: ZLinkSessionContext, payload: ZLinkMessage): Promise<void> {
    await this.requireIdentityActor(context).relay(payload);
  }

  async relayConversation(
    context: ZLinkSessionContext,
    dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const identity = this.requireIdentity(context);
    const conversationId = dispatch.metadata.get(SampleNames.conversationIdMetadataKey);
    if (conversationId === undefined || conversationId.length === 0) {
      if (dispatch.packetName === PacketNames.setTypingMsg) return;
      throw new Error(`Conversation metadata is required for '${dispatch.packetName}'.`);
    }
    if (identity.role === SupportChatRoles.Agent
        && dispatch.packetName === PacketNames.joinConversationReq
        && !identity.conversationActors.has(conversationId)) {
      const actorId = `${identity.actorId}@${conversationId}`;
      const actorRef = await this.getOrCreateActor(
        actorId,
        new SupportUserActorCreateReq(
          actorId,
          identity.displayName,
          SupportChatRoles.Agent,
          identity.actorId
        )
      );
      await context.actors.bindOrGet(actorRef);
      identity.conversationActors.set(conversationId, actorRef.actorId);
      const actor = context.actors.find(actorRef.actorId);
      if (actor === undefined) throw new Error(`Bound conversation actor '${actorRef.actorId}' was not found.`);
      await actor.relay(payload);
      return;
    }
    const actor = await this.conversationActor(context, identity, conversationId, dispatch.packetName);
    if (actor !== undefined) await actor.relay(payload);
  }

  private requireIdentity(context: ZLinkSessionContext): SessionIdentity {
    const identity = this.identities.get(context);
    if (identity === undefined) throw new Error('AuthenticateReq is required before support packets.');
    return identity;
  }

  private requireIdentityActor(context: ZLinkSessionContext): ZLinkSessionActor {
    const identity = this.requireIdentity(context);
    const actor = context.actors.find(identity.actorId);
    if (actor === undefined) throw new Error(`Bound identity actor '${identity.actorId}' was not found.`);
    return actor;
  }

  private async conversationActor(
    context: ZLinkSessionContext,
    identity: SessionIdentity,
    conversationId: string,
    packetName: string
  ): Promise<ZLinkSessionActor | undefined> {
    if (identity.role === SupportChatRoles.Customer) return this.requireIdentityActor(context);
    let actorId = identity.conversationActors.get(conversationId);
    if (actorId === undefined) {
      if (packetName === PacketNames.setTypingMsg) return undefined;
      throw new Error(`JoinConversationReq is required before '${packetName}'.`);
    }
    return context.actors.find(actorId);
  }

  private async getOrCreateActor(actorId: string, request: SupportUserActorCreateReq): Promise<ActorRef> {
    const result = await this.actors
      .getOrCreate(actorId, SampleNames.supportActorType)
      .inMesh(SampleNames.meshName)
      .request(request)
      .timeout(SampleTimings.requestTimeout)
      .submit();
    if (result.status === 'rejected') throw new Error(`Support actor '${actorId}' creation was rejected.`);
    return result.actor;
  }
}

@Injectable()
@ZLinkPacket(PacketNames.authenticateReq)
class AuthenticateSupportChatSessionHandler {
  constructor(private readonly router: SupportChatSessionRouter) {}
  async handle(context: ZLinkSessionContext, _dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    await this.router.authenticate(context, payload);
  }
}

function identityHandler(packetName: string) {
  @Injectable()
  @ZLinkPacket(packetName)
  class IdentityHandler {
    constructor(readonly router: SupportChatSessionRouter) {}
    async handle(context: ZLinkSessionContext, _dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
      await this.router.relayIdentity(context, payload);
    }
  }
  return IdentityHandler;
}

function conversationHandler(packetName: string) {
  @Injectable()
  @ZLinkPacket(packetName)
  class ConversationHandler {
    constructor(readonly router: SupportChatSessionRouter) {}
    async handle(context: ZLinkSessionContext, dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
      await this.router.relayConversation(context, dispatch, payload);
    }
  }
  return ConversationHandler;
}

const OpenConversationSessionHandler = identityHandler(PacketNames.openConversationReq);
const SetAgentAvailableSessionHandler = identityHandler(PacketNames.setAgentAvailableReq);
const JoinConversationSessionHandler = conversationHandler(PacketNames.joinConversationReq);
const SendChatMessageSessionHandler = conversationHandler(PacketNames.sendChatMessageReq);
const SetTypingSessionHandler = conversationHandler(PacketNames.setTypingMsg);
const CloseConversationSessionHandler = conversationHandler(PacketNames.closeConversationReq);
class SupportChatSession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (!await this.context.handlers.tryHandle(dispatch, payload)) {
      throw new Error(`Unsupported SupportChat packet '${dispatch.packetName}'.`);
    }
  }

  async onDisconnected(): Promise<void> {
    await Promise.allSettled(this.context.actors.bound.map((actor) => actor.notifyDisconnected()));
  }
}

class SupportChatSessionFactory implements ZLinkSessionFactory<SupportChatSession> {
  async create(context: ZLinkSessionContext): Promise<SupportChatSession> {
    return new SupportChatSession(context);
  }
}

export {
  AuthenticateSupportChatSessionHandler,
  CloseConversationSessionHandler,
  JoinConversationSessionHandler,
  OpenConversationSessionHandler,
  SendChatMessageSessionHandler,
  SetAgentAvailableSessionHandler,
  SetTypingSessionHandler,
  SupportChatSession,
  SupportChatSessionFactory,
  SupportChatSessionRouter
};
