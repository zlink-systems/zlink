export function isEvent(line: string, runId: string, topic: string): boolean {
  return line.includes('event|') && line.includes(`run=${runId}`) && line.includes(`topic=${topic}`);
}

export function extractInt(line: string, key: string): number {
  const marker = `${key}=`;
  const start = line.indexOf(marker);
  if (start < 0) {
    return 0;
  }
  const valueStart = start + marker.length;
  const end = line.indexOf('|', valueStart);
  return Number.parseInt(end < 0 ? line.slice(valueStart) : line.slice(valueStart, end), 10);
}

export function commonContiguousSequence(
  snapshots: readonly (readonly string[])[],
  runId: string,
  topic: string,
  min: number,
  max: number,
  valuePrefix: string
): readonly number[] {
  const sequences = snapshots.map((lines) => lines
      .filter((line) => isEvent(line, runId, topic))
      .filter((line) => {
        const sequence = extractInt(line, 'seq');
        return sequence >= min
          && sequence <= max
          && line.includes(`value=${valuePrefix}${sequence}`);
      })
      .map((line) => extractInt(line, 'seq'))
  );
  if (sequences.length === 0) {
    return [];
  }

  let best: number[] = [];
  for (let start = 0; start < sequences[0].length; start += 1) {
    const first = sequences[0][start];
    const positions = sequences.map((sequence) => sequence.indexOf(first));
    if (positions.some((position) => position < 0)) {
      continue;
    }
    const current = [first];
    for (let index = start + 1; index < sequences[0].length; index += 1) {
      const candidate = sequences[0][index];
      if (candidate !== current[current.length - 1] + 1) {
        break;
      }
      const nextPositions = sequences.map((sequence, sequenceIndex) =>
        sequence.findIndex((value, position) => position > positions[sequenceIndex] && value === candidate)
      );
      if (nextPositions.some((position) => position < 0)) {
        break;
      }
      current.push(candidate);
      positions.splice(0, positions.length, ...nextPositions);
    }
    if (current.length > best.length) {
      best = current;
    }
  }
  return best;
}
