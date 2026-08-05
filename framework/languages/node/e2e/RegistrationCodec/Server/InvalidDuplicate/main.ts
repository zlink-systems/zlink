import { startInvalidDuplicate } from './invalid-duplicate-host-factory';

startInvalidDuplicate().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
