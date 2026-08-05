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
  type ZLinkStreamFrameHeader
} from './protocol';
import { throwIfAborted } from '../abort';
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
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    // A relocation seal can hold this request until the target route is
    // published. Wait before entering the actor lifecycle lane: route
    // publication uses the same lane and must be able to release the seal.
    await this.routes.acceptWhenReady(actor.actorId, actor.bindingToken, signal);
    return await this.lifecycle.run(actor.actorId, () => this.relayInsideLifecycle(actor, payload, signal));
  }

  private async relayInsideLifecycle(
    actor: DefaultZLinkSessionActor,
    payload: ZLinkMessage,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    this.routes.requireCurrentToken(actor.actorId, actor.bindingToken);
    const currentHeader = this.routes.requireRoute(actor.actorId).context.dispatchHeader;
    if (currentHeader === undefined) {
      throw new Error('Session actor relay requires an active stream dispatch.');
    }
    throwIfAborted(signal);
    const payloadMessage = encodeFrameworkPayloadMessage(payload, this.options.messageSerializers);
    try {
      if (this.options.relay !== undefined) {
        const handled = await this.options.relay(actor, currentHeader, payloadMessage, signal);
        if (handled) {
          return { status: ZLinkSubmitStatus.Submitted };
        }
      }
      const route = this.routes.requireRoute(actor.actorId);
      if (!(route.context.stream instanceof ZLinkManagedStream)) {
        return { status: ZLinkSubmitStatus.TargetNotFound };
      }
      const headerMessage = this.frameMessages.createBinaryMessage(encodeStreamHeader(currentHeader));
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
    await this.lifecycle.run(actor.actorId, async () => {
      this.routes.requireCurrentToken(actor.actorId, actor.bindingToken);
      const route = this.routes.requireRoute(actor.actorId);
      try {
        if (
          unbindNative
          &&
          route.bindingToken === actor.bindingToken
          && route.context.stream instanceof ZLinkManagedStream
        ) {
          await route.context.stream.unbindActor(
            actor.actorId,
            this.options.actorBindTimeoutMs ?? 2000,
            signal
          );
        }
        await this.options.notifyDisconnected?.(actor, signal);
      } finally {
        this.routes.unbind(actor.actorId, route.context, actor.bindingToken);
      }
    });
  }

  async notifyPhysicalDisconnect(context: DefaultZLinkSessionContext): Promise<void> {
    const snapshot = [...context.boundActors];
    const timeoutMs = this.options.actorBindTimeoutMs ?? 2000;
    await Promise.allSettled(
      snapshot.map(async (actor) => {
        const route = this.routes.route(actor.actorId);
        if (
          route === undefined
          || route.context !== context
          || route.actor !== actor
          || route.bindingToken !== actor.bindingToken
        ) {
          return;
        }
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), timeoutMs);
        try {
          await Promise.race([
            this.notifyDisconnectedCore(actor, false, controller.signal),
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
          const current = this.routes.route(actor.actorId);
          if (
            current !== undefined
            && current.context === context
            && current.actor === actor
            && current.bindingToken === actor.bindingToken
          ) {
            this.routes.unbind(actor.actorId, context, actor.bindingToken);
          }
        }
      })
    );
  }
}
