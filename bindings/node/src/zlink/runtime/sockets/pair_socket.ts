// SPDX-License-Identifier: MPL-2.0

import { CommonSocketOptions } from './socket_options';
import {
  MessageSocket,
} from './socket_operations';
import type { RuntimeContext as Context } from '../core/context';
import { SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';

export class PairSocket extends MessageSocket {
  readonly options: CommonSocketOptions;
  constructor(ctx: Context) { super(ctx, NativeSocketType.PAIR); this.options = CommonSocketOptions.create(this); }
}
