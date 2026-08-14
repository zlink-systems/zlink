// SPDX-License-Identifier: MPL-2.0

import { DealerSocketOptions } from './socket_options';
import { normalizeRoutingId } from '../core/routing_id';
import {
  ManagedRoutedRuntimeSendOperation,
  ReceiveSocket,
  RuntimeRequestOperation,
} from './socket_operations';
import { RoutedAdmission, resolvedRequestTimeout } from './routed_admission';
import type { RuntimeContext as Context } from '../core/context';
import { configCall } from '../errors/native_errors';
import { getNativeHandle } from '../handles/native_handle';
import type { NativeReceivedRaw } from '../messaging/message_materializer';
import { requireNative } from '../native/native';
import { RoutingId } from '../../contracts';
import { RecvFlags, SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';
import type { RequestOperation, RoutedSendOperation } from '../../contracts/messaging';

const native = requireNative();

export class DealerSocket extends ReceiveSocket {
  private readonly admission: RoutedAdmission;
  readonly options: DealerSocketOptions;
  constructor(ctx: Context) {
    super(ctx, NativeSocketType.DEALER);
    this.options = DealerSocketOptions.create(this);
    try {
      this.admission = new RoutedAdmission(getNativeHandle(this), 'dealer');
    } catch (error) {
      super.close();
      throw error;
    }
  }
  setRoutingId(routingId: RoutingId): void {
    const normalizedRoutingId = normalizeRoutingId(routingId);
    configCall('routing id set failed', () => {
      native.handleSetRoutingId(getNativeHandle(this), normalizedRoutingId);
    });
  }
  getRoutingId(): RoutingId {
    return RoutingId.from(
      configCall('routing id get failed', () =>
        native.handleGetRoutingId(getNativeHandle(this)) as Buffer
      )
    );
  }
  send(): RoutedSendOperation {
    return new ManagedRoutedRuntimeSendOperation(
      (_selector, parts, timeoutMs, startedAt) => this.admission.send(
        null,
        parts,
        timeoutMs === 0 ? this.options.sendTimeout : timeoutMs,
        startedAt
      ),
      null
    );
  }
  request(): RequestOperation {
    return new RuntimeRequestOperation((parts, timeoutMs, startedAt) =>
      this.admission.request(
        null,
        parts,
        resolvedRequestTimeout(timeoutMs, this.options.requestTimeout),
        startedAt
      )
    );
  }
  /** @internal DEALER retained receive must preserve typed request metadata. */
  protected recvRetainedRaw(flags: RecvFlags): NativeReceivedRaw | null {
    return ((flags | 0) & (RecvFlags.DontWait | 0))
      ? native.dealerRecvMessageRetainedNoWait(getNativeHandle(this))
      : native.dealerRecvMessageRetained(getNativeHandle(this), flags | 0);
  }
  close(): void {
    this.admission.close();
    super.close();
    this.admission.finishClose();
  }
}
