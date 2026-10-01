export type Type<T = unknown> = new (...args: never[]) => T;
export type RoutingId = string;
/** Global logical Actor identity. It does not encode a Mesh or owner route. */
export type ActorId = string;
/** Global logical Spot identity. It is not a transport RoutingId. */
export type SpotId = string;

export const ZLINK_MAX_ACTOR_ID_BYTES = 255;
export const ZLINK_MAX_SPOT_ID_BYTES = 255;
export const ZLINK_MAX_ROUTING_ID_BYTES = 255;
export const ZLINK_MAX_STABLE_TYPE_BYTES = 255;
export const ZLINK_MAX_MESH_NAME_BYTES = 255;
export const ZLINK_MAX_IDENTITY_TEXT_BYTES = 255;
