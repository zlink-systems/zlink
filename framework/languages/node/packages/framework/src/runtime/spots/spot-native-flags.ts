import type { ZLinkBackendRecvFlags } from '../backend/contracts';

// Core RecvFlags.DontWait = 1: non-blocking drain.
export const ZLINK_RECV_DONT_WAIT = 1 as ZLinkBackendRecvFlags;
