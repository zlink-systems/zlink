import type {
  RoutingId,
  ZLinkFrameworkRuntimeState,
  ZLinkSpotKind
} from '../../contracts';

declare const spotHandleBrand: unique symbol;

export interface SpotHandle {
  readonly meshName: string;
  readonly spotId: RoutingId;
  readonly [spotHandleBrand]: never;
}

export interface ZLinkSpotHandleResolver {
  resolveSpotHandle(
    meshName: string,
    spotId: RoutingId,
    signal?: AbortSignal
  ): Promise<SpotHandle | undefined>;
}

export interface ZLinkActorSpotHandleResolver {
  resolveActorSpotHandle(
    meshName: string,
    actorId: string,
    signal?: AbortSignal
  ): Promise<SpotHandle | undefined>;
}

export interface ResolvedSpotHandle {
  readonly meshName: string;
  readonly nodeRid: RoutingId;
  readonly spotId: RoutingId;
  readonly spotKind?: ZLinkSpotKind;
  readonly spotGeneration?: bigint;
  /** Current state of the owning MeshNode, when the Location Store provides it. */
  readonly targetNodeState?: ZLinkFrameworkRuntimeState;
}

type SpotHandleResolver = (signal?: AbortSignal) => Promise<ResolvedSpotHandle | undefined>;

interface SpotHandleState {
  current: ResolvedSpotHandle | undefined;
  readonly refresh: SpotHandleResolver;
  refreshing?: Promise<ResolvedSpotHandle | undefined>;
}

const handleStates = new WeakMap<SpotHandle, SpotHandleState>();

export function createSpotHandle(
  spotId: string,
  initial: ResolvedSpotHandle,
  refresh: SpotHandleResolver
): SpotHandle;
export function createSpotHandle(spotId: string, refresh: SpotHandleResolver): SpotHandle;
export function createSpotHandle(
  spotId: string,
  initialOrRefresh: ResolvedSpotHandle | SpotHandleResolver,
  refresh?: SpotHandleResolver
): SpotHandle {
  const initial = typeof initialOrRefresh === 'function' ? undefined : initialOrRefresh;
  const handle = Object.freeze({
    meshName: initial?.meshName ?? '',
    spotId
  }) as SpotHandle;
  handleStates.set(handle, typeof initialOrRefresh === 'function'
    ? { current: undefined, refresh: initialOrRefresh }
    : { current: initialOrRefresh, refresh: refresh! });
  return handle;
}

export async function resolveSpotHandle(
  handle: SpotHandle,
  signal?: AbortSignal
): Promise<ResolvedSpotHandle | undefined> {
  const state = requireHandleState(handle);
  return state.current ?? await refreshSpotHandle(handle, signal);
}

export async function refreshSpotHandle(
  handle: SpotHandle,
  signal?: AbortSignal
): Promise<ResolvedSpotHandle | undefined> {
  const state = requireHandleState(handle);
  state.refreshing ??= state.refresh(signal).then((resolved) => {
    state.current = resolved;
    return resolved;
  }).finally(() => {
    state.refreshing = undefined;
  });
  return await state.refreshing;
}

function requireHandleState(handle: SpotHandle): SpotHandleState {
  const state = handleStates.get(handle);
  if (state === undefined) {
    throw new TypeError('SpotHandle was not created by this framework runtime.');
  }
  return state;
}
