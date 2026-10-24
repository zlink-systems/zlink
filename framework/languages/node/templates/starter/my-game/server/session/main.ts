import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { SessionFactory } from './session';
runHost((builder, config) => {
  if (config.streamEndpoint === null) throw new Error('Session requires streamEndpoint');
  builder.addStreamNode(Names.stream).bind(config.streamEndpoint).registerSession(SessionFactory);
  return {
    providers: [SessionFactory],
    listener: { kind: 'stream', name: Names.stream },
    ready: () => true
  };
}).catch(failHost);
