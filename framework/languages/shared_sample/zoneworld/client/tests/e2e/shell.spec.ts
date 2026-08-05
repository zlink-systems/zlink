import { expect, test } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.route('**/config.json', (route) => route.fulfill({
    contentType: 'application/json',
    body: JSON.stringify({
      gateway: 'ws://127.0.0.1:48080',
      ops: 'ws://127.0.0.1:48090',
    }),
  }));
});

test('game shell exposes the authoritative world controls', async ({ page }) => {
  await page.goto('/game.html');
  await expect(page.getByRole('heading', { name: 'ZoneWorld', exact: true })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Enter world' })).toBeVisible();
  await expect(page.getByLabel('ZoneWorld map')).toBeVisible();
});

test('operations shell starts without polling or an implicit connection', async ({ page }) => {
  await page.goto('/ops.html');
  await expect(page.getByRole('heading', { name: 'ZoneWorld Ops' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Connect console' })).toBeVisible();
  await expect(page.getByText('No polling. Every transition arrives from the server.')).toBeVisible();
});
