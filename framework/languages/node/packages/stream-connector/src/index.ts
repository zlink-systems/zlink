export * from './Contracts';
export { zlinkStreamAssert } from './Runtime/ZlinkStreamAssertions';
import { DefaultZlinkStreamConnector } from './Runtime/ZlinkStreamConnector';
import type { ZlinkStreamConnector, ZlinkStreamConnectorOptions } from './Contracts';

/**
 * The only way to build a connector (TypeScript spec §4). The concrete class
 * stays inside the package — the other four connectors hide their
 * implementation type as well, and a consumer that named it here would depend
 * on a shape no spec fixes. `create` validates every option before it returns
 * and throws `ZlinkStreamException` when one is rejected (common spec §6.3).
 */
export const zlinkStreamConnectorFactory = {
  create(options: ZlinkStreamConnectorOptions): ZlinkStreamConnector {
    return new DefaultZlinkStreamConnector(options);
  }
};
