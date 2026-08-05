import type { ZLinkBackendObject } from '../contracts';
import {
  closeWithBusyRetry,
  disableSocketLinger,
  isBindingNotFound,
  isContextTerminatedError,
  isRouteRecvRetryable,
  submitBindingRequestCallback,
  submitBindingSend,
  toNativeRoutingId,
  withNativeFallback,
  zlink,
  type ZLinkBindingRequestOperation,
  type ZLinkBindingSendOperation
} from './node-backend-adapter-support';

export interface ZLinkNodeSocketWrapOptions {
  readonly reuseReceived?: boolean;
}

export function wrapSocket<T extends { close(): void }>(
  nativeInstance: T,
  options: ZLinkNodeSocketWrapOptions = {}
): T & ZLinkBackendObject {
  const boundEndpoints = new Set<string>();
  const connectedEndpoints = new Set<string>();
  const peerRoutingIds = new Set<unknown>();
  // STREAM Framework ingress closes each envelope before the next recv. Reuse
  // the caller-provided envelope there so an idle/steady receive loop does not
  // allocate another JavaScript Received object for every poll.
  const reusableReceived = options.reuseReceived === true ? new zlink.Received() : undefined;
  const socket = nativeInstance as T & {
    options?: {
      peerWeight?: number;
      sendHwm?: number;
      recvHwm?: number;
      sendTimeout?: number;
      maxMsgSize?: bigint;
      lastEndpoint?: string;
    };
  };
  const adapter = {
    nativeInstance,
    async dispose(): Promise<void> {
      reusableReceived?.close();
      disableSocketLinger(nativeInstance);
      closeSocketRoutes(nativeInstance, peerRoutingIds);
      closeSocketEndpoints(nativeInstance, boundEndpoints, connectedEndpoints);
      await closeWithBusyRetry(nativeInstance);
    },
    close(): void {
      reusableReceived?.close();
      nativeInstance.close();
    },
    bind(endpoint: string): void {
      (nativeInstance as T & { bind(endpoint: string): void }).bind(endpoint);
      boundEndpoints.add(endpoint);
    },
    unbind(endpoint: string): void {
      (nativeInstance as T & { unbind(endpoint: string): void }).unbind(endpoint);
      boundEndpoints.delete(endpoint);
    },
    connect(endpoint: string): void {
      (nativeInstance as T & { connect(endpoint: string): void }).connect(endpoint);
      connectedEndpoints.add(endpoint);
    },
    disconnect(endpoint: string): void {
      (nativeInstance as T & { disconnect(endpoint: string): void }).disconnect(endpoint);
      connectedEndpoints.delete(endpoint);
    },
    setChannelName(channelName: string): void {
      const setChannelName = (nativeInstance as T & { setChannelName?: (value: string) => void }).setChannelName;
      setChannelName?.call(nativeInstance, channelName);
    },
    get lastEndpoint(): string | undefined {
      return socket.options?.lastEndpoint;
    },
    setRoutingId(routingId: unknown): void {
      (nativeInstance as T & { setRoutingId(value: unknown): void }).setRoutingId(toNativeRoutingId(routingId));
    },
    onSendReady(handler: () => void): void {
      (nativeInstance as T & { setSendReadyHandler(value: () => void): void }).setSendReadyHandler(handler);
    },
    get peerWeight(): number {
      return socket.options?.peerWeight ?? 100;
    },
    set peerWeight(value: number) {
      requireSocketOptions(socket).peerWeight = value;
    },
    get sendHighWaterMark(): number {
      return Number(socket.options?.sendHwm ?? 0n);
    },
    set sendHighWaterMark(value: number) {
      requireSocketOptions(socket).sendHwm = value;
    },
    get receiveHighWaterMark(): number {
      return Number(socket.options?.recvHwm ?? 0n);
    },
    set receiveHighWaterMark(value: number) {
      requireSocketOptions(socket).recvHwm = value;
    },
    get sendTimeoutMs(): number {
      return socket.options?.sendTimeout ?? 0;
    },
    set sendTimeoutMs(value: number) {
      requireSocketOptions(socket).sendTimeout = value;
    },
    get maxMessageSize(): number {
      return Number(socket.options?.maxMsgSize ?? 0n);
    },
    set maxMessageSize(value: number) {
      requireSocketOptions(socket).maxMsgSize = BigInt(value);
    },
    send(...args: unknown[]): unknown {
      if (args.length >= 3) {
        const [routingId, payload, flags] = args as [unknown, unknown, number];
        peerRoutingIds.add(routingId);
        return submitBindingSend(
          (nativeInstance as T & { send(routingId: unknown): ZLinkBindingSendOperation })
            .send(toNativeRoutingId(routingId)),
          payload,
          flags
        );
      }
      const [payload, flags] = args as [unknown, number | undefined];
      return submitBindingSend(
        (nativeInstance as T & { send(): ZLinkBindingSendOperation }).send(),
        payload,
        flags ?? 0
      );
    },
    request(...args: unknown[]): unknown {
      if (args.length >= 5) {
        const [routingId, payload, callback, flags, timeoutMs] = args as [
          unknown, unknown, unknown, number, number | undefined
        ];
        peerRoutingIds.add(routingId);
        return submitBindingRequestCallback(
          (nativeInstance as T & { request(routingId: unknown): ZLinkBindingRequestOperation })
            .request(toNativeRoutingId(routingId)),
          payload,
          callback,
          flags,
          timeoutMs
        );
      }
      if (args.length >= 4) {
        const [payload, callback, flags, timeoutMs] = args as [unknown, unknown, number, number | undefined];
        return submitBindingRequestCallback(
          (nativeInstance as T & { request(): ZLinkBindingRequestOperation }).request(),
          payload,
          callback,
          flags,
          timeoutMs
        );
      }
      return (nativeInstance as T & { request(...values: unknown[]): unknown }).request(...args);
    },
    reply(...args: unknown[]): unknown {
      const [routingId, requestSeq, payload] = args as [unknown, bigint, unknown];
      peerRoutingIds.add(routingId);
      const operation = (nativeInstance as T & {
        reply(routingId: unknown, requestSeq: bigint): ZLinkBindingSendOperation;
      }).reply(toNativeRoutingId(routingId), requestSeq);
      return args.length < 3 ? operation : submitBindingSend(operation, payload, 0);
    },
    recv(flags?: number): unknown {
      const received = reusableReceived ?? new zlink.Received();
      let ok = false;
      try {
        ok = (nativeInstance as T & { recv(result: unknown, flags?: number): boolean }).recv(received, flags);
      } catch (error) {
        received.close();
        if (isRouteRecvRetryable(error)) {
          return undefined;
        }
        throw error;
      }
      if (!ok) {
        received.close();
        return undefined;
      }
      return received;
    },
    publish(...args: unknown[]): unknown {
      if (args.length >= 3) {
        const [topic, payload, flags] = args as [string, unknown, number];
        return submitBindingSend(
          (nativeInstance as T & { publish(topic: string): ZLinkBindingSendOperation }).publish(topic),
          payload,
          flags
        );
      }
      return (nativeInstance as T & { publish(...values: unknown[]): unknown }).publish(...args);
    },
    sendToSpot(targetRid: unknown, targetSpot: unknown, payload: unknown, flags: number): boolean {
      return submitBindingSend(
        (nativeInstance as T & {
          sendToSpot(targetRid: unknown, targetSpot: unknown): ZLinkBindingSendOperation;
        }).sendToSpot(toNativeRoutingId(targetRid), toNativeRoutingId(targetSpot)),
        payload,
        flags
      );
    },
    requestToSpot(
      targetRid: unknown,
      targetSpot: unknown,
      payload: unknown,
      callback: unknown,
      flags: number,
      timeoutMs?: number
    ): boolean {
      return submitBindingRequestCallback(
        (nativeInstance as T & {
          requestToSpot(targetRid: unknown, targetSpot: unknown): ZLinkBindingRequestOperation;
        }).requestToSpot(toNativeRoutingId(targetRid), toNativeRoutingId(targetSpot)),
        payload,
        callback,
        flags,
        timeoutMs
      );
    },
    disconnectPeer(routingId: unknown): void {
      (nativeInstance as T & { disconnectRid(value: unknown): void }).disconnectRid(toNativeRoutingId(routingId));
    },
    async bindActor(_sessionRid: unknown, _actor: unknown, _timeoutMs: number, _signal?: AbortSignal): Promise<void> {
      throw new Error(
        'Session Actor dispatch requires enableActorDispatch() and a Framework MeshNode service route.'
      );
    },
    async unbindActor(_sessionRid: unknown, _actorId: string, _timeoutMs: number, _signal?: AbortSignal): Promise<void> {
      throw new Error(
        'Session Actor dispatch requires enableActorDispatch() and a Framework MeshNode service route.'
      );
    },
    sendBoundActor(_sessionRid: unknown, _actorId: string, _parts: readonly unknown[], _flags: number): boolean {
      return false;
    }
  };
  return withNativeFallback(adapter, nativeInstance) as unknown as T & ZLinkBackendObject;
}

function requireSocketOptions<TOptions>(socket: { readonly options?: TOptions }): TOptions {
  if (socket.options === undefined) {
    throw new TypeError('Binding socket does not expose options.');
  }
  return socket.options;
}

function closeSocketRoutes(target: unknown, peerRoutingIds: Set<unknown>): void {
  if (peerRoutingIds.size === 0 || !hasDisconnectRid(target)) {
    return;
  }
  for (const routingId of peerRoutingIds) {
    try {
      target.disconnectRid(toNativeRoutingId(routingId));
    } catch (error) {
      if (!isDisconnectRouteNotFoundError(error)) {
        throw error;
      }
    }
    peerRoutingIds.delete(routingId);
  }
}

export function isDisconnectRouteNotFoundError(error: unknown): boolean {
  return isBindingNotFound(error) || isContextTerminatedError(error) || (
    error instanceof Error && 'code' in error &&
    ((error as { code: unknown }).code === zlink.ConnectResult.NotFound ||
      (error as { code: unknown }).code === zlink.ConnectResult.Busy ||
      ((error as { code: unknown }).code === zlink.ConnectResult.InternalError &&
        /current state/i.test(error.message)))
  );
}

function hasDisconnectRid(target: unknown): target is { disconnectRid(routingId: unknown): void } {
  return target !== null && typeof target === 'object' && 'disconnectRid' in target &&
    typeof (target as { disconnectRid: unknown }).disconnectRid === 'function';
}

function closeSocketEndpoints(target: unknown, boundEndpoints: Set<string>, connectedEndpoints: Set<string>): void {
  for (const endpoint of connectedEndpoints) {
    try {
      (target as { disconnect(endpoint: string): void }).disconnect(endpoint);
    } catch (error) {
      if (!isEndpointCloseIgnorableError(error)) {
        throw error;
      }
    }
    connectedEndpoints.delete(endpoint);
  }
  for (const endpoint of boundEndpoints) {
    try {
      (target as { unbind(endpoint: string): void }).unbind(endpoint);
    } catch (error) {
      if (!isEndpointCloseIgnorableError(error)) {
        throw error;
      }
    }
    boundEndpoints.delete(endpoint);
  }
}

export function isEndpointCloseIgnorableError(error: unknown): boolean {
  return isContextTerminatedError(error) || (
    error instanceof Error && 'code' in error &&
    ((error as { code: unknown }).code === 604 || (error as { code: unknown }).code === zlink.ConnectResult.NotFound)
  );
}
