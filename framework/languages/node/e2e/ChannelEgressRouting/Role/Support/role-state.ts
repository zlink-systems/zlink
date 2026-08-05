import { Injectable } from '@nestjs/common';

@Injectable()
export class RoleState {
  private released = false;

  hold(): void {
    this.released = false;
  }

  release(): void {
    this.released = true;
  }

  async waitUntilReleased(): Promise<void> {
    while (!this.released) await new Promise((resolve) => setTimeout(resolve, 10));
  }
}
