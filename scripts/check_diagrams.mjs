// Проверка синтаксиса схем Mermaid в каталоге docs/uml.
// Установка: npm install mermaid@11 jsdom
// Запуск:    node scripts/check_diagrams.mjs docs/uml
import { JSDOM } from 'jsdom';
import fs from 'fs'; import path from 'path';
const dom = new JSDOM('<!DOCTYPE html><body></body>');
globalThis.window = dom.window; globalThis.document = dom.window.document;
Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true });
globalThis.DOMPurify = undefined;
const mermaid = (await import('mermaid')).default;
mermaid.initialize({ startOnLoad: false });
const dir = process.argv[2]; let bad = 0, total = 0;
for (const f of fs.readdirSync(dir).filter(f => f.endsWith('.md')).sort()) {
  const text = fs.readFileSync(path.join(dir, f), 'utf8');
  const blocks = [...text.matchAll(/```mermaid\n([\s\S]*?)```/g)].map(m => m[1]);
  for (const [i, b] of blocks.entries()) {
    total++;
    try { const r = await mermaid.parse(b); console.log('ok ', f, i + 1, r.diagramType); }
    catch (e) { bad++; console.log('BAD', f, i + 1, String(e.message || e).split('\n').slice(0, 4).join(' | ')); }
  }
}
console.log(`total ${total}, bad ${bad}`); process.exit(bad ? 1 : 0);
