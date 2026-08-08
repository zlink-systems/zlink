type ZLinkInt32 = number & { readonly __zlinkInt32: unique symbol };
type ZLinkInt64 = bigint;
type ZLinkBytes = Uint8Array;

enum FixtureStatus {
  Ready = 'ready',
  Closed = 'closed'
}

interface FixtureNested {
  readonly label: string;
  readonly count?: ZLinkInt32;
}

type FixturePacket = {
  required: string;
  optional?: boolean;
  nullable: FixtureNested | null;
  nested: FixtureNested;
  values: readonly ZLinkInt32[];
  sequence: ZLinkInt64;
  bytes: ZLinkBytes;
  status: FixtureStatus;
};

class ClassPacket {
  readonly identifier!: string;
  readonly flags?: readonly boolean[];
}

const PacketNames = Object.freeze({ fixture: 'FixturePacket', classPacket: 'ClassPacket' });
void PacketNames.fixture;
void PacketNames.classPacket;

export type { FixturePacket, ClassPacket };
