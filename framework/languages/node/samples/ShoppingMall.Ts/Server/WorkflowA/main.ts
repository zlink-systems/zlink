import { bootstrapShoppingMall } from '../bootstrap';

bootstrapShoppingMall('workflow-a').catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
