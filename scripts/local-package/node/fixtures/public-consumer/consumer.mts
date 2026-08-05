import {
  Message,
  Received,
  createContext,
  createPairSocket,
  createPoller,
  createStreamSocket
} from '@zlink-systems/zlink';

const context = createContext();
const pair = createPairSocket(context);
pair.send().message(Message.from('one')).message('two').submit();
const received = new Received();
pair.recv(received);
pair.monitorOpen().status();
createPoller().size;
createStreamSocket(context).setPacketHandler((_source, header, body) => {
  header.size();
  body.size();
});
context.shutdown();
