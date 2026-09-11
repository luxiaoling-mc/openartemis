// Minimal Chrome DevTools Protocol client (test helper, not shipped).
// Reads the page's log panel text from a headless Chrome started with
// --remote-debugging-port=<port>.
//
//   node tools/cdp_dump.js <port> [expression]
//
// Default expression: the openartemis web shell's log panel innerText.
const port = process.argv[2] || '9222';
const expr = process.argv[3] ||
  "(() => { const p = document.getElementById('log-panel'); return p ? p.innerText : '[no log panel]'; })()";

const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const page = list.find((t) => t.type === 'page');
if (!page) {
  console.error('no page target:', JSON.stringify(list.map((t) => t.type)));
  process.exit(2);
}
const ws = new WebSocket(page.webSocketDebuggerUrl);
const done = new Promise((resolve) => {
  ws.addEventListener('open', () => {
    ws.send(JSON.stringify({
      id: 1,
      method: 'Runtime.evaluate',
      params: { expression: expr, returnByValue: true, awaitPromise: true },
    }));
  });
  ws.addEventListener('message', (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id === 1) {
      const r = msg.result && msg.result.result;
      console.log(r && r.value !== undefined ? r.value : JSON.stringify(msg));
      resolve();
    }
  });
  ws.addEventListener('error', (e) => { console.error('ws error', e.message || e); resolve(); });
});
await done;
ws.close();
process.exit(0);
