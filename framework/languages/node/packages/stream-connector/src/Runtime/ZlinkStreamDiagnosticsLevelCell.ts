import { ZlinkStreamDiagnosticsLevel, ZlinkStreamErrorCode } from '../Contracts';
import { validateDiagnosticsLevel } from './ZlinkStreamConnectorOptions';
import { connectorError } from './ZlinkStreamSupport';

/**
 * Mutable holder for the connector's current diagnostics level (spec 26 §4.1,
 * spec stream-connector 32 §13). The application can read and change the
 * level at runtime without recreating the connector. Each processing point
 * reads {@link level} exactly once and uses that value for the whole
 * operation; the change takes effect for processing points that read the
 * cell after the change and is never applied retroactively to frames already
 * built.
 */
export class ZlinkStreamDiagnosticsLevelCell {
  private current: ZlinkStreamDiagnosticsLevel;

  constructor(initial: ZlinkStreamDiagnosticsLevel) {
    this.current = initial;
  }

  get level(): ZlinkStreamDiagnosticsLevel {
    return this.current;
  }

  set(level: ZlinkStreamDiagnosticsLevel): void {
    // Unlike the construction-time option, a runtime write has no "omitted
    // means default" meaning: `undefined`/`null` (reachable from plain JS,
    // bypassing the TS type) must be rejected rather than silently stored.
    const candidate: unknown = level;
    if (candidate === undefined || candidate === null) {
      throw connectorError(ZlinkStreamErrorCode.ConfigurationError, 'DiagnosticsLevel is invalid.');
    }
    validateDiagnosticsLevel(level);
    this.current = level;
  }
}
