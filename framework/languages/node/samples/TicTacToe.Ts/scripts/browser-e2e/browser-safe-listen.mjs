const minimumPort = 41_000;
const maximumPort = 60_999;
const portCount = maximumPort - minimumPort + 1;

export async function listenOnBrowserSafeLoopbackPort(server) {
  const firstPort = minimumPort + Math.floor(Math.random() * portCount);
  for (let offset = 0; offset < portCount; offset += 1) {
    const port = minimumPort + ((firstPort - minimumPort + offset) % portCount);
    const error = await tryListen(server, port);
    if (error === undefined) return;
    if (error.code !== 'EADDRINUSE' && error.code !== 'EACCES') throw error;
  }
  throw new Error('No browser-safe loopback port is available.');
}

function tryListen(server, port) {
  return new Promise((resolve) => {
    const onError = (error) => {
      server.off('listening', onListening);
      resolve(error);
    };
    const onListening = () => {
      server.off('error', onError);
      resolve(undefined);
    };
    server.once('error', onError);
    server.once('listening', onListening);
    server.listen(port, '127.0.0.1');
  });
}
