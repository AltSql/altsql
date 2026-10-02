/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
// Runs the AltSql DB demo in headless Chromium, desktop and phone: the four
// steps of "Try this", every SQL preset, both kinds of power cut a few times,
// and Play for a moment; checks what the page says at each step, that nothing
// overflows the phone's width, collects page errors and takes screenshots.
// Usage: NODE_PATH=$(npm root -g) node tools/headless.js URL outdir
const { chromium } = require('playwright');
const url = process.argv[2], out = process.argv[3] || 'build/shots';
(async () => {
  const browser = await chromium.launch({ executablePath: '/opt/pw-browsers/chromium-1194/chrome-linux/chrome' }).catch(() => chromium.launch());
  const errors = [];
  const expect = (cond, what) => { if (!cond) errors.push(what); };
  for (const vp of [{ name: 'desktop', width: 1280, height: 900 }, { name: 'phone', width: 390, height: 844 }]) {
    const page = await browser.newPage({ viewport: { width: vp.width, height: vp.height }, deviceScaleFactor: vp.name === 'phone' ? 2 : 1 });
    page.on('pageerror', e => errors.push(vp.name + ': ' + e.message));
    page.on('console', m => { if (m.type() === 'error') errors.push(vp.name + ' console: ' + m.text()); });
    const attr = async a => page.getAttribute('body', a);
    const text = async id => (await page.textContent(id)).trim();
    const overflow = async () => page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    const t0 = Date.now();
    await page.goto(url, { waitUntil: 'load' });
    await page.waitForSelector('body[data-state="ready"]', { timeout: 60000 });
    console.log(vp.name, 'ready in', Date.now() - t0, 'ms |', await text('#engine-state'));
    await page.screenshot({ path: `${out}/${vp.name}-start.png`, fullPage: true });

    await page.click('#go-run');
    await page.waitForSelector('body[data-run]');
    console.log(vp.name, 'step 1 |', await text('#step-note'));
    expect(+(await attr('data-run')) >= 5040, vp.name + ': step 1 rows');

    await page.click('#go-sql');
    await page.waitForSelector('body[data-sql]');
    console.log(vp.name, 'step 2 |', await attr('data-sql'), '|', await text('#sql-meta'), '|', await text('#plan'));
    expect((await attr('data-sql')).startsWith('range per device:12:'), vp.name + ': step 2 plan or rows');

    await page.click('#go-direct');
    await page.waitForSelector('body[data-direct]');
    console.log(vp.name, 'step 3 |', await attr('data-direct'), '|', await text('#d-time'), '|', await text('#s-time'));
    expect(await attr('data-direct') === 'same:ok', vp.name + ': step 3 bytes');
    await page.screenshot({ path: `${out}/${vp.name}-direct.png`, fullPage: true });

    await page.click('#go-cut');
    await page.waitForSelector('body[data-cut]');
    console.log(vp.name, 'step 4 |', await attr('data-cut'), '|', await text('#step-note'));
    expect(/:0:complete:complete$/.test(await attr('data-cut')), vp.name + ': step 4 cut');

    for (let i = 0; i < 6; i++) await page.click(i % 2 ? '#btn-cut-pages' : '#btn-cut-header');
    const log = await page.$$eval('#cutlog li', ls => ls.map(l => l.textContent));
    const kept = log.map(l => /the commit under way/.test(l) ? 'under way' : 'last');
    console.log(vp.name, 'cuts |', log.length, 'logged |', kept.join(', '));
    expect(log.length === 7 && log.every(l => /all there, once/.test(l) && /complete trees/.test(l)), vp.name + ': a cut lost readings or a tree');

    const presets = await page.$$('#presets button');
    for (const b of presets) {
      await b.click();
      const meta = await text('#sql-meta'), plan = (await page.isVisible('#plan')) ? await text('#plan') : 'no plan';
      console.log(vp.name, '  preset |', meta, '|', plan);
      expect(/rows? ?|read/.test(meta) && !/error/i.test(meta), vp.name + ': preset failed: ' + meta);
    }
    await page.fill('#sql', 'SELECT * FROM nosuch');
    await page.click('#btn-sql');
    expect(/no such table/.test(await text('#result')), vp.name + ': an error is shown');

    await page.click('#btn-play');
    await page.waitForTimeout(1200);
    await page.click('#btn-play');
    console.log(vp.name, 'played |', await text('#clock'));
    const ov = await overflow();
    expect(ov <= 0, vp.name + ': sideways overflow ' + ov + ' px');
    await page.screenshot({ path: `${out}/${vp.name}-end.png`, fullPage: true });
    await page.close();
  }
  await browser.close();
  if (errors.length) { console.log('PROBLEMS:\n' + errors.join('\n')); process.exit(1); }
  console.log('all checks passed');
})();
