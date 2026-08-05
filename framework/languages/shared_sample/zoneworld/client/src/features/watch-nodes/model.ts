import { signal } from '@preact/signals';
import { NodeModel } from '../../entities/node/model';
import type {
  AnnounceWorldRes,
  NodeAlertNotify,
  NodeDiagnosticsRes,
  NodeStatusNotify,
  SetMaintenanceRes,
  WatchNodesRes,
} from '../../shared/api/contracts';
import { Packets } from '../../shared/api/contracts';
import { StreamClient } from '../../shared/api/stream';

export class OpsController {
  readonly topology = new NodeModel();
  readonly stream: StreamClient;
  readonly diagnostics = signal<NodeDiagnosticsRes | null>(null);
  readonly lastAnnouncementId = signal<string | null>(null);

  constructor(endpoint: string) {
    this.stream = new StreamClient(endpoint);
    this.stream.on<NodeStatusNotify>(Packets.NodeStatusNotify, (message) => this.topology.applyStatus(message));
    this.stream.on<NodeAlertNotify>(Packets.NodeAlertNotify, (message) => this.topology.applyAlert(message));
  }

  async connect(): Promise<void> {
    await this.stream.connect();
    const snapshot = await this.stream.request<WatchNodesRes>(Packets.WatchNodesReq, {});
    this.topology.applySnapshot(snapshot);
  }

  async announce(text: string): Promise<void> {
    if (text.trim().length === 0) return;
    const reply = await this.stream.request<AnnounceWorldRes>(Packets.AnnounceWorldReq, { text: text.trim() });
    this.lastAnnouncementId.value = reply.announcementId;
  }

  async setMaintenance(nodeId: string, enabled: boolean): Promise<void> {
    const reply = await this.stream.request<SetMaintenanceRes>(Packets.SetMaintenanceReq, {
      nodeId: nodeId,
      enabled: enabled,
    });
    if (reply.error !== undefined && reply.error !== null) throw new Error(reply.error);
  }

  async diagnose(nodeId: string): Promise<void> {
    this.diagnostics.value = await this.stream.request<NodeDiagnosticsRes>(Packets.NodeDiagnosticsReq, {
      nodeId: nodeId,
    });
  }

  close(): Promise<void> {
    return this.stream.close();
  }
}
