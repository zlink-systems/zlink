import { startJsonOnlyPeer } from './json-only-host-factory';

startJsonOnlyPeer().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
