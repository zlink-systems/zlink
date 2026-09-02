// SPDX-License-Identifier: MPL-2.0

import type { RoutingId } from '../core';
import type { Message } from './message';

/** Reusable output storage for one STREAM packet. */
export class StreamPacket {
  private _receiving = false;
  private _routingId: RoutingId | null = null;
  private _header: Message | null = null;
  private _body: Message | null = null;

  get isEmpty(): boolean {
    return !this._receiving
      && this._routingId === null
      && this._header === null
      && this._body === null;
  }

  get routingId(): RoutingId | null { return this._routingId; }
  get header(): Message | null { return this._header; }
  get body(): Message | null { return this._body; }

  close(): void {
    this._header?.close();
    this._body?.close();
    this._routingId = null;
    this._header = null;
    this._body = null;
  }
}

interface StreamPacketState {
  _receiving: boolean;
  _routingId: RoutingId | null;
  _header: Message | null;
  _body: Message | null;
}

/** @internal */
export function streamPacketState(packet: StreamPacket): StreamPacketState {
  return packet as unknown as StreamPacketState;
}
