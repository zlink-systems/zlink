import type {
  ZlinkStreamConnection,
  ZlinkStreamEncodedPayload,
  ZlinkStreamMetadata,
  ZlinkStreamMessageKind
} from '../Contracts';
import type { ZlinkStreamFrameProtocol } from './Protocol/ZlinkStreamFrameProtocol';
import { throwIfAborted } from './ZlinkStreamSupport';

export class ZlinkStreamFrameSender {
  private readonly pendingWrites = new Set<Promise<void>>();

  constructor(private readonly protocol: ZlinkStreamFrameProtocol) {}

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
    actorSlot?: number
  ): Promise<void> {
    throwIfAborted(signal);
    await this.write(
      connection,
      this.protocol.encode(
        kind,
        name,
        payload,
        metadata,
        compress,
        requestSeq,
        correlationId,
        actorSlot
      ),
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
      await Promise.allSettled(Array.from(this.pendingWrites));
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
    } finally {
      this.pendingWrites.delete(write);
    }
  }
}
