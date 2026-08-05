import { startCodecRequester } from './codec-requester-host-factory';

startCodecRequester().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
