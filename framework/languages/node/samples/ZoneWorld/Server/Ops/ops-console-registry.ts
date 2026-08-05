import { Injectable } from '@nestjs/common';
import { NodeAlertNotify, NodeStatusNotify } from '../../Shared/contracts';
import type { NodeView } from '../../Shared/contracts';
import type { ZLinkSessionContext } from '@zlink-systems/framework';

const RECENT_ALERT_COUNT = 20;

@Injectable()
class OpsConsoleRegistry {
  private readonly consoles = new Map<string, ZLinkSessionContext>();
  private readonly alerts: NodeAlertNotify[] = [];

  add(context: ZLinkSessionContext): void {
    this.consoles.set(context.sessionId, context);
  }

  remove(context: ZLinkSessionContext): void {
    this.consoles.delete(context.sessionId);
  }

  publish(node: NodeView): void {
    const notify = new NodeStatusNotify(
      node.nodeId,
      node.registered,
      node.connected,
      node.maintenance,
      node.zones,
      node.playerCount
    );
    for (const context of this.consoles.values()) this.send(context, notify);
  }

  publishAlert(alert: NodeAlertNotify): void {
    this.alerts.push(alert);
    if (this.alerts.length > RECENT_ALERT_COUNT) this.alerts.shift();
    for (const context of this.consoles.values()) this.send(context, alert);
  }

  replayAlerts(context: ZLinkSessionContext): void {
    for (const alert of this.alerts) this.send(context, alert);
  }

  private send(context: ZLinkSessionContext, message: NodeAlertNotify | NodeStatusNotify): void {
    try {
      void context.client.send(message).submit().catch(() => {
        this.remove(context);
      });
    } catch {
      // Session disconnect cleanup owns removal; a concurrent close does not invalidate the alert.
      this.remove(context);
    }
  }
}

export { OpsConsoleRegistry };
