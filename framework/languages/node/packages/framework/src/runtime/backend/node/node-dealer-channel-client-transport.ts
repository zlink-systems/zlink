import { RequestRuntime } from '../../foundation/request-runtime';
import {
  prepareZLinkRequestReply,
  identityRequestReply,
  rejectZLinkRequestReply,
  type ZLinkRequestResult
} from '../../messaging/request-result';
import { bindingRequestReply } from './node-backend-adapter-support';
import type { DealerSocket, Message, PubSocket } from '@zlink-systems/zlink';
import { ZLinkConfigurationException } from '../../configuration';
import { ZLinkSubmitStatus, type ZLinkSubmitResult } from '../../messaging/submission-result';
import {
  decodeChannelReply,
  encodeChannelEnvelopeParts,
  ZLinkChannelMessageKind
} from '../../channels/channel-envelope';
import { throwIfAborted } from '../../abort';
import { appendParts, mapRequestFailure } from '../../channels/channel-multipart';
import type { ZLinkChannelClientTransport } from '../../channels/channel-transports';
import { runWithOutboundFlow } from '../../diagnostics/flow-context';

const EMPTY_METADATA: ReadonlyMap<string, string> = new Map();

/** Adapter used by direct binding-socket integration and its contract tests. */
export class ZLinkDealerChannelClientTransport implements ZLinkChannelClientTransport {
  constructor(
    private readonly dealer: DealerSocket,
    private readonly publisher?: PubSocket,
    private readonly requests = new RequestRuntime()
  ) {}

  async send(
    channelName: string,
    packetName: string | undefined,
    message: Message,
    metadata: ReadonlyMap<string, string> = EMPTY_METADATA
  ): Promise<ZLinkSubmitResult> {
    // Call-scoped flow (spec 27 §4): the envelope flow pair does not outlive
    // this outbound call.
    await runWithOutboundFlow(
      true,
      () =>
        appendParts(
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
        ).submit().admitted
    );
    return { status: ZLinkSubmitStatus.Submitted };
  }

  request<TReply>(
    channelName: string,
    packetName: string | undefined,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = EMPTY_METADATA,
    onAdmitted?: () => void
  ): ZLinkRequestResult<TReply> {
    try {
      throwIfAborted(signal);
    } catch (error) {
      return rejectZLinkRequestReply(error);
    }
    const requestOperation = this.requests.begin(timeoutMs, signal);
    try {
      throwIfAborted(signal);
      const operation = runWithOutboundFlow(true, () =>
        appendParts(
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
        )
      );
      if (timeoutMs !== undefined) operation.timeout(timeoutMs);
      const decode = (raw: unknown): TReply => {
        const reply = raw as readonly Message[];
        try {
          return decodeChannelReply<TReply>(reply);
        } finally {
          closeReply(reply);
        }
      };
      requestOperation.mapCaller(decode, identityRequestReply);
      if (requestOperation.isPending()) {
        const receiver = {
          operationCompleted: (_id: bigint, raw: unknown) => {
            if (requestOperation.complete(raw)) return;
            try {
              requestOperation.resolveCaller(decode(raw));
            } catch (error) {
              requestOperation.rejectCaller(error);
            }
          },
          operationFailed: (_id: bigint, error: unknown) => {
            let failure = error;
            try {
              failure = mapRequestFailure(error, 'channel request');
            } catch (mapped) {
              failure = mapped;
            }
            requestOperation.fail(failure);
          }
        };
        try {
          bindingRequestReply(
            operation.submit(),
            receiver,
            requestOperation,
            undefined,
            onAdmitted
          );
        } catch (error) {
          receiver.operationFailed(requestOperation.id, error);
        }
      }
      return {
        pending: requestOperation.promise,
        decode: identityRequestReply<TReply>,
        mapFailure: identityRequestReply,
        operation:
          requestOperation as import('../../foundation/operation-registry').PendingOperation<TReply>
      };
    } catch (error) {
      requestOperation.fail(error);
      return prepareZLinkRequestReply(
        requestOperation.promise as Promise<TReply>,
        identityRequestReply
      );
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
    runWithOutboundFlow(true, () =>
      appendParts(
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
      ).submit()
    );
    return { status: ZLinkSubmitStatus.Submitted };
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

function closeReply(parts: readonly { close(): void }[]): void {
  for (const part of parts) part.close();
}
