/**
 * The first four ChannelHeader fields (kind, channelName, messageName,
 * contentType) are identical for every message of one channel, packet type
 * and direction, and protobuf writes them first, in field-number order. Their
 * encoded bytes are therefore a fixed prefix of every header of that kind.
 *
 * This module owns that prefix. The bytes are always produced by the
 * generated ChannelHeader encoder (passed in as `encodePrefix`), never written
 * by hand, so the wire format stays owned by the `.proto`. The same entries
 * serve the outbound side (skip re-encoding the four strings) and the inbound
 * side (skip re-decoding them when the header bytes start with a known prefix).
 */
export interface ChannelHeaderPrefix {
  readonly kind: number;
  readonly channelName: string;
  readonly messageName: string;
  readonly contentType: string;
  /** Generated-encoder bytes of exactly these four fields. */
  readonly bytes: Buffer;
}

export type EncodeChannelHeaderPrefix = (
  kind: number,
  channelName: string,
  messageName: string,
  contentType: string
) => Buffer;

// Distinct (kind, channel, packet, content type) tuples in flight on one
// process are few; the ring only has to outlast a request/reply pair per hot
// channel. A miss costs one extra generated-encoder call, never a wrong byte.
const PREFIX_CAPACITY = 8;

export class ChannelHeaderPrefixes {
  private readonly entries: ChannelHeaderPrefix[] = [];
  private next = 0;

  constructor(private readonly encodePrefix: EncodeChannelHeaderPrefix) {}

  /** Outbound: the prefix of a header with these four fields. */
  forFields(
    kind: number,
    channelName: string,
    messageName: string,
    contentType: string
  ): ChannelHeaderPrefix {
    const entries = this.entries;
    for (let index = 0; index < entries.length; index += 1) {
      const entry = entries[index]!;
      if (
        entry.kind === kind &&
        entry.channelName === channelName &&
        entry.messageName === messageName &&
        entry.contentType === contentType
      ) {
        return entry;
      }
    }
    return this.remember({
      kind,
      channelName,
      messageName,
      contentType,
      bytes: this.encodePrefix(kind, channelName, messageName, contentType)
    });
  }

  /** Inbound: the known prefix `header` starts with, if any. */
  matching(header: Uint8Array): ChannelHeaderPrefix | undefined {
    const entries = this.entries;
    for (let index = 0; index < entries.length; index += 1) {
      const entry = entries[index]!;
      const length = entry.bytes.length;
      if (length <= header.length && entry.bytes.compare(header, 0, length, 0, length) === 0) {
        return entry;
      }
    }
    return undefined;
  }

  /**
   * Inbound, after a full decode: remembers the four decoded fields when the
   * header really starts with their canonical encoding (a header that encodes
   * them differently is simply never matched by prefix).
   */
  learn(
    header: Uint8Array,
    kind: unknown,
    channelName: unknown,
    messageName: unknown,
    contentType: unknown
  ): void {
    if (
      typeof kind !== 'number' ||
      typeof channelName !== 'string' ||
      typeof messageName !== 'string' ||
      typeof contentType !== 'string'
    ) {
      return;
    }
    const bytes = this.encodePrefix(kind, channelName, messageName, contentType);
    const length = bytes.length;
    if (length <= header.length && bytes.compare(header, 0, length, 0, length) === 0) {
      this.remember({ kind, channelName, messageName, contentType, bytes });
    }
  }

  private remember(entry: ChannelHeaderPrefix): ChannelHeaderPrefix {
    if (this.entries.length < PREFIX_CAPACITY) {
      this.entries.push(entry);
    } else {
      this.entries[this.next] = entry;
      this.next = (this.next + 1) % PREFIX_CAPACITY;
    }
    return entry;
  }
}
