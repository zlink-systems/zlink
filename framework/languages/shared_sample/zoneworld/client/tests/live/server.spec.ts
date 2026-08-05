import { expect, test } from '@playwright/test';
import { writeFile } from 'node:fs/promises';

test('one browser socket keeps authoritative state across a node transfer', async ({ page }) => {
  await page.goto('/game.html');
  await page.getByLabel('Player ID').fill(`browser-${Date.now()}`);
  await page.getByRole('button', { name: 'Enter world' }).click();
  await expect(page.getByText('connected', { exact: true })).toBeVisible();
  await expect(page.getByTestId('player-position')).toHaveText('25, 25');

  for (const x of [30, 35, 40, 45, 50]) {
    await page.keyboard.press('ArrowRight');
    await expect(page.getByTestId('player-position')).toHaveText(`${x}, 25`);
  }

  await expect(page.getByTestId('player-zone')).toHaveText('zone-ne');
  await expect(page.getByTestId('player-node')).toHaveText('zone-node-2');
  await expect(page.getByTestId('transfer-state')).toContainText('Actor transferred');
  await expect(page.getByTestId('socket-state')).toHaveText('connected');

  await page.keyboard.press('ArrowRight');
  await expect(page.getByTestId('player-position')).toHaveText('55, 25');
});

test('operations page applies owner-targeted maintenance and diagnostics', async ({ page }) => {
  await page.goto('/ops.html');
  await page.getByRole('button', { name: 'Connect console' }).click();
  const west = page.getByTestId('node-zone-node-1');
  const east = page.getByTestId('node-zone-node-2');
  await expect(west).toContainText('registered');
  await expect(east).toContainText('connected');

  await expect(west.getByRole('button', { name: 'Maintain' })).toBeVisible();
  await expect(east.getByRole('button', { name: 'Maintain' })).toBeVisible();

  await east.getByRole('button', { name: 'Maintain' }).click();
  await expect(east.getByRole('button', { name: 'Restore' })).toBeVisible();
  await expect(west.getByRole('button', { name: 'Maintain' })).toBeVisible();

  try {
    await east.getByRole('button', { name: 'Diagnose' }).click();
    await expect(page.getByTestId('diagnostics-card')).toContainText('zone-node-2');
    await expect(page.getByTestId('diagnostics-card')).toContainText('zone-ne');
    await expect(page.getByTestId('diagnostics-card')).toContainText('enabled');
  } finally {
    await east.getByRole('button', { name: 'Restore' }).click();
    await expect(east.getByRole('button', { name: 'Maintain' })).toBeVisible();
  }
});

test('operations page receives node loss through server push', async ({ page }, testInfo) => {
  const marker = testInfo.config.metadata.lifecycleMarker;
  test.skip(typeof marker !== 'string', 'the language runner owns the node lifecycle');

  await page.goto('/ops.html');
  await page.getByRole('button', { name: 'Connect console' }).click();
  const east = page.getByTestId('node-zone-node-2');
  await expect(east.getByTestId('registered-state')).toHaveAttribute('data-on', 'true');
  await expect(east.getByTestId('connected-state')).toHaveAttribute('data-on', 'true');

  await writeFile(marker as string, 'armed\n', 'utf8');

  // Zone nodes use the documented 30-second owner lease. Allow its bounded expiry and one
  // reconcile window instead of assuming Playwright's unrelated five-second assertion default.
  await expect(east.getByTestId('registered-state')).toHaveAttribute('data-on', 'false', { timeout: 40_000 });
  await expect(east.getByTestId('connected-state')).toHaveAttribute('data-on', 'false', { timeout: 40_000 });
});
