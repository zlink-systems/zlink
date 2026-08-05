import type {
  Type,
  ZLinkDispatchOptions,
  ZLinkMessageFlowObserver,
  ZLinkRuntimeErrorSink
} from '../../contracts';

const observerTypes = new WeakMap<object, Type<ZLinkMessageFlowObserver>>();
const runtimeErrorSinkTypes = new WeakMap<object, Type<ZLinkRuntimeErrorSink>>();

export function setDispatchObserverType(
  dispatch: ZLinkDispatchOptions,
  observerType: Type<ZLinkMessageFlowObserver>
): void {
  observerTypes.set(dispatch, observerType);
}

export function getDispatchObserverType(
  dispatch: ZLinkDispatchOptions | undefined
): Type<ZLinkMessageFlowObserver> | undefined {
  return dispatch === undefined ? undefined : observerTypes.get(dispatch);
}

export function setRuntimeErrorSinkType(
  dispatch: ZLinkDispatchOptions,
  sinkType: Type<ZLinkRuntimeErrorSink>
): void {
  runtimeErrorSinkTypes.set(dispatch, sinkType);
}

export function getRuntimeErrorSinkType(
  dispatch: ZLinkDispatchOptions | undefined
): Type<ZLinkRuntimeErrorSink> | undefined {
  return dispatch === undefined ? undefined : runtimeErrorSinkTypes.get(dispatch);
}
