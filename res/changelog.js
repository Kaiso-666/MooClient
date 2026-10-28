// Injected into every moomoo.io document. Only acts on plain-text documents
// (e.g. /docs/versions.txt): replaces Chromium's unreadable dark-on-dark text
// viewer with a styled document view, a back button and Escape/Alt+Left.
(() => {
  'use strict';

  const HOSTS = new Set(['moomoo.io', 'www.moomoo.io']);
  if (!HOSTS.has(location.hostname)) return;

  function isPlainText() {
    if (document.contentType !== 'text/plain') return false;
    return !!(document.body && document.body.innerText.trim());
  }

  function escapeHtml(value) {
    return String(value).replace(/[&<>"']/g, (ch) => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
    }[ch]));
  }

  function parse(raw) {
    const lines = raw.replace(/\r\n?/g, '\n').split('\n');
    const intro = [];
    const versions = [];
    let current = null;

    const header = /^(\d+\.\d+(?:\.\d+)?)\s*(?:\(([^)]*)\))?\s*:?\s*$/;
    const bullet = /^>\s*(.*)$/;
    const subBullet = /^\s+[-*]\s+(.*)$/;

    for (const line of lines) {
      const trimmed = line.trim();
      if (!trimmed) continue;

      const head = trimmed.match(header);
      if (head) {
        current = { version: head[1], date: head[2] || '', items: [] };
        versions.push(current);
        continue;
      }

      if (!current) {
        intro.push(trimmed);
        continue;
      }

      const sub = line.match(subBullet);
      if (sub) {
        current.items.push({ kind: 'sub', text: sub[1].trim() });
        continue;
      }

      const top = line.match(bullet);
      if (top) {
        current.items.push({ kind: 'bullet', text: top[1].trim() });
        continue;
      }

      // Wrapped continuation of the previous bullet/paragraph.
      if (/^\s+/.test(line) && current.items.length) {
        const prev = current.items[current.items.length - 1];
        prev.text += ' ' + trimmed;
        continue;
      }

      current.items.push({ kind: 'text', text: trimmed });
    }

    return { intro, versions };
  }

  function renderItems(items) {
    let html = '';
    for (const item of items) {
      if (item.kind === 'bullet') html += `<li>${escapeHtml(item.text)}</li>`;
      else if (item.kind === 'sub') html += `<li class="sub">${escapeHtml(item.text)}</li>`;
      else html += `<p class="note">${escapeHtml(item.text)}</p>`;
    }
    return html ? `<ul>${html}</ul>` : '';
  }

  function build(raw) {
    const { intro, versions } = parse(raw);
    const firstLine = raw.split('\n').map((l) => l.trim()).find(Boolean) || '';
    const looksLikeTitle = firstLine && firstLine.length <= 90 &&
      !firstLine.startsWith('>') && !/^\d+\.\d+/.test(firstLine);
    const title = looksLikeTitle ? firstLine : (document.title || 'Document');

    let introLines = intro;
    if (looksLikeTitle && introLines[0] === firstLine) introLines = introLines.slice(1);

    const cards = versions.map((v) => `
      <section class="version">
        <div class="version-head">
          <h2>v${escapeHtml(v.version)}</h2>
          ${v.date ? `<span class="date">${escapeHtml(v.date)}</span>` : ''}
        </div>
        ${renderItems(v.items)}
      </section>`).join('');

    const introHtml = introLines.length
      ? `<p class="intro">${introLines.map(escapeHtml).join('<br>')}</p>` : '';

    const body = versions.length
      ? cards
      : `<section class="version"><pre class="plain">${escapeHtml(raw)}</pre></section>`;

    const subtitle = versions.length
      ? `${versions.length} version${versions.length === 1 ? '' : 's'}`
      : 'Plain text document';

    return { title, subtitle, introHtml, body };
  }

  function render(raw) {
    const { title, subtitle, introHtml, body } = build(raw);

    document.title = title;

    const style = document.createElement('style');
    style.textContent = `
      :root { color-scheme: dark; --bg:#111113; --card:#1a1a1f; --line:#2a2a33;
              --text:#ececf2; --muted:#9a9aa8; --accent:#e0a45e; }
      * { box-sizing: border-box; }
      html { margin:0; min-height:100%; background:var(--bg); color:var(--text); }
      body { margin:0; min-height:100%; background:transparent; color:var(--text);
             font: 15px/1.6 "Segoe UI", system-ui, sans-serif; padding: 0 0 64px;
             white-space: normal; }
      .bg { position: fixed; inset: 0; z-index: 0; pointer-events: none;
            background: url('https://mooclient.local/changelog-bg.png') center center / cover no-repeat; }
      .bg::after { content:''; position:absolute; inset:0;
                   background: linear-gradient(180deg, rgba(12,12,14,.18), rgba(12,12,14,.34)); }
      .top, .wrap { position: relative; z-index: 1; }
      .top { position: sticky; top:0; z-index:2; display:flex; align-items:center;
             gap:16px; padding: 22px 24px 24px; backdrop-filter: blur(12px);
             -webkit-backdrop-filter: blur(12px);
             background: linear-gradient(180deg, rgba(17,17,19,.94),
                         rgba(17,17,19,.72) 72%, rgba(17,17,19,.35));
             border-bottom: 1px solid rgba(255,255,255,.06); }
      .back { display:inline-flex; align-items:center; gap:8px; border:1px solid var(--line);
              background:#17171c; color:var(--text); font:inherit; padding:8px 14px;
              border-radius:10px; cursor:pointer; }
      .back:hover { background:#202028; border-color:#3a3a46; }
      .back svg { width:16px; height:16px; }
      .crumb { color:var(--muted); font-size:13px; overflow:hidden;
               text-overflow:ellipsis; white-space:nowrap; }
      .wrap { max-width: 908px; margin: 0 auto; padding: 0 24px; }
      h1 { font-size: 30px; line-height:1.2; margin: 18px 0 6px; letter-spacing:-0.02em;
           text-shadow: 0 2px 16px rgba(0,0,0,.85); }
      .subtitle { color:#d6d6e0; margin: 0 0 26px;
                  text-shadow: 0 1px 12px rgba(0,0,0,.9); }
      .intro { color:var(--muted); background:rgba(26,26,31,.67);
               backdrop-filter: blur(12px) saturate(1.3) brightness(1.12);
               -webkit-backdrop-filter: blur(12px) saturate(1.3) brightness(1.12);
               border:1px solid rgba(255,255,255,.10); border-radius:14px;
               padding:14px 18px; margin: 0 0 22px; }
      .version { background:rgba(26,26,31,.67); border:1px solid rgba(255,255,255,.10);
                 backdrop-filter: blur(12px) saturate(1.3) brightness(1.12);
                 -webkit-backdrop-filter: blur(12px) saturate(1.3) brightness(1.12);
                 border-radius:16px; padding: 18px 20px; margin: 0 0 14px; }
      .version-head { display:flex; align-items:baseline; gap:12px; margin-bottom: 8px; }
      .version h2 { font-size: 18px; margin:0; color:var(--accent); }
      .date { color:#c2c2cf; font-size:13px; }
      .version ul { margin: 0; padding-left: 20px; }
      .version li { margin: 5px 0; }
      .version li.sub { margin-left: 18px; color:#c9c9d4; list-style: circle; }
      .version li::marker { color: var(--accent); }
      .note { color:var(--muted); margin: 8px 0; }
      .plain { white-space: pre-wrap; font: 13px/1.6 ui-monospace, Consolas, monospace;
               color:#d6d6e0; margin:0; }
      @media (max-width: 560px) {
        .top { padding: 18px 14px 20px; }
        .wrap { padding: 0 14px; }
        h1 { font-size: 24px; }
      }
    `;
    if (document.head) document.head.appendChild(style);

    document.body.innerHTML = `
      <div class="bg" aria-hidden="true"></div>
      <div class="top">
        <button class="back" id="mooclient-back" type="button" aria-label="Back to the game">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"
               stroke-linecap="round" stroke-linejoin="round">
            <path d="M19 12H5"/><path d="m12 19-7-7 7-7"/>
          </svg>
          Back
        </button>
        <span class="crumb">moomoo.io${escapeHtml(location.pathname)}</span>
      </div>
      <main class="wrap">
        <h1>${escapeHtml(title)}</h1>
        <p class="subtitle">${escapeHtml(subtitle)}</p>
        ${introHtml}
        ${body}
      </main>
    `;

    const goBack = () => {
      if (history.length > 1) {
        history.back();
        setTimeout(() => {
          if (!document.hidden) location.href = 'https://moomoo.io/';
        }, 450);
      } else {
        location.href = 'https://moomoo.io/';
      }
    };

    const back = document.getElementById('mooclient-back');
    if (back) back.addEventListener('click', goBack);
    window.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' || (event.altKey && event.key === 'ArrowLeft')) {
        event.preventDefault();
        goBack();
      }
    });
  }

  const boot = () => {
    if (!isPlainText()) return;
    try {
      render(document.body.innerText);
    } catch (error) {
      console.error('MooClient document view failed', error);
    }
  };

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot, { once: true });
  } else {
    boot();
  }
})();
