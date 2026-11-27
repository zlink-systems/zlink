export function selectPair<T>(waiting: Iterable<T>): [T, T] | undefined {
  // A rating rule can use the first waiting ticket to determine the eligible range.
  let first: T | undefined;
  for (const ticket of waiting) {
    if (first !== undefined) return [first, ticket];
    first = ticket;
  }
  return undefined;
}
