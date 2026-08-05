import { startMultiNodeHost } from './multi-node-host-factory';

startMultiNodeHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
