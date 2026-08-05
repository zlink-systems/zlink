import { startServiceHost } from './service-host-factory';

startServiceHost({ profileServer: false }).catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
