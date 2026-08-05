import type { ZLinkRouteChannelOptions, ZLinkRouteMeshChannelOptions } from './RegistrationTypes';

export interface RouteChannelInternalState {
  readonly transportDeclared?: boolean;
  readonly clientEnabled?: boolean;
}

export function markRouteTransportDeclared(routeChannel: object): void {
  defineRouteInternalFlag(routeChannel, 'transportDeclared');
}

export function markRouteClientEnabled(routeChannel: object): void {
  defineRouteInternalFlag(routeChannel, 'clientEnabled');
}

export function copyRouteInternalState(source: object, target: object): void {
  const state = routeInternalState(source);
  if (state.transportDeclared === true) {
    markRouteTransportDeclared(target);
  }
  if (state.clientEnabled === true) {
    markRouteClientEnabled(target);
  }
}

export function isRouteTransportDeclared(routeChannel: ZLinkRouteChannelOptions): boolean {
  return routeInternalState(routeChannel).transportDeclared === true;
}

export function isRouteClientEnabled(
  routeChannel: ZLinkRouteChannelOptions | ZLinkRouteMeshChannelOptions
): boolean {
  return routeInternalState(routeChannel).clientEnabled === true;
}

function routeInternalState(routeChannel: object): RouteChannelInternalState {
  return routeChannel as RouteChannelInternalState;
}

function defineRouteInternalFlag(
  routeChannel: object,
  key: keyof RouteChannelInternalState
): void {
  Object.defineProperty(routeChannel, key, {
    value: true,
    configurable: true,
    enumerable: false,
    writable: true
  });
}
