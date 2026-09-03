import type { ZLinkBackendObject } from '../contracts';
import {
  closeWithBusyRetry,
  disableSocketLinger,
  isBindingNotFound,
  isContextTerminatedError,
  isRouteRecvRetryable,
  submitBindingRequest,
  submitBindingPublish,
  submitBindingReply,
  submitBindingAsyncSend,
  submitBindingSyncSend,
  toNativeRoutingId,
  zlink,
  type ZLinkBindingRequestOperation,
  type ZLinkBindingPublishOperation,
  type ZLinkBindingReplyOperation,
  type ZLinkBindingAsyncSendOperation
} from './node-backend-adapter-support';


export function wrapSocket<T extends { close(): void }>(
  nativeInstance: T
): T & ZLinkBackendObject {
  const boundEndpoints = new Set<string>();
  const connectedEndpoints = new Set<string>();
  const peerRoutingIds = new Set<unknown>();
  const socket = nativeInstance as T & {
    options?: {
      probe?: boolean;
      peerWeight?: number;
      sendHwm?: number;
      recvHwm?: number;
      sendTimeout?: number;
      maxMsgSize?: bigint;
      lastEndpoint?: string;
    };
  };
  const hasRequest = typeof (nativeInstance as { request?: unknown }).request === 'function';
  const hasRoutedPeer = hasRequest
    && typeof (nativeInstance as { reply?: unknown }).reply === 'function';
  const hasStream = typeof (nativeInstance as { recvPacket?: unknown }).recvPacket === 'function';
  const adapter = {
    nativeInstance,
    async dispose(): Promise<void> {
      disableSocketLinger(nativeInstance);
      closeSocketRoutes(nativeInstance, peerRoutingIds);
      closeSocketEndpoints(nativeInstance, boundEndpoints, connectedEndpoints);
      await closeWithBusyRetry(nativeInstance);
    },
    close(): void {
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
    setReceiveFlowState(state: 0 | 1): void {
      (nativeInstance as T & { setReceiveFlowState(value: 0 | 1): void })
        .setReceiveFlowState(state);
    },
    setProbe(enabled: boolean): void {
      requireSocketOptions(socket).probe = enabled;
    },
    setTlsServer(cert: string, key: string, requireClientCert?: boolean): void {
      (nativeInstance as T & {
        setTlsServer(cert: string, key: string, requireClientCert?: boolean): void;
      }).setTlsServer(cert, key, requireClientCert);
    },
    setSubscription(topic: string): void {
      (nativeInstance as T & { setSubscription(topic: string): void }).setSubscription(topic);
    },
    unsetSubscription(topic: string): void {
      (nativeInstance as T & { unsetSubscription(topic: string): void }).unsetSubscription(topic);
    },
    subscribe(result: unknown, flags?: number): boolean {
      return Boolean((nativeInstance as T & {
        subscribe(result: unknown, flags?: number): boolean;
      }).subscribe(result, flags));
    },
    get lastEndpoint(): string | undefined {
      return socket.options?.lastEndpoint;
    },
    setRoutingId(routingId: unknown): void {
      (nativeInstance as T & { setRoutingId(value: unknown): void }).setRoutingId(toNativeRoutingId(routingId));
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
      if (hasStream) {
        const [routingId, payload] = args as [unknown, unknown];
        peerRoutingIds.add(routingId);
        const operation = (nativeInstance as T & {
          send(routingId: unknown): ZLinkBindingAsyncSendOperation;
        }).send(toNativeRoutingId(routingId));
        submitBindingSyncSend(operation, payload);
        return true;
      }
      if (hasRoutedPeer) {
        const [routingId, payload] = args as [unknown, unknown];
        peerRoutingIds.add(routingId);
        return submitBindingAsyncSend(
          (nativeInstance as T & { send(routingId: unknown): ZLinkBindingAsyncSendOperation })
            .send(toNativeRoutingId(routingId)),
          payload
        );
      }
      const [payload, flags] = args as [unknown, number | undefined];
      if (hasRequest) {
        return submitBindingAsyncSend(
          (nativeInstance as T & { send(): ZLinkBindingAsyncSendOperation }).send(),
          payload
        );
      }
      const operation = (nativeInstance as T & {
        send(): ZLinkBindingAsyncSendOperation;
      }).send();
      if ((flags ?? zlink.SendFlags.None) === zlink.SendFlags.DontWait) {
        submitBindingSyncSend(operation, payload);
        return true;
      }
      return submitBindingAsyncSend(operation, payload);
    },
    submit(routingId: unknown, payload: unknown, _timeoutMs?: number): Promise<void> {
      if (!hasStream) throw new TypeError('Async stream send requires a STREAM socket.');
      peerRoutingIds.add(routingId);
      return submitBindingAsyncSend(
        (nativeInstance as T & { send(routingId: unknown): ZLinkBindingAsyncSendOperation })
          .send(toNativeRoutingId(routingId)),
        payload
      );
    },
    request(...args: unknown[]): unknown {
      if (hasRoutedPeer) {
        const [routingId, payload, timeoutMs] = args as [unknown, unknown, number | undefined];
        peerRoutingIds.add(routingId);
        return submitBindingRequest(
          (nativeInstance as T & { request(routingId: unknown): ZLinkBindingRequestOperation })
            .request(toNativeRoutingId(routingId)),
          payload,
          timeoutMs
        );
      }
      if (hasRequest) {
        const [payload, timeoutMs] = args as [unknown, number | undefined];
        return submitBindingRequest(
          (nativeInstance as T & { request(): ZLinkBindingRequestOperation }).request(),
          payload,
          timeoutMs
        );
      }
      throw new TypeError('Backend request requires a DEALER or ROUTER socket.');
    },
    reply(...args: unknown[]): unknown {
      const [routingId, replyToken, payload] = args as [unknown, unknown, unknown];
      peerRoutingIds.add(routingId);
      const operation = (nativeInstance as T & {
        reply(routingId: unknown, replyToken: unknown): ZLinkBindingReplyOperation;
      }).reply(toNativeRoutingId(routingId), replyToken);
      return args.length < 3 ? operation : submitBindingReply(operation, payload);
    },
    recv(flags?: number): unknown {
      const received = new zlink.Received();
      let ok = false;
      try {
        ok = (nativeInstance as T & {
          recv(result: unknown, flags?: number): boolean;
        }).recv(received, flags);
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
    recvPacket(packet: unknown, flags?: number): boolean {
      if (!hasStream) throw new TypeError('Packet receive requires a STREAM socket.');
      try {
        return (nativeInstance as T & {
          recvPacket(result: unknown, flags?: number): boolean;
        }).recvPacket(packet, flags);
      } catch (error) {
        if (isRouteRecvRetryable(error)) return false;
        throw error;
      }
    },
    publish(...args: unknown[]): unknown {
      if (args.length >= 2) {
        const [topic, payload] = args as [string, unknown];
        submitBindingPublish(
          (nativeInstance as T & { publish(topic: string): ZLinkBindingPublishOperation }).publish(topic),
          payload
        );
        return;
      }
      throw new TypeError('Backend publish requires a topic and payload.');
    },
    async sendToSpot(
      targetRid: unknown,
      targetSpot: unknown,
      payload: unknown,
      flags?: number
    ): Promise<void> {
      const operation = (nativeInstance as T & {
        sendToSpot(targetRid: unknown, targetSpot: unknown): ZLinkBindingAsyncSendOperation;
      }).sendToSpot(toNativeRoutingId(targetRid), toNativeRoutingId(targetSpot));
      if ((flags ?? zlink.SendFlags.None) === zlink.SendFlags.DontWait) {
        submitBindingSyncSend(operation, payload);
        return;
      }
      await submitBindingAsyncSend(operation, payload);
    },
    requestToSpot(
      targetRid: unknown,
      targetSpot: unknown,
      payload: unknown,
      timeoutMs?: number
    ): Promise<readonly unknown[]> {
      return submitBindingRequest(
        (nativeInstance as T & {
          requestToSpot(targetRid: unknown, targetSpot: unknown): ZLinkBindingRequestOperation;
        }).requestToSpot(toNativeRoutingId(targetRid), toNativeRoutingId(targetSpot)),
        payload,
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
  return adapter as unknown as T & ZLinkBackendObject;
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
