/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* Runs the demo engine under Node and prints what the page shows: a check
 * that the WebAssembly build behaves as the native one.
 *   node tools/facts.js [app/engine-db.v1.js] */
'use strict';
require(require('path').resolve(process.argv[2] || 'app/engine-db.v1.js'));
globalThis.AltSqlDbDemo.load().then(function (e) {
  var t0 = Date.now(), r, i, n = 0, kept = { 'the last commit': 0, 'the commit under way': 0 }, bad = 0;
  console.log('engine', e.version(), JSON.stringify(e.init()), (Date.now() - t0) + ' ms to start');
  r = e.tick(60);
  console.log('after ten more minutes:', r.gw.rows, 'rows from', r.devices.length, 'devices,', r.gw.pages, 'pages,', r.gw.perReading, 'bytes per reading');
  [ 'SELECT device, COUNT(*), AVG(temp) FROM temps WHERE time >= 1767228600 GROUP BY device',
    'SELECT * FROM temps WHERE device = 105 AND time > 1767229800',
    'SELECT * FROM temps WHERE device = 105 ORDER BY time DESC LIMIT 1',
    'SELECT key, value, COUNT(*) FROM kv GROUP BY key, value',
    'SELECT * FROM nosuch' ].forEach(function (q) {
    var a = e.sql(q);
    console.log(q, '->', a.ok ? a.count + ' rows, ' + a.read + ' read, plan ' + a.plan : 'error ' + a.error);
  });
  console.log('direct:', JSON.stringify(e.direct(4)));
  r = e.bytes(4);
  console.log('bytes: same', r.same, 'device', r.onDevice, 'gateway', r.onGateway);
  for (i = 0; i < 30; i++) {
    r = e.cut(i % 2);
    if (!r.ok || r.slot0 === 'damaged' || r.slot1 === 'damaged' || r.devicesShort) bad++;
    kept[r.kept]++;
    n++;
  }
  console.log('power cuts:', n, 'kept the last commit', kept['the last commit'], 'kept the commit under way', kept['the commit under way'], 'problems', bad);
  if (bad) process.exit(1);
});
