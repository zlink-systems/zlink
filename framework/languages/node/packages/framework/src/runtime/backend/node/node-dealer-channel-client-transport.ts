import type {
  DealerSocket,
  Message,
  PubSocket
} from '@zlink-systems/zlink';
import { ZLinkConfigurationException } from '../../configuration';
import {
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../../messaging/submission-result';
import {
  decodeChannelReply,
  encodeChannelEnvelopeParts,
  ZLinkChannelMessageKind
} from '../../channels/channel-envelope';
import { throwIfAborted } from '../../abort';
import {
  appendParts,
  submitRequestOperation
} from '../../channels/channel-multipart';
import type { ZLinkChannelClientTransport } from '../../channels/channel-transports';
import { runWithOutboundFlow } from '../../diagnostics/flow-context';

const EMPTY_METADATA: ReadonlyMap<string, string> = new Map();

/** Adapter used by direct binding-socket integration and its contract tests. */
export class ZLinkDealerChannelClientTransport implements ZLinkChannelClientTransport {
  constructor(
    private readonly dealer: DealerSocket,
    private readonly publisher?: PubSocket
  ) {}

  async send(
    channelName: string,
    packetName: string | undefined,
    message: Message,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = EMPTY_METADATA
  ): Promise<ZLinkSubmitResult> {
    throwIfAborted(signal);
    // Call-scoped flow (spec 27 §4): the envelope flow pair does not outlive
    // this outbound call.
    await runWithOutboundFlow(true, () => appendParts(
      this.dealer.send(),
      encodeChannelEnvelopeParts(
        ZLinkChannelMessageKind.Command,
        channelName,
        packetName,
        message,
        undefined,
        undefined,
        undefined,
        undefined,
        true,
        metadata
      )
    ).submit());
    return { status: ZLinkSubmitStatus.Submitted };
  }

  async request<TReply>(
    channelName: string,
    packetName: string | undefined,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = EMPTY_METADATA
  ): Promise<TReply> {
    throwIfAborted(signal);
    const operation = runWithOutboundFlow(true, () => appendParts(
      this.dealer.request(),
      encodeChannelEnvelopeParts(
        ZLinkChannelMessageKind.Request,
        channelName,
        packetName,
        request,
        timeoutMs,
        undefined,
        undefined,
        undefined,
        true,
        metadata
      )
    ));
    if (timeoutMs !== undefined) operation.timeout(timeoutMs);
    const reply = await submitRequestOperation(operation, 'channel request');
    try {
      return decodeChannelReply<TReply>(reply);
    } finally {
      for (const part of reply) part.close();
    }
  }

  tryPublish(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: Message,
    metadata: ReadonlyMap<string, string> = EMPTY_METADATA
  ): ZLinkSubmitResult {
    if (this.publisher === undefined) {
      throw new ZLinkConfigurationException('Channel publisher runtime is not started.');
    }
    const accepted = runWithOutboundFlow(true, () => appendParts(
      this.publisher!.publish(topic),
      encodeChannelEnvelopeParts(
        ZLinkChannelMessageKind.Publish,
        channelName,
        packetName,
        event,
        undefined,
        topic,
        undefined,
        undefined,
        true,
        metadata
      )
    ).submit());
    return {
      status: accepted === false ? ZLinkSubmitStatus.Backpressured : ZLinkSubmitStatus.Submitted
    };
  }

  async publish(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: Message,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = EMPTY_METADATA
  ): Promise<ZLinkSubmitResult> {
    throwIfAborted(signal);
    return this.tryPublish(channelName, topic, packetName, event, metadata);
  }
}
