import { useState } from 'preact/hooks';

export function AnnounceForm({ onAnnounce, result }: { onAnnounce: (text: string) => Promise<void>; result: string | null }) {
  const [text, setText] = useState('Server maintenance starts in 10 minutes');
  return (
    <section class="panel announce-panel">
      <p class="eyebrow">Fanout</p><h2>World announcement</h2>
      <form onSubmit={(event) => { event.preventDefault(); void onAnnounce(text); }}>
        <input value={text} onInput={(event) => setText(event.currentTarget.value)} aria-label="Announcement text" />
        <button>Publish</button>
      </form>
      {result !== null && <p class="muted">Published as <code>{result}</code></p>}
    </section>
  );
}
