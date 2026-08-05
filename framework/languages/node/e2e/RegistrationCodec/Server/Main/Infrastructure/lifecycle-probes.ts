import { Injectable, Scope } from '@nestjs/common';
import type { OnModuleDestroy } from '@nestjs/common';

let scopedCounter = 0;
let singletonCounter = 0;
let disposedCount = 0;

@Injectable()
export class SingletonProbe {
  readonly id = `singleton-${++singletonCounter}`;
}

@Injectable({ scope: Scope.REQUEST })
export class ScopedProbe implements OnModuleDestroy {
  readonly id = `scoped-${++scopedCounter}`;

  static get disposedCount(): number {
    return disposedCount;
  }

  onModuleDestroy(): void {
    disposedCount += 1;
  }
}
