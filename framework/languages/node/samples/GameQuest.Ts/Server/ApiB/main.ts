import { bootstrapGameQuest } from '../bootstrap';

bootstrapGameQuest('api-b').catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
