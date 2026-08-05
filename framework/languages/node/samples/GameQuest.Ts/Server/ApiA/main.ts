import { bootstrapGameQuest } from '../bootstrap';

bootstrapGameQuest('api-a').catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
