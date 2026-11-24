import { runExample } from './example.js';

const form = document.querySelector<HTMLFormElement>('#connect')!;
const endpoint = document.querySelector<HTMLInputElement>('#endpoint')!;
const output = document.querySelector<HTMLPreElement>('#output')!;
const run = document.querySelector<HTMLButtonElement>('#run')!;

form.addEventListener('submit', async (event) => {
  event.preventDefault();
  run.disabled = true;
  output.textContent = '';
  const report = (line: string): void => {
    output.textContent += line + '\n';
  };
  try {
    await runExample(endpoint.value, report);
  } catch (error) {
    report(String(error));
  } finally {
    run.disabled = false;
  }
});
