import type { Message } from '../../contracts/Common/Message';
import { ZLinkFrameworkErrorKind, ZLinkFrameworkException } from '../../contracts';
import { channelErrorCodeName } from '../channels/channel-envelope';
import { ZLinkRouteDisconnectedError } from '../channels/route-disconnected-error';
import { ZLinkSubmitStatus, type ZLinkSubmitResult } from '../messaging/submission-result';
import { ZLinkStreamMessageKind } from './protocol';
import type { ZLinkStreamFrameMessageFactory } from './stream-frame-factory';

export interface ZLinkBoundSessionResponseTarget {
  sendResponse(
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    metadata: ReadonlyMap<string, string>
  ): Promise<boolean>;
  sendError(
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>
  ): Promise<boolean>;
}

export interface ZLinkBoundSessionResponseContext {
  readonly stream: {
    captureBoundSessionSubmission?(
      actorId: string
    ): (payload: Message) => Promise<ZLinkSubmitResult>;
    submitRaw(payload: Message): Promise<ZLinkSubmitResult>;
  };
}

export function captureBoundSessionSubmission(
  context: ZLinkBoundSessionResponseContext,
  actorId: string
): (payload: Message) => Promise<ZLinkSubmitResult> {
  return (
    context.stream.captureBoundSessionSubmission?.(actorId) ??
    ((payload) => context.stream.submitRaw(payload))
  );
}

export class DefaultZLinkBoundSessionResponseTarget implements ZLinkBoundSessionResponseTarget {
  private readonly submit: (payload: Message) => Promise<ZLinkSubmitResult>;
  constructor(
    private readonly frameMessages: ZLinkStreamFrameMessageFactory,
    context: ZLinkBoundSessionResponseContext,
    private readonly actorId: string
  ) {
    this.submit = captureBoundSessionSubmission(context, actorId);
  }

  sendResponse(
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    metadata: ReadonlyMap<string, string>
  ): Promise<boolean> {
    return this.send(
      ZLinkStreamMessageKind.Response,
      packetName,
      requestSeq,
      message,
      metadata,
      'response'
    );
  }

  sendError(
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>
  ): Promise<boolean> {
    return this.send(
      ZLinkStreamMessageKind.Error,
      packetName,
      requestSeq,
      boundSessionErrorPayload(error),
      metadata,
      'error response'
    );
  }

  private async send(
    kind: ZLinkStreamMessageKind,
    packetName: string,
    requestSeq: bigint,
    payload: unknown,
    metadata: ReadonlyMap<string, string>,
    operationName: string
  ): Promise<boolean> {
    const frame = this.frameMessages.createJsonFrameMessage(
      kind,
      packetName,
      metadata,
      false,
      requestSeq,
      payload
    );
    try {
      const result = await this.submit(frame);
      if (result.status !== ZLinkSubmitStatus.Submitted) {
        throw new Error(`Actor '${this.actorId}' local bound session ${operationName} failed.`);
      }
      return true;
    } finally {
      frame.close();
    }
  }
}

export function boundSessionErrorPayload(error: unknown): {
  readonly code: string;
  readonly message: string;
} {
  return {
    code: channelErrorCodeName(
      error instanceof ZLinkFrameworkException
        ? error.kind
        : error instanceof ZLinkRouteDisconnectedError
          ? ZLinkFrameworkErrorKind.Unavailable
          : ZLinkFrameworkErrorKind.InternalFailure
    ),
    message: errorMessage(error)
  };
}

function errorMessage(error: unknown): string {
  if (error instanceof Error) return error.message;
  if (typeof error === 'object' && error !== null) {
    const message = (error as { message?: unknown }).message;
    if (typeof message === 'string') return message;
    if (message !== undefined) return errorMessage(message);
  }
  return String(error);
}
