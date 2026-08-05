import { Inject, Injectable } from '@nestjs/common';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  type ZLinkChannelClient,
  type ZLinkMessageContext,
  type ZLinkRequestHandler,
  type ZLinkRouteClient,
  type ZLinkSendHandler
} from '@zlink-systems/framework';
import { ZLINK_CHANNEL_CLIENT, ZLINK_ROUTE_CLIENT } from '@zlink-systems/nestjs';
import { ChannelEgressNames, ChannelProbeMsg, ChannelProbeReq } from '../../Shared/messages';
import { EvidenceStore } from '../Support/evidence-store';
import { RoleState } from '../Support/role-state';

@Injectable()
export class ChannelProbeRequestHandler implements ZLinkRequestHandler<ChannelProbeReq, object> {
  constructor(
    private readonly evidence: EvidenceStore,
    private readonly state: RoleState,
    @Inject(ZLINK_CHANNEL_CLIENT) private readonly channels: ZLinkChannelClient,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly routes: ZLinkRouteClient
  ) {}

  async handle(request: ChannelProbeReq, context: ZLinkMessageContext): Promise<object> {
    const channel = context.channelName ?? '<none>';
    this.evidence.add(`request-start|role=${this.evidence.role}|channel=${channel}|id=${request.id}`);
    if (this.evidence.role.startsWith('workflow') && request.mode === 'hold') {
      this.state.hold();
      this.evidence.add(`request-held|role=${this.evidence.role}|id=${request.id}`);
      await this.state.waitUntilReleased();
    }

    const downstream: string[] = [];
    if (this.evidence.role === 'play' && channel === ChannelEgressNames.play && request.mode === 'cascade') {
      const audit = await this.routes
        .requestToChannel(ChannelEgressNames.audit, new ChannelProbeReq(`${request.id}-audit`))
        .timeout(5000)
        .submit<ChannelProbeReply>();
      downstream.push(`${audit.role}:${audit.channel}`);
      const workflow = await this.channels
        .requestToChannel(ChannelEgressNames.workflow, new ChannelProbeReq(`${request.id}-workflow`))
        .timeout(5000)
        .submit<ChannelProbeReply>();
      downstream.push(`${workflow.role}:${workflow.channel}`);
    }

    this.evidence.add(`request-end|role=${this.evidence.role}|channel=${channel}|id=${request.id}`);
    return {
      id: request.id,
      role: this.evidence.role.startsWith('workflow') ? this.evidence.rid : this.evidence.role,
      lifecycle: this.evidence.instanceMarker,
      channel,
      downstream
    } satisfies ChannelProbeReply;
  }
}

@Injectable()
export class ChannelProbeSendHandler implements ZLinkSendHandler<ChannelProbeMsg> {
  constructor(
    private readonly evidence: EvidenceStore
  ) {}

  async handle(message: ChannelProbeMsg, context: ZLinkMessageContext): Promise<void> {
    this.evidence.add(`send|role=${this.evidence.role}|channel=${context.channelName ?? '<none>'}|id=${message.id}`);
  }
}

export function publicErrorKind(error: unknown): string {
  if (error instanceof ZLinkFrameworkException) {
    return ZLinkFrameworkErrorKind[error.kind] ?? String(error.kind);
  }
  return error instanceof Error ? error.name : String(error);
}

interface ChannelProbeReply {
  readonly id: string;
  readonly role: string;
  readonly lifecycle: string;
  readonly channel: string;
  readonly downstream: readonly string[];
}
