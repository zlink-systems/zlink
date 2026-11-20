// Deadline text is `new Date(ms).toISOString()`. A deadline is set for every
// request, but the date and time down to the second repeat for every message
// sent in that second, so only the millisecond digits are formatted per call.
const MAX_ISO_FOUR_DIGIT_YEAR_MS = 253402300799999; // 9999-12-31T23:59:59.999Z

let cachedSecond = -1;
let cachedSecondText = '';

/** Same text as `new Date(unixMs).toISOString()` for a non-negative integer millisecond. */
export function formatDeadlineText(unixMs: number): string {
  if (!(unixMs >= 0 && unixMs <= MAX_ISO_FOUR_DIGIT_YEAR_MS) || !Number.isInteger(unixMs)) {
    return new Date(unixMs).toISOString();
  }
  const second = Math.floor(unixMs / 1000);
  if (second !== cachedSecond) {
    cachedSecondText = new Date(second * 1000).toISOString().slice(0, 20);
    cachedSecond = second;
  }
  const millisecond = unixMs - second * 1000;
  return (
    cachedSecondText + (millisecond < 10 ? '00' : millisecond < 100 ? '0' : '') + millisecond + 'Z'
  );
}
