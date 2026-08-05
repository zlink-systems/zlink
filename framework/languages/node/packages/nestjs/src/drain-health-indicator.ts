import { Injectable } from '@nestjs/common';
import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';

@Injectable()
export class ZLinkDrainHealthIndicator {
  constructor(
    private readonly runtime: ZLinkRouteMeshRuntime,
    private readonly meshName: string
  ) {}

  async isHealthy(key = 'zlink'): Promise<Record<string, { readonly status: 'up' }>> {
    if (!this.runtime.isReady(this.meshName)) {
      throw new Error(`${key} is draining.`);
    }
    return { [key]: { status: 'up' } };
  }
}
