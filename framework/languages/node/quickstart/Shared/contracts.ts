// Request/reply contract shared by the server and client processes.
//
// `Hello` is a class because the framework derives the wire packet name from
// `payload.constructor.name`; an object literal's constructor is `Object`,
// which it refuses. `Greeting` is decoded by shape, so it stays an interface.
export class Hello {
  constructor(readonly name: string) {}
}

export interface Greeting {
  readonly text: string;
}

// The packet name the request is registered and dispatched under.
export const PacketNames = {
  hello: 'Hello'
} as const;
