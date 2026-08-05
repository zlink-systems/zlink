import { startPlayHost } from './play-host-factory';

startPlayHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
