import { startSessionHost } from './session-host-factory';

startSessionHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
