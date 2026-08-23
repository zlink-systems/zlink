import { expect, test } from '@playwright/test';
import { writeFile } from 'node:fs/promises';

test('one browser socket keeps authoritative state across an Ops-observed owner boundary', async ({ page }) => {
  await page.goto('/game.html');
  await page.getByLabel('Player ID').fill(`browser-${Date.now()}`);
  await page.getByRole('button', { name: 'Enter world' }).click();
  await expect(page.getByText('connected', { exact: true })).toBeVisible();
  await expect(page.getByTestId('player-position')).toHaveText('25, 25');
  await expect(page.getByTestId('player-node')).toHaveText(/zone-node-[12]/);
  const initialOwner = await page.getByTestId('player-node').textContent();

  for (const x of [30, 35, 40, 45, 50]) {
    await page.keyboard.press('ArrowRight');
    await expect(page.getByTestId('player-position')).toHaveText(`${x}, 25`);
  }

  await expect(page.getByTestId('player-zone')).toHaveText('zone-ne');
  let boundaryOwner = await page.getByTestId('player-node').textContent();
  if (boundaryOwner === initialOwner) {
    for (const x of [45, 40, 35, 30, 25]) {
      await page.keyboard.press('ArrowLeft');
      await expect(page.getByTestId('player-position')).toHaveText(`${x}, 25`);
    }
    for (const y of [30, 35, 40, 45, 50]) {
      await page.keyboard.press('ArrowDown');
      await expect(page.getByTestId('player-position')).toHaveText(`25, ${y}`);
    }
    await expect(page.getByTestId('player-zone')).toHaveText('zone-sw');
    boundaryOwner = await page.getByTestId('player-node').textContent();
  }
  expect(boundaryOwner).not.toBe(initialOwner);
  await expect(page.getByTestId('socket-state')).toHaveText('connected');

  const position = await page.getByTestId('player-position').textContent();
  await page.keyboard.press(position === '50, 25' ? 'ArrowRight' : 'ArrowDown');
  await expect(page.getByTestId('player-position')).not.toHaveText(position ?? '');
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
  const lifecycleNodeId = testInfo.config.metadata.lifecycleNodeId;
  test.skip(typeof lifecycleNodeId !== 'string', 'the language runner selects the lifecycle node from Ops');
  const lifecycleNode = page.getByTestId(`node-${lifecycleNodeId as string}`);
  await expect(lifecycleNode.getByTestId('registered-state')).toHaveAttribute('data-on', 'true');
  await expect(lifecycleNode.getByTestId('connected-state')).toHaveAttribute('data-on', 'true');

  await writeFile(marker as string, 'armed\n', 'utf8');

  // Registered is report-owned: it becomes false after the documented 15-second report TTL.
  // Connected is runtime-event-owned and may become false earlier.
  await expect(lifecycleNode.getByTestId('connected-state')).toHaveAttribute('data-on', 'false', { timeout: 20_000 });
  await expect(lifecycleNode.getByTestId('registered-state')).toHaveAttribute('data-on', 'false', { timeout: 20_000 });
});
