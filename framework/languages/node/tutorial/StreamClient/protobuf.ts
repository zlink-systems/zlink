// --8<-- [start:protobuf-imports]
import { createZlinkStreamProtobufCodec, fromProto } from '@zlink-systems/framework-codec-protobuf';
import {
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  type ZlinkStreamMessage
} from '@zlink-systems/stream-connector';
import messages from './generated/messages.cjs';

const { Ping, Pong } = messages.tutorial;
// --8<-- [end:protobuf-imports]

export async function runProtobuf(endpoint: string): Promise<string> {
  // --8<-- [start:protobuf-register]
  // This codec uses Ping for every encoded and decoded payload.
  const connector = zlinkStreamConnectorFactory.create({
    endpoint,
    codec: createZlinkStreamProtobufCodec(Ping),
    dispatchMode: ZlinkStreamDispatchMode.Immediate
  });
  // --8<-- [end:protobuf-register]

  const received: string[] = [];
  let resolvePush!: () => void;
  const pushed = new Promise<void>((resolve) => {
    resolvePush = resolve;
  });
  const handlePing = (message: ZlinkStreamMessage<messages.tutorial.Ping>): void => {
    received.push(message.payload.text);
    resolvePush();
  };
  // --8<-- [start:typed-receive]
  // The constructor supplies the packet name and runtime decoding type.
  const byType = connector.on<messages.tutorial.Ping>(Ping, handlePing);
  // An explicit wire name can differ from the generated class name.
  const byNameAndType = connector.on<messages.tutorial.Ping>('Ping', handlePing, Ping);
  // With this fixed-type codec, a name alone also decodes as Ping.
  const byName = connector.on<messages.tutorial.Ping>('Ping', handlePing);
  // --8<-- [end:typed-receive]
  try {
    await connector.connect();
    // --8<-- [start:protobuf-send]
    // Use a generated instance so its constructor supplies the packet name.
    await connector.send(new Ping({ text: 'hello' })).submit();
    // --8<-- [end:protobuf-send]
    await pushed;
    // --8<-- [start:protobuf-request]
    // submit<Pong>() does not pass Pong to the codec at runtime.
    const encodedReply = await connector.request(new Ping({ text: 'rank' })).submitEncoded();
    const reply = fromProto(encodedReply, Pong);
    // --8<-- [end:protobuf-request]
    if (received.length !== 3 || received.some((text) => text !== 'hello') || reply.rank !== 7) {
      throw new Error('Protobuf tutorial response did not match the expected push and reply.');
    }
    return `protobuf: push=${received[0]}, reply.rank=${reply.rank}`;
  } finally {
    byType.dispose();
    byNameAndType.dispose();
    byName.dispose();
    await connector.close();
  }
}
