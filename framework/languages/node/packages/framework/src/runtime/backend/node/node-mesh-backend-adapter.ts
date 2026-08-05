import type {
  ZLinkBackendContext,
  ZLinkBackendMeshNode,
  ZLinkMeshBackendAdapter
} from '../contracts';
import { ZLinkNodeRawMeshBackend } from './node-raw-mesh-backend';

export class ZLinkNodeMeshBackendAdapter implements ZLinkMeshBackendAdapter {
  createMeshNode(
    context: ZLinkBackendContext,
    options: {
      readonly meshName: string;
      readonly routingId?: string;
      readonly trustProfile?: string;
    }
  ): ZLinkBackendMeshNode {
    void context;
    if (options.trustProfile !== undefined) {
      throw new Error('M6A raw MeshNode trust profiles require the service admission security runtime.');
    }
    return new ZLinkNodeRawMeshBackend(options.meshName, options.routingId);
  }
}
