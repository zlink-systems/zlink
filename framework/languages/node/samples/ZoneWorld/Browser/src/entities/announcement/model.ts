import { signal } from '@preact/signals';
import type { WorldAnnounceNotify } from '../../shared/api/contracts';

export class AnnouncementModel {
  readonly items = signal<readonly WorldAnnounceNotify[]>([]);
  private readonly seen = new Set<string>();

  apply(notification: WorldAnnounceNotify): void {
    if (this.seen.has(notification.announcementId)) return;
    this.seen.add(notification.announcementId);
    this.items.value = [notification, ...this.items.value].slice(0, 8);
  }
}
