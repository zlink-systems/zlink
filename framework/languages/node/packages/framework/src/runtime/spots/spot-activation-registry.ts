import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type {
  RoutingId,
  Type,
  ZLinkSpot,
  ZLinkSpotInfo
} from '../../contracts';
import type { ZLinkLocalSpotCreateResult } from './spot-manager-internal-contracts';
import {
  ZLinkSpotCreateState
} from '../../contracts';
import type { ZLinkSpotActivation } from './spot-activation-state';
import { ZLinkSpotCloseOccupiedError } from './spot-activation-state';
import { ZLinkConfigurationException } from '../configuration';
import { createAbortError } from '../abort';
import { ZLinkSpotLifecycleMetrics } from './spot-lifecycle-metrics';
export { ZLinkSpotActivation } from './spot-activation-state';

interface PendingSpotActivation {
  readonly spotType: Type<ZLinkSpot>;
  readonly ready: Promise<ZLinkLocalSpotCreateResult>;
}

export interface ZLinkSpotActivationOperation {
  readonly owner: boolean;
  readonly ready: Promise<ZLinkLocalSpotCreateResult>;
}

export interface ZLinkSpotCloseOperation {
  readonly activation: ZLinkSpotActivation;
  /** Resolves to `false` when a post-quiescence occupancy recheck aborted the close. */
  readonly ready: Promise<boolean>;
  readonly started: boolean;
}

export class ZLinkSpotActivationRegistry {
  private nextId = 1;
  private readonly activations = new Map<string, ZLinkSpotActivation>();
  private readonly staged = new Set<string>();
  private readonly pending = new Map<string, PendingSpotActivation>();
  private readonly closing = new Map<string, ZLinkSpotCloseOperation>();
  private readonly failedClose = new Set<string>();
  private readonly emptyWaiters = new Set<() => void>();
  private readonly meshEmptyWaiters = new Map<string, Set<() => void>>();
  private readonly lifecycleMetrics: ZLinkSpotLifecycleMetrics;

  constructor(metrics?: import('../diagnostics').ZLinkRuntimeMetrics) {
    this.lifecycleMetrics = new ZLinkSpotLifecycleMetrics(metrics);
  }

  allocateSpotId(meshName: string): RoutingId {
    let spotId: RoutingId;
    do {
      spotId = `spot-${this.nextId}`;
      this.nextId += 1;
    } while (this.activations.has(spotActivationKey(meshName, spotId)));
    return spotId;
  }

  resolve(meshName: string, spotId: RoutingId): ZLinkSpotActivation | undefined {
    const key = spotActivationKey(meshName, spotId);
    return this.closing.has(key) || this.failedClose.has(key) || this.staged.has(key)
      ? undefined
      : this.activations.get(key);
  }

  resolveUnique(spotId: RoutingId): ZLinkSpotActivation | undefined {
    const matches = [...this.activations.values()]
      .filter((activation) =>
        String(activation.spotId) === String(spotId)
        && !this.staged.has(spotActivationKey(activation.meshName, activation.spotId))
        && !this.closing.has(spotActivationKey(activation.meshName, activation.spotId))
        && !this.failedClose.has(spotActivationKey(activation.meshName, activation.spotId)));
    return matches.length === 1 ? matches[0] : undefined;
  }

  has(meshName: string, spotId: RoutingId): boolean {
    const key = spotActivationKey(meshName, spotId);
    return !this.staged.has(key)
      && !this.closing.has(key)
      && !this.failedClose.has(key)
      && this.activations.has(key);
  }

  canClose(meshName: string, spotId: RoutingId): boolean {
    const key = spotActivationKey(meshName, spotId);
    return !this.staged.has(key)
      && !this.closing.has(key)
      && !this.failedClose.has(key)
      && this.activations.get(key)?.canClose() === true;
  }

  list(meshName: string): readonly ZLinkSpotInfo[] {
    return [...this.activations.values()]
      .filter((activation) => {
        if (activation.meshName !== meshName) return false;
        const key = spotActivationKey(activation.meshName, activation.spotId);
        return !this.staged.has(key) && !this.closing.has(key) && !this.failedClose.has(key);
      })
      .map((activation) => String(activation.spotId))
      .sort((left, right) => left.localeCompare(right))
      .map((spotId) => ({ spotId }));
  }

  activeActivations(): readonly ZLinkSpotActivation[] {
    return [...this.activations.values()];
  }

  closingOperation(meshName: string, spotId: RoutingId): ZLinkSpotCloseOperation | undefined {
    return this.closing.get(spotActivationKey(meshName, spotId));
  }

  whenEmpty(signal?: AbortSignal): Promise<void> {
    if (this.isEmpty()) return Promise.resolve();
    if (signal?.aborted === true) return Promise.reject(createAbortError());
    return new Promise<void>((resolve, reject) => {
      const complete = () => {
        signal?.removeEventListener('abort', abort);
        resolve();
      };
      const abort = () => {
        this.emptyWaiters.delete(complete);
        reject(createAbortError());
      };
      this.emptyWaiters.add(complete);
      signal?.addEventListener('abort', abort, { once: true });
    });
  }

  whenMeshEmpty(meshName: string, signal?: AbortSignal): Promise<void> {
    if (!this.hasMeshWork(meshName)) return Promise.resolve();
    if (signal?.aborted === true) return Promise.reject(createAbortError());
    return new Promise<void>((resolve, reject) => {
      const waiters = this.meshEmptyWaiters.get(meshName) ?? new Set<() => void>();
      this.meshEmptyWaiters.set(meshName, waiters);
      const complete = () => {
        signal?.removeEventListener('abort', abort);
        resolve();
      };
      const abort = () => {
        waiters.delete(complete);
        reject(createAbortError());
      };
      waiters.add(complete);
      signal?.addEventListener('abort', abort, { once: true });
    });
  }

  register(activation: ZLinkSpotActivation): void {
    const key = spotActivationKey(activation.meshName, activation.spotId);
    if (this.activations.has(key)) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotTypeMismatch,
        `Spot '${activation.spotId}' was activated more than once.`
      );
    }
    this.activations.set(key, activation);
    this.lifecycleMetrics.opened('user');
  }

  stage(meshName: string, spotId: RoutingId): void {
    this.staged.add(spotActivationKey(meshName, spotId));
  }

  publish(meshName: string, spotId: RoutingId): void {
    this.staged.delete(spotActivationKey(meshName, spotId));
  }

  detachRelocated(meshName: string, spotId: RoutingId): ZLinkSpotActivation | undefined {
    const key = spotActivationKey(meshName, spotId);
    if (this.pending.has(key) || this.closing.has(key)) {
      throw new Error(`Spot '${String(spotId)}' cannot detach while lifecycle work is pending.`);
    }
    const activation = this.activations.get(key);
    if (activation === undefined) return undefined;
    this.activations.delete(key);
    this.staged.delete(key);
    this.failedClose.delete(key);
    this.lifecycleMetrics.closed('user');
    this.resolveEmptyWaiters();
    this.resolveMeshEmptyWaiters(meshName);
    return activation;
  }

  abandonStage(meshName: string, spotId: RoutingId): void {
    const key = spotActivationKey(meshName, spotId);
    // A materialized Spot must remain hidden until close cleanup removes it.
    // If materialization failed before registration there is nothing to hide.
    if (!this.activations.has(key)) {
      this.staged.delete(key);
    }
  }

  startClose(
    meshName: string,
    spotId: RoutingId,
    close: (activation: ZLinkSpotActivation) => Promise<void>,
    resourcesReleased: (activation: ZLinkSpotActivation) => boolean
  ): ZLinkSpotCloseOperation | undefined {
    const key = spotActivationKey(meshName, spotId);
    const closing = this.closing.get(key);
    if (closing !== undefined) {
      return { ...closing, started: false };
    }
    const activation = this.activations.get(key);
    if (activation === undefined || !activation.canClose()) {
      return undefined;
    }
    const operation = {} as ZLinkSpotCloseOperation;
    let completed = false;
    let occupiedAfterQuiescence = false;
    const ready = Promise.resolve()
      .then(() => close(activation))
      .then(() => { completed = true; })
      .catch((error: unknown) => {
        // The seal's post-quiescence recheck (spot-activation.ts
        // closeAfterSeal) found a new join and released the seal instead of
        // closing. The activation is already back to normal operation, so
        // this is a "did not close" outcome, not a close failure.
        if (error instanceof ZLinkSpotCloseOccupiedError) {
          occupiedAfterQuiescence = true;
          return;
        }
        throw error;
      })
      .finally(() => {
        if (this.closing.get(key) === operation) {
          this.closing.delete(key);
          if (occupiedAfterQuiescence) {
            this.resolveEmptyWaiters();
            this.resolveMeshEmptyWaiters(meshName);
            return;
          }
          if (completed || resourcesReleased(activation)) {
            this.activations.delete(key);
            this.staged.delete(key);
            this.lifecycleMetrics.closed('user');
            this.failedClose.delete(key);
          } else {
            this.failedClose.add(key);
          }
          this.resolveEmptyWaiters();
          this.resolveMeshEmptyWaiters(meshName);
        }
      })
      .then(() => !occupiedAfterQuiescence);
    Object.assign(operation, { activation, ready, started: true });
    this.closing.set(key, operation);
    return operation;
  }

  getOrBegin(
    meshName: string,
    spotType: Type<ZLinkSpot>,
    spotId: RoutingId,
    create: () => Promise<ZLinkLocalSpotCreateResult>
  ): ZLinkSpotActivationOperation {
    const key = spotActivationKey(meshName, spotId);
    const closing = this.closing.get(key);
    if (closing !== undefined) {
      return {
        owner: false,
        ready: closing.ready
          .catch(() => undefined)
          .then(() => this.getOrBegin(meshName, spotType, spotId, create).ready)
      };
    }
    if (this.failedClose.has(key)) {
      return {
        owner: false,
        ready: Promise.reject(new ZLinkConfigurationException(
          `Spot '${spotId}' cleanup has not completed.`
        ))
      };
    }
    const existing = this.activations.get(key);
    if (existing !== undefined) {
      if (existing.spotType !== spotType) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotTypeMismatch,
          `Spot '${spotId}' already exists with a different spot type.`
        );
      }
      return {
        owner: false,
        ready: Promise.resolve({ spotId, state: ZLinkSpotCreateState.Existing })
      };
    }

    const pending = this.pending.get(key);
    if (pending === undefined) {
      const ready = Promise.resolve().then(create);
      const pending: PendingSpotActivation = { spotType, ready };
      this.pending.set(key, pending);
      const tracked = ready.finally(() => {
        if (this.pending.get(key) === pending) {
          this.pending.delete(key);
          this.resolveEmptyWaiters();
          this.resolveMeshEmptyWaiters(meshName);
        }
      });
      return { owner: true, ready: tracked };
    }
    if (pending.spotType !== spotType) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotTypeMismatch,
        `Spot '${spotId}' is being created with a different spot type.`
      );
    }
    return {
      owner: false,
      ready: pending.ready.then((result) => result.state === ZLinkSpotCreateState.Created
        ? { spotId, state: ZLinkSpotCreateState.Existing }
        : { spotId, state: result.state, reply: result.reply })
    };
  }

  private isEmpty(): boolean {
    return this.activations.size === 0 && this.pending.size === 0 && this.closing.size === 0;
  }

  private resolveEmptyWaiters(): void {
    if (!this.isEmpty()) return;
    for (const resolve of this.emptyWaiters) resolve();
    this.emptyWaiters.clear();
  }

  private hasMeshWork(meshName: string): boolean {
    const prefix = `${meshName}\0`;
    return [...this.activations.keys(), ...this.pending.keys(), ...this.closing.keys()]
      .some((key) => key.startsWith(prefix));
  }

  private resolveMeshEmptyWaiters(meshName: string): void {
    if (this.hasMeshWork(meshName)) return;
    const waiters = this.meshEmptyWaiters.get(meshName);
    if (waiters === undefined) return;
    this.meshEmptyWaiters.delete(meshName);
    for (const resolve of waiters) resolve();
  }
}

function spotActivationKey(meshName: string, spotId: RoutingId): string {
  return `${meshName}\0${String(spotId)}`;
}
