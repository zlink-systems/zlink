export type Type<T = unknown> = new (...args: never[]) => T;
export type RoutingId = string;
/** Global logical Actor identity. It does not encode a Mesh or owner route. */
export type ActorId = string;
/** Global logical Spot identity. It is not a transport RoutingId. */
export type SpotId = string;
