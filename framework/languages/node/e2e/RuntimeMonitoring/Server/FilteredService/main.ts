import { startServiceHost } from '../Service/service-host-factory';

startServiceHost({ socketFilter: true, profileServer: true }).catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
