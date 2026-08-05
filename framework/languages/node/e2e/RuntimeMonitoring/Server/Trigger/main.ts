import { startTriggerHost } from './trigger-host-factory';

startTriggerHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
