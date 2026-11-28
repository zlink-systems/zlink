import { ZLinkFrameworkErrorKind, ZLinkFrameworkException } from '../../contracts';
import type { normalizeStreamAuthenticationAdmission } from '../../contracts/Configuration/RegistrationNormalizers';
import {
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  ZLinkRuntimeDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { ZLinkBackendStreamSocket } from '../backend/contracts';
import type { ZLinkDispatchErrorReporter } from '../channels/dispatch-error-reporter';
import type { ZLinkRuntimeMetrics } from '../diagnostics';
import type { ZLinkRuntimeMetricRegistration } from '../diagnostics/runtime-metrics';
import { requireOneWayCompletion } from '../messaging/submission-result';
import { boundSessionErrorPayload } from './bound-session-response-target';
import { streamSessionIdFromRoutingId, submitStreamRaw } from './managed-stream';
import { ZLinkStreamMessageKind, type ZLinkStreamFrameHeader } from './protocol';
import { ZLinkStreamFrameMessageFactory } from './stream-frame-factory';
import { queueRequiredMetadata, type AdmissionUpdate } from './admission-queue-messages';

const queueRejectionMetadata = new Map(Object.entries(queueRequiredMetadata));
const emptyMetadata = new Map<string, string>();

export class ZLinkAuthenticationAdmission {
  private inFlight = 0;
  private rejections = 0;
  private readonly observation: ZLinkRuntimeMetricRegistration | undefined;
  private readonly frames: ZLinkStreamFrameMessageFactory;
  private readonly metadata: ReadonlyMap<string, string>;

  constructor(
    private readonly settings: ReturnType<typeof normalizeStreamAuthenticationAdmission>,
    metrics?: ZLinkRuntimeMetrics,
    private readonly dispatchErrors?: ZLinkDispatchErrorReporter
  ) {
    this.observation = metrics?.registerAuthenticationAdmission(() => ({
      inFlight: this.inFlight,
      rejections: this.rejections
    }));
    this.frames = new ZLinkStreamFrameMessageFactory({
      flowCreationEnabled: () => dispatchErrors?.flow.flowCreationEnabled() ?? true
    });
    this.metadata = new Map([
      ['$zlink.admission.retryAfterMs', String(settings.authenticationRetryDelayMs)]
    ]);
  }

  isAuthentication(header: ZLinkStreamFrameHeader): boolean {
    return (
      header.kind === ZLinkStreamMessageKind.Request &&
      header.name === this.settings.authenticationPacketName
    );
  }

  get activeCount(): number {
    return this.inFlight;
  }

  tryAcquire(): (() => void) | undefined {
    if (this.inFlight >= this.settings.maxConcurrentAuthentications) {
      this.rejections += 1;
      return undefined;
    }
    this.inFlight += 1;
    // Each lease returns once, whichever terminal calls it first.
    let released = false;
    return () => {
      if (released) return;
      released = true;
      this.inFlight -= 1;
    };
  }

  async replyRejected(
    socket: ZLinkBackendStreamSocket,
    routingId: unknown,
    header: ZLinkStreamFrameHeader,
    reason: 'localLimit' | 'queueRequired' | 'unavailable' = 'localLimit'
  ): Promise<void> {
    const error = new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.Unavailable,
      reason === 'localLimit'
        ? 'Authentication admission is full.'
        : 'Authentication admission is unavailable.'
    );
    if (this.dispatchErrors?.captureEnabled() === true) {
      this.dispatchErrors.report({
        surface: ZLinkDispatchErrorSurface.StreamSession,
        messageKind: ZLinkDispatchMessageKind.Request,
        reason: ZLinkRuntimeDispatchErrorReason.Backpressure,
        action: ZLinkRuntimeDispatchErrorAction.ReplyError,
        packetName: header.name,
        sourceRid: streamSessionIdFromRoutingId(routingId),
        correlationId: header.correlationId,
        flowId: header.flowId,
        flowOrigin: header.flowOrigin,
        error
      });
    }
    const frame = this.frames.createJsonReplyFrameMessage(
      { ...header, actorSlot: undefined },
      ZLinkStreamMessageKind.Error,
      reason === 'localLimit'
        ? this.metadata
        : reason === 'queueRequired'
          ? queueRejectionMetadata
          : emptyMetadata,
      false,
      boundSessionErrorPayload(error)
    );
    await this.submitFrame(socket, routingId, frame);
  }

  async replyEnqueue(
    socket: ZLinkBackendStreamSocket,
    routingId: unknown,
    header: ZLinkStreamFrameHeader,
    update: AdmissionUpdate
  ): Promise<void> {
    const frame = this.frames.createJsonReplyFrameMessage(
      { ...header, actorSlot: undefined },
      ZLinkStreamMessageKind.Response,
      emptyMetadata,
      false,
      update
    );
    await this.submitFrame(socket, routingId, frame);
  }

  async push(
    socket: ZLinkBackendStreamSocket,
    routingId: unknown,
    name: string,
    update: AdmissionUpdate
  ): Promise<void> {
    const frame = this.frames.createJsonFrameMessage(
      ZLinkStreamMessageKind.Send,
      name,
      emptyMetadata,
      false,
      undefined,
      update
    );
    await this.submitFrame(socket, routingId, frame);
  }

  private async submitFrame(
    socket: ZLinkBackendStreamSocket,
    routingId: unknown,
    frame: ReturnType<ZLinkStreamFrameMessageFactory['createJsonReplyFrameMessage']>
  ): Promise<void> {
    try {
      requireOneWayCompletion(
        await submitStreamRaw(socket, routingId, frame),
        'STREAM authentication admission frame'
      );
    } finally {
      frame.close();
    }
  }

  resetMetrics(): void {
    this.rejections = 0;
  }

  dispose(): void {
    this.observation?.dispose();
  }
}
