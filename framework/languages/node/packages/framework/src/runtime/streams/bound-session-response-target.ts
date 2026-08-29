import type { Message } from '../../contracts/Common/Message';
import {
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
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
    submitRaw(payload: Message): Promise<ZLinkSubmitResult>;
  };
}

export class DefaultZLinkBoundSessionResponseTarget implements ZLinkBoundSessionResponseTarget {
  constructor(
    private readonly frameMessages: ZLinkStreamFrameMessageFactory,
    private readonly context: ZLinkBoundSessionResponseContext,
    private readonly actorId: string
  ) {}

  sendResponse(
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    metadata: ReadonlyMap<string, string>
  ): Promise<boolean> {
    return this.send(ZLinkStreamMessageKind.Response, packetName, requestSeq, message, metadata, 'response');
  }

  sendError(
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>
  ): Promise<boolean> {
    return this.send(ZLinkStreamMessageKind.Error, packetName, requestSeq, boundSessionErrorPayload(error), metadata, 'error response');
  }

  private async send(
    kind: ZLinkStreamMessageKind,
    packetName: string,
    requestSeq: bigint,
    payload: unknown,
    metadata: ReadonlyMap<string, string>,
    operationName: string
  ): Promise<boolean> {
    const frame = this.frameMessages.createJsonFrameMessage(kind, packetName, metadata, false, requestSeq, payload);
    try {
      const result = await this.context.stream.submitRaw(frame);
      if (result.status !== ZLinkSubmitStatus.Submitted) {
        throw new Error(`Actor '${this.actorId}' local bound session ${operationName} failed.`);
      }
      return true;
    } finally {
      frame.close();
    }
  }
}

export function boundSessionErrorPayload(error: unknown): { readonly code: string; readonly message: string } {
  return {
    code: error instanceof Error ? error.constructor.name : 'RemoteError',
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
