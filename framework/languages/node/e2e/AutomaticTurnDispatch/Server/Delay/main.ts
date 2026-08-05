import { startDelayHost } from './delay-host-factory';

startDelayHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
