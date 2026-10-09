import type {
  ZLinkBackendContext,
  ZLinkBackendMeshNode,
  ZLinkMeshBackendAdapter
} from '../contracts';
import { ZLinkNodeRawMeshBackend } from './node-raw-mesh-backend';
import { ZLinkNodeRawBindingPort } from './node-raw-binding-port';
import type { Context } from '@zlink-systems/zlink';

export class ZLinkNodeMeshBackendAdapter implements ZLinkMeshBackendAdapter {
  createMeshNode(
    context: ZLinkBackendContext,
    options: Parameters<ZLinkMeshBackendAdapter['createMeshNode']>[1]
  ): ZLinkBackendMeshNode {
    if (options.trustProfile !== undefined) {
      throw new Error(
        'M6A raw MeshNode trust profiles require the service admission security runtime.'
      );
    }
    const bindingContext = context.nativeInstance;
    if (!isBindingContext(bindingContext)) {
      throw new TypeError(
        'Node MeshNode creation requires the Node binding Context owned by the host.'
      );
    }
    return new ZLinkNodeRawMeshBackend(
      options.meshName,
      options.routingId,
      new ZLinkNodeRawBindingPort(bindingContext, options.receiveTimeoutMs),
      options.applicationJobQueue,
      options.applicationJobReceiveFlowFailureSink,
      options.peerAdmissionSealed,
      options.infrastructureTaskRunner
    );
  }
}

function isBindingContext(value: unknown): value is Context {
  return (
    typeof value === 'object' &&
    value !== null &&
    typeof (value as { shutdown?: unknown }).shutdown === 'function' &&
    typeof (value as { close?: unknown }).close === 'function'
  );
}
