// SPDX-License-Identifier: MPL-2.0

import type { PairSocket } from './pair_socket';
import type { PubSocket } from './pub_socket';
import type { XPubSocket } from './xpub_socket';
import type { SubSocket } from './sub_socket';
import type { XSubSocket } from './xsub_socket';
import type { DealerSocket } from './dealer_socket';
import type { RouterSocket } from './router_socket';
import type { StreamSocket } from './stream_socket';

export {
  CommonSocketOptions as RuntimeCommonSocketOptions,
  DealerSocketOptions as RuntimeDealerSocketOptions,
  PubSocketOptions as RuntimePubSocketOptions,
  RouterSocketOptions as RuntimeRouterSocketOptions,
  StreamSocketOptions as RuntimeStreamSocketOptions,
  SubSocketOptions as RuntimeSubSocketOptions,
} from './socket_options';
export { PairSocket as RuntimePairSocket } from './pair_socket';
export { PubSocket as RuntimePubSocket } from './pub_socket';
export { XPubSocket as RuntimeXPubSocket } from './xpub_socket';
export { SubSocket as RuntimeSubSocket } from './sub_socket';
export { XSubSocket as RuntimeXSubSocket } from './xsub_socket';
export { DealerSocket as RuntimeDealerSocket } from './dealer_socket';
export { RouterSocket as RuntimeRouterSocket } from './router_socket';
export { StreamSocket as RuntimeStreamSocket } from './stream_socket';

export type BaseSocket =
  | PairSocket
  | PubSocket
  | XPubSocket
  | SubSocket
  | XSubSocket
  | DealerSocket
  | RouterSocket
  | StreamSocket;
export type { BaseSocket as RuntimeBaseSocket };
