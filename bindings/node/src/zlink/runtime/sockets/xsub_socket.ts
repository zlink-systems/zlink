// SPDX-License-Identifier: MPL-2.0

import { SubSocketOptions } from './socket_options';
import {
  SubscriberSocket,
} from './socket_operations';
import type { RuntimeContext as Context } from '../core/context';
import { SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';

export class XSubSocket extends SubscriberSocket {
  readonly options: SubSocketOptions;
  constructor(ctx: Context) { super(ctx, NativeSocketType.XSUB); this.options = SubSocketOptions.create(this); }
}
