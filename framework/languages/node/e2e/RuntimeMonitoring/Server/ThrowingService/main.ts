import { startServiceHost } from '../Service/service-host-factory';

startServiceHost({ throwMonitor: true, profileServer: true }).catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
