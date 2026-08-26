import type {
  ZLinkMessage,
  ZLinkMessageSerializer,
  ZLinkSessionActor
} from '../../contracts';
import {
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import type { Message } from '../../contracts/Common/Message';
import { encodeFrameworkPayloadMessage } from '../messaging/payload-codec';
import {
  encodeStreamHeader,
  messageToBytes,
  type ZLinkStreamFrameHeader,
  ZLinkStreamMessageKind
} from './protocol';
import { throwIfAborted } from '../abort';
import { captureZLinkExecutionTurn } from '../execution';
import {
  ZLinkActorSessionBindingRegistry
} from './actor-session-binding-registry';
import { ZLinkActorSessionLifecycleCoordinator } from './actor-session-lifecycle-coordinator';
import { ZLinkManagedStream } from './managed-stream';
import {
  DefaultZLinkSessionActor,
  DefaultZLinkSessionContext
} from './session-context';
import {
  ZLinkStreamFrameMessageFactory
} from './stream-frame-factory';

export interface ZLinkBoundActorRelaySenderOptions {
  readonly actorBindTimeoutMs?: number;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly relay?: (actor: ZLinkSessionActor, header: ZLinkStreamFrameHeader, payload: Message, signal?: AbortSignal) => Promise<boolean>;
  readonly notifyDisconnected?: (actor: ZLinkSessionActor, signal?: AbortSignal) => Promise<void>;
}

export class ZLinkBoundActorRelaySender {
  constructor(
    private readonly routes: ZLinkActorSessionBindingRegistry<DefaultZLinkSessionContext, DefaultZLinkSessionActor>,
    private readonly frameMessages: ZLinkStreamFrameMessageFactory,
    private readonly options: ZLinkBoundActorRelaySenderOptions = {},
    private readonly lifecycle = new ZLinkActorSessionLifecycleCoordinator()
  ) {}

  async relay(
    actor: DefaultZLinkSessionActor,
    payload: ZLinkMessage,
    signal?: AbortSignal,
    dispatchHeader?: ZLinkStreamFrameHeader
  ): Promise<ZLinkSubmitResult> {
    const header = dispatchHeader === undefined
      ? await this.currentHeader(actor)
      : await this.requireDispatchHeader(actor, dispatchHeader);
    if (
      header.kind === ZLinkStreamMessageKind.Request
      && header.requestSeq !== undefined
    ) {
      const operation = async (): Promise<ZLinkSubmitResult> => {
        const admission = await this.routes.beginAcceptedRequestFrameWhenReady(
          actor.actorId,
          actor.bindingToken,
          signal
        );
        try {
          return await this.relayAcceptedFrame(actor, payload, signal, header, admission);
        } finally {
          await admission.complete();
        }
      };
      if (await this.routes.isSealed(actor.actorId) && captureZLinkExecutionTurn() !== undefined) {
        void operation().catch(() => undefined);
        return { status: ZLinkSubmitStatus.Submitted };
      }
      return await operation();
    }
    if (await this.routes.isSealed(actor.actorId) && captureZLinkExecutionTurn() !== undefined) {
      //  Spec 32:62 — a Send completes once the source outbound queue
      //  accepts it; spec 05 — infrastructure send progress must not depend
      //  on the application turn. When the CALLER IS an application turn
      //  (a lifecycle callback pushing to a bound actor), awaiting the seal
      //  release here deadlocks: the relocation seal waits for that very
      //  callback to finish (onJoinedActor -> push -> seal ->
      //  onJoinedActor). Accept the frame now and deliver it after the
      //  seal releases, in per-actor order through the lifecycle lane; the
      //  bounded seal wait converts a stuck seal into a typed failure.
      //  Non-turn callers (stream ingress relays) keep the waiting
      //  semantics so a held frame across a failed rebind still
      //  terminalizes to the caller.
      void this.routes.runAcceptedFrameWhenReady(
        actor.actorId,
        actor.bindingToken,
        () => this.relayAcceptedFrame(actor, payload, signal),
        signal
      ).catch(() => undefined);
      return { status: ZLinkSubmitStatus.Submitted };
    }
    return this.routes.runAcceptedFrameWhenReady(
      actor.actorId,
      actor.bindingToken,
      () => this.relayAcceptedFrame(actor, payload, signal),
      signal
    );
  }

  private async relayAcceptedFrame(
    actor: DefaultZLinkSessionActor,
    payload: ZLinkMessage,
    signal?: AbortSignal,
    header?: ZLinkStreamFrameHeader,
    requestAdmission?: { beginSubmission(signal?: AbortSignal): Promise<void> | undefined }
  ): Promise<ZLinkSubmitResult> {
    const activeHeader = header ?? await this.currentHeader(actor);
    const started = await this.lifecycle.run(actor.actorId, async () => {
      const completion = this.relayInsideLifecycle(
        actor,
        payload,
        signal,
        activeHeader,
        requestAdmission
      );
      if (
        activeHeader.kind === ZLinkStreamMessageKind.Request
        && activeHeader.requestSeq !== undefined
      ) {
        // A routed REQUEST only needs this lane while its ordered submission
        // is started. Its admission acknowledgement can depend on relocation
        // route publication, and command 44 uses the same lane. Keeping the
        // lane until that acknowledgement arrives creates:
        // REQUEST -> lane -> command 44 -> seal -> active REQUEST.
        // The request admission remains observable outside the lane, so
        // command 42 can capture its Session frame while the one original
        // submission continues to its detached terminal.
        return { kind: 'completion' as const, completion };
      }
      return { kind: 'result' as const, result: await completion };
    });
    return started.kind === 'result' ? started.result : await started.completion;
  }

  private async currentHeader(actor: DefaultZLinkSessionActor): Promise<ZLinkStreamFrameHeader> {
    await this.routes.requireCurrentToken(actor.actorId, actor.bindingToken);
    const header = (await this.routes.requireRoute(actor.actorId)).context.dispatchHeader;
    if (header === undefined) {
      throw new Error('Session actor relay requires an active stream dispatch.');
    }
    return header;
  }

  private async requireDispatchHeader(
    actor: DefaultZLinkSessionActor,
    header: ZLinkStreamFrameHeader
  ): Promise<ZLinkStreamFrameHeader> {
    await this.routes.requireCurrentToken(actor.actorId, actor.bindingToken);
    if ((await this.routes.requireRoute(actor.actorId)).context.dispatchHeader !== header) {
      throw new Error('Session actor relay dispatch is not active for this bound actor.');
    }
    return header;
  }

  private async relayInsideLifecycle(
    actor: DefaultZLinkSessionActor,
    payload: ZLinkMessage,
    signal: AbortSignal | undefined,
    currentHeader?: ZLinkStreamFrameHeader,
    requestAdmission?: { beginSubmission(signal?: AbortSignal): Promise<void> | undefined }
  ): Promise<ZLinkSubmitResult> {
    const header = currentHeader ?? await this.currentHeader(actor);
    const held = requestAdmission?.beginSubmission(signal);
    if (held !== undefined) await held;
    throwIfAborted(signal);
    const payloadMessage = encodeFrameworkPayloadMessage(payload, this.options.messageSerializers);
    try {
      if (this.options.relay !== undefined) {
        const handled = await this.options.relay(actor, header, payloadMessage, signal);
        if (handled) {
          return { status: ZLinkSubmitStatus.Submitted };
        }
      }
      const route = await this.routes.requireRoute(actor.actorId);
      if (!(route.context.stream instanceof ZLinkManagedStream)) {
        return { status: ZLinkSubmitStatus.TargetNotFound };
      }
      const headerMessage = this.frameMessages.createBinaryMessage(encodeStreamHeader(header));
      const framePayloadMessage = this.frameMessages.createBinaryMessage(messageToBytes(payloadMessage));
      try {
        return await route.context.stream.submitBoundActor(
          actor.actorId,
          [headerMessage, framePayloadMessage],
          signal
        );
      } finally {
        headerMessage.close();
        framePayloadMessage.close();
      }
    } finally {
      payloadMessage.close();
    }
  }

  async notifyDisconnected(actor: DefaultZLinkSessionActor, signal?: AbortSignal): Promise<void> {
    await this.notifyDisconnectedCore(actor, true, signal);
  }

  private async notifyDisconnectedCore(
    actor: DefaultZLinkSessionActor,
    unbindNative: boolean,
    signal?: AbortSignal
  ): Promise<void> {
    const detached = await this.lifecycle.run(actor.actorId, async () => {
      await this.routes.requireCurrentToken(actor.actorId, actor.bindingToken);
      const route = await this.routes.requireRoute(actor.actorId);
      await this.routes.unbind(actor.actorId, route.context, actor.bindingToken);
      return route;
    });
    if (
      unbindNative
      && detached.bindingToken === actor.bindingToken
      && detached.context.stream instanceof ZLinkManagedStream
    ) {
      await detached.context.stream.unbindActor(
        actor.actorId,
        this.options.actorBindTimeoutMs ?? 2000,
        signal
      );
    }
    await this.options.notifyDisconnected?.(actor, signal);
  }

  async notifyPhysicalDisconnect(context: DefaultZLinkSessionContext): Promise<void> {
    const snapshot = [...context.boundActors];
    const timeoutMs = this.options.actorBindTimeoutMs ?? 2000;
    await Promise.allSettled(
      snapshot.map(async (actor) => {
        const detached = await this.lifecycle.run(actor.actorId, async () => {
          const route = await this.routes.route(actor.actorId);
          if (
            route === undefined
            || route.context !== context
            || route.actor !== actor
            || route.bindingToken !== actor.bindingToken
          ) {
            return false;
          }
          // The transport is already closed, so remove its exact route before
          // invoking application lifecycle code. A reconnect can then install
          // a successor while this best-effort notification is in flight.
          await this.routes.unbind(actor.actorId, context, actor.bindingToken);
          return true;
        });
        if (!detached) return;
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), timeoutMs);
        try {
          await Promise.race([
            this.options.notifyDisconnected?.(actor, controller.signal) ?? Promise.resolve(),
            new Promise<never>((_, reject) => {
              controller.signal.addEventListener(
                'abort',
                () => reject(new Error(
                  `Actor '${actor.actorId}' disconnect notification exceeded ${timeoutMs} ms.`
                )),
                { once: true }
              );
            })
          ]);
        } finally {
          clearTimeout(timer);
        }
      })
    );
  }
}
