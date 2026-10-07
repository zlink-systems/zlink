// --8<-- [start:protobuf-imports]
import { createZlinkStreamProtobufCodec } from '@zlink-systems/framework-codec-protobuf';
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
  // Ping is the fallback when no runtime decoding type is supplied.
  const connector = zlinkStreamConnectorFactory.create({
    endpoint,
    codec: createZlinkStreamProtobufCodec(Ping),
    dispatchMode: ZlinkStreamDispatchMode.Immediate
  });
  // --8<-- [end:protobuf-register]

  const received: string[] = [];
  let resolvePush!: () => void;
  let resolvePong!: (rank: number) => void;
  let rejectPong!: (error: Error) => void;
  let rejectPush!: (error: Error) => void;
  const pushed = new Promise<void>((resolve, reject) => {
    resolvePush = resolve;
    rejectPush = reject;
  });
  const pongPushed = new Promise<number>((resolve, reject) => {
    resolvePong = resolve;
    rejectPong = reject;
  });
  const errors = connector.onErrorReceived((error) => {
    rejectPush(new Error(error.message));
    rejectPong(new Error(error.message));
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
  // Without a runtime type, the codec falls back to Ping.
  const byName = connector.on<messages.tutorial.Ping>('Ping', handlePing);
  // The same codec decodes Pong with the handler's generated constructor.
  const byPong = connector.on<messages.tutorial.Pong>(Pong, (message) => {
    if (!(message.payload instanceof Pong)) {
      rejectPong(new Error('Pong handler received a payload with the wrong generated type.'));
      return;
    }
    resolvePong(message.payload.rank);
  });
  // --8<-- [end:typed-receive]
  try {
    await connector.connect();
    // --8<-- [start:protobuf-send]
    // Use a generated instance so its constructor supplies the packet name.
    await connector.send(new Ping({ text: 'hello' })).submit();
    // The peer sends both push types after receiving Ping.
    const [, pushRank] = await Promise.all([pushed, pongPushed]);
    await connector.send(new Pong({ rank: 3 })).submit();
    // --8<-- [end:protobuf-send]
    // --8<-- [start:protobuf-request]
    // Pass the generated constructor to the existing codec's decode method.
    const reply = await connector.request(new Ping({ text: 'rank' })).submit(Pong);
    // --8<-- [end:protobuf-request]
    // --8<-- [start:protobuf-request-callback]
    const callbackReply = await new Promise<messages.tutorial.Pong>((resolve, reject) => {
      connector.request(new Ping({ text: 'rank' })).submitCallback(Pong, (result) => {
        if (result.isSuccess) resolve(result.value!);
        else reject(new Error(result.error!.message));
      });
    });
    // --8<-- [end:protobuf-request-callback]
    if (
      !(reply instanceof Pong) ||
      !(callbackReply instanceof Pong) ||
      callbackReply.rank !== 7 ||
      received.length !== 3 ||
      received.some((text) => text !== 'hello') ||
      pushRank !== 3 ||
      reply.rank !== 7
    ) {
      throw new Error('Protobuf tutorial response did not match the expected push and reply.');
    }
    return `protobuf: Ping=${received[0]}, Pong.rank=${pushRank}, reply.rank=${reply.rank}`;
  } finally {
    errors.dispose();
    byPong.dispose();
    byType.dispose();
    byNameAndType.dispose();
    byName.dispose();
    await connector.close();
  }
}
