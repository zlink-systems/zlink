import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_MANAGER, ZLINK_CHANNEL_CLIENT } from '@zlink-systems/nestjs';
import { ZLinkPacket } from '@zlink-systems/framework';
import { SampleNames, SampleTimings } from '../../Configuration/sample-names';
import {
  AuthenticateReq,
  AuthenticateRes,
  JoinConversationReq,
  PacketNames,
  SupportChatRoles,
  authenticateUser,
  SupportUserActorCreateReq
} from '../../../Shared/Contracts/messages';
import type { AuthenticateUserRes, SupportRole } from '../../../Shared/Contracts/messages';
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
};

@Injectable()
class SupportChatSessionRouter {
  private readonly identities = new WeakMap<ZLinkSessionContext, SessionIdentity>();

  constructor(
    @Inject(ZLINK_CHANNEL_CLIENT) private readonly channels: ZLinkChannelClient,
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager
  ) {}

  async authenticate(context: ZLinkSessionContext, payload: ZLinkMessage): Promise<void> {
    const request = payload.decode(AuthenticateReq);
    const authenticated = await this.channels
      .requestToChannel(SampleNames.apiChannel, authenticateUser(request.accessToken))
      .timeout(SampleTimings.requestTimeout)
      .submit<AuthenticateUserRes>();
    if (
      !authenticated.accepted ||
      authenticated.actorId === undefined ||
      authenticated.displayName === undefined ||
      authenticated.role === undefined
    ) {
      throw new Error(authenticated.reason ?? 'SupportChat authentication failed.');
    }
    // --8<-- [start:doc-sc-session-auth]
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
      role: authenticated.role
    });
    // --8<-- [end:doc-sc-session-auth]
    context.client
      .reply(
        new AuthenticateRes(authenticated.actorId, authenticated.displayName, authenticated.role)
      )
      .submit();
  }

  // --8<-- [start:doc-sc-agent-join]
  async joinConversation(context: ZLinkSessionContext, payload: ZLinkMessage): Promise<void> {
    const identity = this.requireIdentity(context);
    if (identity.role === SupportChatRoles.Agent) {
      const request = payload.decode(JoinConversationReq);
      const actorId = `${identity.actorId}@${request.conversationId}`;
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
      const actor = context.actors.find(actorRef.actorId);
      if (actor === undefined)
        throw new Error(`Bound conversation actor '${actorRef.actorId}' was not found.`);
      await actor.relay(payload);
      return;
    }
    await this.requireIdentityActor(context).relay(payload);
  }
  // --8<-- [end:doc-sc-agent-join]

  async relay(
    context: ZLinkSessionContext,
    dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    // --8<-- [start:doc-sc-actor-relay]
    const actor = dispatch.actor ?? this.requireIdentityActor(context);
    // --8<-- [end:doc-sc-actor-relay]
    await actor.relay(payload);
  }

  private requireIdentity(context: ZLinkSessionContext): SessionIdentity {
    const identity = this.identities.get(context);
    if (identity === undefined)
      throw new Error('AuthenticateReq is required before support packets.');
    return identity;
  }

  private requireIdentityActor(context: ZLinkSessionContext): ZLinkSessionActor {
    const identity = this.requireIdentity(context);
    const actor = context.actors.find(identity.actorId);
    if (actor === undefined)
      throw new Error(`Bound identity actor '${identity.actorId}' was not found.`);
    return actor;
  }

  private async getOrCreateActor(
    actorId: string,
    request: SupportUserActorCreateReq
  ): Promise<ActorRef> {
    const result = await this.actors
      .getOrCreate(actorId, SampleNames.supportActorType)
      .inMesh(SampleNames.meshName)
      .request(request)
      .timeout(SampleTimings.requestTimeout)
      .submit();
    if (result.status === 'rejected')
      throw new Error(`Support actor '${actorId}' creation was rejected.`);
    return result.actor;
  }
}

@Injectable()
@ZLinkPacket(PacketNames.authenticateReq)
class AuthenticateSupportChatSessionHandler {
  constructor(private readonly router: SupportChatSessionRouter) {}
  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    await this.router.authenticate(context, payload);
  }
}

function relayHandler(packetName: string) {
  @Injectable()
  @ZLinkPacket(packetName)
  class IdentityHandler {
    constructor(readonly router: SupportChatSessionRouter) {}
    async handle(
      context: ZLinkSessionContext,
      dispatch: ZLinkSessionDispatchContext,
      payload: ZLinkMessage
    ): Promise<void> {
      await this.router.relay(context, dispatch, payload);
    }
  }
  return IdentityHandler;
}

// --8<-- [start:doc-sc-session-dispatch]
const OpenConversationSessionHandler = relayHandler(PacketNames.openConversationReq);
const SetAgentAvailableSessionHandler = relayHandler(PacketNames.setAgentAvailableReq);
@Injectable()
@ZLinkPacket(PacketNames.joinConversationReq)
class JoinConversationSessionHandler {
  constructor(private readonly router: SupportChatSessionRouter) {}
  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    await this.router.joinConversation(context, payload);
  }
}
const SendChatMessageSessionHandler = relayHandler(PacketNames.sendChatMessageReq);
const SetTypingSessionHandler = relayHandler(PacketNames.setTypingMsg);
const CloseConversationSessionHandler = relayHandler(PacketNames.closeConversationReq);
class SupportChatSession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (!(await this.context.handlers.tryHandle(dispatch, payload))) {
      throw new Error(`Unsupported SupportChat packet '${dispatch.packetName}'.`);
    }
  }

  async onDisconnected(): Promise<void> {
    // Framework cleanup notifies the exact bound-actor snapshot. Re-submitting
    // actor.notifyDisconnected() here can race a later session binding.
  }
}
// --8<-- [end:doc-sc-session-dispatch]

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
