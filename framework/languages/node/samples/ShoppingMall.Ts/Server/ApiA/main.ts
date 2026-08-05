import { bootstrapShoppingMall } from '../bootstrap';

bootstrapShoppingMall('api-a').catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
