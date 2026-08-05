import { setTimeout as delay } from 'node:timers/promises';
import { Inject, Injectable } from '@nestjs/common';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  type ZLinkRouteClient
} from '@zlink-systems/framework';
import { ZLINK_ROUTE_CLIENT } from '@zlink-systems/nestjs';
import { ReportSpotEventMsg } from '../../../../../Shared/contracts';
import { ZoneWorldNames } from '../../../../../Shared/spec';

/** Sends provider-neutral Spot failure reports to the Ops application channel. */
@Injectable()
class OpsReportAdapter {
  constructor(@Inject(ZLINK_ROUTE_CLIENT) private readonly channels: ZLinkRouteClient) {}

  async reportSpotEvent(nodeId: string, kind: string, detail: string): Promise<void> {
    const message = new ReportSpotEventMsg(
      nodeId,
      kind,
      detail,
      new Date().toISOString()
    );

    // The node may start its timer before Ops has finished opening the report
    // endpoint. Retry only that bounded readiness failure; the original timer
    // exception remains the failure observed by the timer runtime.
    const maxAttempts = 50;
    for (let attempt = 0; attempt < maxAttempts; attempt += 1) {
      try {
        console.log(`spot event report submit node=${nodeId} attempt=${attempt + 1}`);
        await this.channels
          .sendToChannel(ZoneWorldNames.reportChannel, message)
          .submit();
        console.log(`spot event report submitted node=${nodeId}`);
        return;
      } catch (error) {
        if (!isReportReadinessFailure(error) || attempt === maxAttempts - 1) {
          throw error;
        }
        await delay(100);
      }
    }

    throw new Error('Spot event report retry loop did not complete.');
  }
}

function isReportReadinessFailure(error: unknown): boolean {
  return error instanceof ZLinkFrameworkException
    && (error.kind === ZLinkFrameworkErrorKind.NotFound
      || error.kind === ZLinkFrameworkErrorKind.Unavailable);
}

export { OpsReportAdapter };
