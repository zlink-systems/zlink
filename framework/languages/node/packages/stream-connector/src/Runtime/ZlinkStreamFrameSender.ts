import type {
  ZlinkStreamConnection,
  ZlinkStreamEncodedPayload,
  ZlinkStreamFlow,
  ZlinkStreamMetadata,
  ZlinkStreamMessageKind
} from '../Contracts';
import type { ZlinkStreamFrameProtocol } from './Protocol/ZlinkStreamFrameProtocol';
import { throwIfAborted } from './ZlinkStreamSupport';
import type { ZlinkFlowContext } from './ZlinkFlowContext';
import type { ZlinkStreamRuntimeMetrics } from './ZlinkStreamRuntimeMetrics';

export class ZlinkStreamFrameSender {
  private readonly pendingWrites = new Set<Promise<void>>();

  constructor(
    private readonly protocol: ZlinkStreamFrameProtocol,
    private readonly flowContext: ZlinkFlowContext,
    private readonly metrics: ZlinkStreamRuntimeMetrics
  ) {}

  async send(
    connection: ZlinkStreamConnection,
    kind: ZlinkStreamMessageKind,
    name: string,
    payload: ZlinkStreamEncodedPayload,
    metadata: ZlinkStreamMetadata,
    compress: boolean,
    requestSeq: bigint | undefined,
    signal?: AbortSignal,
    correlationId?: string,
    explicitFlow?: ZlinkStreamFlow
  ): Promise<void> {
    throwIfAborted(signal);
    const flow = this.flowContext.currentOrCreate(explicitFlow);
    await this.write(
      connection,
      this.protocol.encode(kind, name, payload, metadata, compress, requestSeq, correlationId, flow.flowId, flow.flowOrigin),
      signal
    );
  }

  async sendControl(
    connection: ZlinkStreamConnection,
    name: string,
    signal?: AbortSignal
  ): Promise<void> {
    await this.write(connection, this.protocol.encodeControl(name), signal);
  }

  async drain(signal?: AbortSignal): Promise<void> {
    while (this.pendingWrites.size > 0) {
      throwIfAborted(signal);
      await Promise.allSettled([...this.pendingWrites]);
    }
  }

  private async write(
    connection: ZlinkStreamConnection,
    frame: Uint8Array,
    signal?: AbortSignal
  ): Promise<void> {
    const write = connection.write(frame, signal);
    this.pendingWrites.add(write);
    try {
      await write;
      this.metrics.outbound(frame.byteLength);
    } finally {
      this.pendingWrites.delete(write);
    }
  }
}
