import 'reflect-metadata';
import { startWorkflowHost } from './workflow-host-factory';

startWorkflowHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
