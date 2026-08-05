// SPDX-License-Identifier: MPL-2.0

import { PubSocketOptions } from './socket_options';
import {
  PublisherSocket,
} from './socket_operations';
import type { RuntimeContext as Context } from '../core/context';
import { SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';

export class PubSocket extends PublisherSocket {
  readonly options: PubSocketOptions;
  constructor(ctx: Context) { super(ctx, NativeSocketType.PUB); this.options = PubSocketOptions.create(this); }
}
