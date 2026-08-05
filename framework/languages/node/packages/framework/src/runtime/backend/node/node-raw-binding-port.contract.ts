import type { ZLinkFrameworkRegistration } from '../../../contracts/Configuration/Registration';
import {
  ZLinkNodeRawBindingPort,
  type ZLinkRawBindingPort,
  type ZLinkRawHostPort
} from './node-raw-binding-port';

const port: ZLinkRawBindingPort = new ZLinkNodeRawBindingPort();

export function compilePublicHostConnection(
  _registration: ZLinkFrameworkRegistration,
  host: ZLinkRawHostPort = port.createHost()
): void {
  const router = host.createRouter();
  router.setRoutingId('node-a');
  const dealer = host.createDealer();
  dealer.setRoutingId('node-b');
  router.close();
  dealer.close();
  host.close();
}
