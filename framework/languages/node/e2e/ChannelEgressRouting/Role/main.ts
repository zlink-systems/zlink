import 'reflect-metadata';
import { startRoleHost } from './role-host-factory';

startRoleHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
