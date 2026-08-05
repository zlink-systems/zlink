import { startGatewayHost } from './gateway-host-factory';

startGatewayHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
