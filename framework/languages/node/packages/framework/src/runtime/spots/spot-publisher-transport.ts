import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type { Message } from '../../contracts/Common/Message';
import {
  ZLinkFrameworkException
} from '../../contracts';
import type { ZLinkSpotPublisherClientTransport } from '../channels';
import type { ZLinkSubmitResult } from '../messaging/submission-result';

interface ZLinkRuntimeSpotPublisherManager {
  tryPublish(
    meshName: string,
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: Message,
    metadata?: ReadonlyMap<string, string>
  ): ZLinkSubmitResult;
  publish(
    meshName: string,
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: Message,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult>;
}

export class ZLinkRuntimeSpotPublisherTransport implements ZLinkSpotPublisherClientTransport {
  constructor(private readonly manager: () => ZLinkRuntimeSpotPublisherManager | undefined) {}

  tryPublish(
    meshName: string,
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: Message,
    metadata?: ReadonlyMap<string, string>
  ): ZLinkSubmitResult {
    const manager = this.manager();
    if (manager === undefined) {
      throw publisherRuntimeShutdown();
    }
    return manager.tryPublish(meshName, channelName, topic, packetName, event, metadata);
  }

  publish(
    meshName: string,
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: Message,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult> {
    const manager = this.manager();
    if (manager === undefined) {
      throw publisherRuntimeShutdown();
    }
    return manager.publish(meshName, channelName, topic, packetName, event, signal, metadata);
  }
}

function publisherRuntimeShutdown(): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
    'SPOT publisher runtime is not available because the runtime is not started or is shutting down.'
  );
}
