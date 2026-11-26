import { addGameMesh, runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { SessionFactory } from './session';
runHost((builder, config) => {
  const mesh = addGameMesh(builder, config);
  if (config.streamEndpoint === null) throw new Error('Session requires streamEndpoint');
  mesh.objects().client();
  builder
    .addStreamNode(Names.stream)
    .enableActorDispatch()
    .bind(config.streamEndpoint)
    .registerSession(SessionFactory);
  return {
    providers: [SessionFactory],
    listener: { kind: 'stream', name: Names.stream },
    ready: (status) => status.readyPeerCount > 0
  };
}).catch(failHost);
