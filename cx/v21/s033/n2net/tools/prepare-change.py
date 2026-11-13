from pathlib import Path
source=Path('/root/worktree/zlink-v21-s033/n2net/framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts')
s=source.read_text()
s=s.replace("const PREPARED_CHANNEL_DEADLINE = '2000-01-01T00:00:00.000Z';", """const PREPARED_CHANNEL_DEADLINE = 946684800000;
const CHANNEL_DEADLINE_TEXT_LENGTH = 24;
const MAXIMUM_CHANNEL_DEADLINE = 8640000000000000;
const MAXIMUM_FOUR_DIGIT_YEAR_DEADLINE = 253402300799999;""")
s=s.replace("const CHANNEL_CORRELATION_PREFIX = randomBytes(CHANNEL_CORRELATION_TEXT_LENGTH / 4).toString('hex');", "const CHANNEL_CORRELATION_BYTES = randomBytes(CHANNEL_CORRELATION_TEXT_LENGTH / 2);")
start=s.index('  const suffix = channelCorrelationCounter.toString(16);')
end=s.index('\n}',start)
s=s[:start]+"""  CHANNEL_CORRELATION_BYTES.writeUInt32BE(Math.floor(channelCorrelationCounter / 0x100000000), 8);
  CHANNEL_CORRELATION_BYTES.writeUInt32BE(channelCorrelationCounter % 0x100000000, 12);
  return CHANNEL_CORRELATION_BYTES.toString('hex');"""+s[end:]
s=s.replace('deadlineUnixMs === undefined ? null : new Date(deadlineUnixMs).toISOString()', 'deadlineUnixMs ?? null')
# All three encoder signatures receive an absolute Unix-millisecond value.
assert s.count('  deadline: string | null,')==3
s=s.replace('  deadline: string | null,','  deadline: number | null,')
s=s.replace('deadline.length === PREPARED_CHANNEL_DEADLINE.length','deadline <= MAXIMUM_FOUR_DIGIT_YEAR_DEADLINE')
s=s.replace("  if (deadline !== null)\n    bytes.write(deadline, prepared.deadlineOffset, PREPARED_CHANNEL_DEADLINE.length, 'ascii');", "  if (deadline !== null) writeChannelDeadline(bytes, prepared.deadlineOffset, deadline);")
s=s.replace('  writer.nullableString(deadline);','  writer.deadline(deadline);')
start=s.index('  string(value: string): void {',s.index('class ChannelHeaderWriter'))
s=s[:start]+"""  deadline(value: number | null): void {
    if (value === null) {
      this.append('null');
      return;
    }
    const length = channelDeadlineTextLength(value);
    if (this.output !== undefined) {
      this.output[this.offset] = 0x22;
      writeChannelDeadline(this.output, this.offset + 1, value);
      this.output[this.offset + length + 1] = 0x22;
    }
    this.offset += length + 2;
  }

"""+s[start:]
start=s.index('function encodeFlowOrigin(')
s=s[:start]+"""function channelDeadlineTextLength(unixMs: number): number {
  if (unixMs > MAXIMUM_CHANNEL_DEADLINE) throw new RangeError('Invalid time value');
  return unixMs <= MAXIMUM_FOUR_DIGIT_YEAR_DEADLINE ? CHANNEL_DEADLINE_TEXT_LENGTH : 27;
}

// Convert the epoch day through Gregorian 400-year eras. All intermediates
// stay exact integers over Date's positive domain; no Date or ISO string is
// needed between the absolute deadline and its final UTF-8 bytes.
function writeChannelDeadline(bytes: Buffer, offset: number, unixMs: number): void {
  const epochDay = Math.floor(unixMs / 86400000);
  const marchDay = epochDay + 719468;
  const era = Math.floor(marchDay / 146097);
  const dayOfEra = marchDay - era * 146097;
  const yearOfEra = Math.floor(
    (dayOfEra - Math.floor(dayOfEra / 1460) + Math.floor(dayOfEra / 36524) -
      Math.floor(dayOfEra / 146096)) / 365
  );
  const dayOfYear = dayOfEra -
    (365 * yearOfEra + Math.floor(yearOfEra / 4) - Math.floor(yearOfEra / 100));
  const marchMonth = Math.floor((5 * dayOfYear + 2) / 153);
  const day = dayOfYear - Math.floor((153 * marchMonth + 2) / 5) + 1;
  const month = marchMonth + (marchMonth < 10 ? 3 : -9);
  const year = era * 400 + yearOfEra + (month <= 2 ? 1 : 0);
  if (year > 9999) {
    bytes[offset++] = 0x2b;
    writeChannelDeadlineDigits(bytes, offset, year, 6);
    offset += 6;
  } else {
    writeChannelDeadlineDigits(bytes, offset, year, 4);
    offset += 4;
  }
  bytes[offset++] = 0x2d;
  writeChannelDeadlineDigits(bytes, offset, month, 2);
  offset += 2;
  bytes[offset++] = 0x2d;
  writeChannelDeadlineDigits(bytes, offset, day, 2);
  offset += 2;
  bytes[offset++] = 0x54;
  const dayMs = unixMs - epochDay * 86400000;
  writeChannelDeadlineDigits(bytes, offset, Math.floor(dayMs / 3600000), 2);
  offset += 2;
  bytes[offset++] = 0x3a;
  writeChannelDeadlineDigits(bytes, offset, Math.floor(dayMs / 60000) % 60, 2);
  offset += 2;
  bytes[offset++] = 0x3a;
  writeChannelDeadlineDigits(bytes, offset, Math.floor(dayMs / 1000) % 60, 2);
  offset += 2;
  bytes[offset++] = 0x2e;
  writeChannelDeadlineDigits(bytes, offset, dayMs % 1000, 3);
  bytes[offset + 3] = 0x5a;
}

function writeChannelDeadlineDigits(
  bytes: Buffer,
  offset: number,
  value: number,
  width: number
): void {
  for (let index = offset + width - 1; index >= offset; index -= 1) {
    bytes[index] = 0x30 + value % 10;
    value = Math.floor(value / 10);
  }
}

"""+s[start:]
Path('/root/jobs/fx/cx/v21/s033/n2net/raw/channel-envelope-candidate.ts').write_text(s)
