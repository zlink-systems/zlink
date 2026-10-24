import { runHost, failHost } from '../shared/hosting';
import { Names } from '../shared/constants';
import { LobbyHttp } from './http';
runHost((_builder, _config, mesh) => {
  mesh.objects().client();
  let http: LobbyHttp;
  return {
    providers: [LobbyHttp],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.readyPeerCount > 0,
    async initialize(app) {
      http = app.get(LobbyHttp);
      await http.start();
    },
    endpoint: () => http.endpoint,
    async release() {
      await http?.close();
    }
  };
}).catch(failHost);
