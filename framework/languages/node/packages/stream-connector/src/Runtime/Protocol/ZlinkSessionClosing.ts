import { ZlinkStreamControlPacket } from '@zlink-systems/stream-wire';
export {
  decodeStreamWireSessionClosing as decodeSessionClosing,
  encodeStreamWireSessionClosing as encodeSessionClosing
} from '@zlink-systems/stream-wire';
export const ZLINK_SESSION_CLOSING = ZlinkStreamControlPacket.SessionClosing;
