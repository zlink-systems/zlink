import { startMainHost } from './main-host';

startMainHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
