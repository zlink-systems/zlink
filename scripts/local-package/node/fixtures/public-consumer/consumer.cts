import zlink = require('@zlink-systems/zlink');

const context = zlink.createContext();
const pair = zlink.createPairSocket(context);
pair.send().message('one').message('two').submit();
const received = new zlink.Received();
pair.recv(received);
pair.monitorOpen().status();
zlink.createPoller().size;
zlink.createStreamSocket(context).setPacketHandler((_source, header, body) => {
  header.size();
  body.size();
});
context.shutdown();
