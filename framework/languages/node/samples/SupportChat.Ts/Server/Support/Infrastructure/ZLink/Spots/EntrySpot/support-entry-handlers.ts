import { Inject } from '@nestjs/common';
import {
  ZLINK_CHANNEL_CLIENT,
  zlinkEntrySpotActorRequestHandler
} from '@zlink-systems/nestjs';
import { AgentAvailabilityDirectory } from '../../../../Application/ConversationAssignment/agent-availability-directory';
import { SampleNames, SampleTimings } from '../../../../../Configuration/sample-names';
import {
  PacketNames,
  SupportChatRoles,
  openConversationApi
} from '../../../../../../Shared/Contracts/messages';
import {
  JoinSupportConversation,
  SupportUserActor
} from '../../Actors/support-user-actor';
import { SupportActorDirectory } from '../../Actors/support-actor-directory';
import { SupportEntrySpot } from './support-entry-spot';
import type {
  OpenConversationApiRes,
  OpenConversationReq,
  OpenConversationRes,
  JoinConversationReq,
  JoinConversationRes,
  SetAgentAvailableReq,
  SetAgentAvailableRes
} from '../../../../../../Shared/Contracts/messages';
import type {
  ZLinkChannelClient,
  ZLinkEntrySpotActorRequestHandler,
  ZLinkMessageContext
} from '@zlink-systems/framework';

@zlinkEntrySpotActorRequestHandler({
  actor: () => SupportUserActor,
  entrySpot: () => SupportEntrySpot,
  packetName: PacketNames.setAgentAvailableReq
})
class SetAgentAvailableHandler implements ZLinkEntrySpotActorRequestHandler<SupportEntrySpot, SupportUserActor, SetAgentAvailableReq, SetAgentAvailableRes> {
  constructor(
    private readonly availability: AgentAvailabilityDirectory,
    private readonly directory: SupportActorDirectory
  ) {}

  async handle(_spot: SupportEntrySpot, actor: SupportUserActor, _context: ZLinkMessageContext, request: SetAgentAvailableReq): Promise<SetAgentAvailableRes> {
    const identity = requireIdentity(this.directory, actor.actorId);
    if (identity.role !== SupportChatRoles.Agent || identity.actorId !== identity.participantId) {
      throw new Error('Customer actor must not set agent availability.');
    }
    return { isAvailable: this.availability.setAvailable(actor.actorId, request.isAvailable) };
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => SupportUserActor,
  entrySpot: () => SupportEntrySpot,
  packetName: PacketNames.joinConversationReq
})
class JoinConversationAtEntryHandler implements ZLinkEntrySpotActorRequestHandler<SupportEntrySpot, SupportUserActor, JoinConversationReq, JoinConversationRes> {
  constructor(private readonly directory: SupportActorDirectory) {}

  async handle(
    _spot: SupportEntrySpot,
    actor: SupportUserActor,
    context: ZLinkMessageContext,
    _request: JoinConversationReq
  ): Promise<JoinConversationRes> {
    const conversationId = context.metadata.find(SampleNames.conversationIdMetadataKey);
    if (conversationId === undefined || conversationId.length === 0) {
      throw new Error('Conversation metadata is required for JoinConversationReq.');
    }
    const identity = requireIdentity(this.directory, actor.actorId);
    return actor.scheduleConversationJoin(new JoinSupportConversation(
      conversationId,
      identity.participantId,
      identity.role,
      identity.displayName
    ));
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => SupportUserActor,
  entrySpot: () => SupportEntrySpot,
  packetName: PacketNames.openConversationReq
})
class OpenConversationActorHandler implements ZLinkEntrySpotActorRequestHandler<SupportEntrySpot, SupportUserActor, OpenConversationReq, OpenConversationRes> {
  constructor(
    @Inject(ZLINK_CHANNEL_CLIENT) private readonly channels: ZLinkChannelClient,
    private readonly directory: SupportActorDirectory
  ) {}

  async handle(_spot: SupportEntrySpot, actor: SupportUserActor, _context: ZLinkMessageContext, request: OpenConversationReq): Promise<OpenConversationRes> {
    const identity = requireIdentity(this.directory, actor.actorId);
    if (identity.role !== SupportChatRoles.Customer) {
      throw new Error('Only a customer can open a support conversation.');
    }
    const opened = await this.channels
      .requestToChannel(
        SampleNames.apiChannel,
        openConversationApi(identity.actorId, identity.displayName, request.subject)
      )
      .timeout(SampleTimings.requestTimeout)
      .submit<OpenConversationApiRes>();
    actor.scheduleConversationJoin(new JoinSupportConversation(
      opened.conversationId,
      identity.participantId,
      identity.role,
      identity.displayName
    ));
    return {
      conversationId: opened.conversationId,
      state: {
        conversationId: opened.conversationId,
        subject: request.subject,
        status: opened.status,
        customerActorId: actor.actorId,
        lastMessageSeq: 0
      }
    };
  }
}

function requireIdentity(directory: SupportActorDirectory, actorId: string) {
  const identity = directory.get(actorId);
  if (identity === undefined) throw new Error(`Support actor '${actorId}' identity was not found.`);
  return identity;
}

export {
  JoinConversationAtEntryHandler,
  SetAgentAvailableHandler,
  OpenConversationActorHandler
};
