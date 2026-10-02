/* AltSql DB live demo, v1.
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0 */
(function () {
  'use strict';
  var E = null, playing = null, T0 = 1767225600;
  var $ = function (id) { return document.getElementById(id); };
  var PRESETS = [
    ['Last ten minutes, every device', 'SELECT device, COUNT(*), AVG(temp), MAX(temp) FROM temps WHERE time >= 1767229200 GROUP BY device'],
    ['One device, a time range', 'SELECT * FROM temps WHERE device = 105 AND time BETWEEN 1767228000 AND 1767228300'],
    ['One reading by its key', ''],
    ['Three devices by key list', 'SELECT device, COUNT(*), MIN(temp), MAX(temp) FROM temps WHERE device IN (101, 105, 109) GROUP BY device'],
    ['Hot readings, any time', 'SELECT device, time, temp FROM temps WHERE temp > 28.5 ORDER BY temp DESC LIMIT 10'],
    ['Readings by machine', 'SELECT machine, COUNT(*), MIN(temp), MAX(temp) FROM temps GROUP BY machine'],
    ['Key-value pairs', 'SELECT key, value, COUNT(*) FROM kv GROUP BY key, value']
  ];

  function fmt(n) { return Number(n).toLocaleString('en-US'); }
  function esc(s) { return String(s).replace(/[&<>"]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]; }); }
  function clock(sec) { var m = Math.floor(sec / 60), h = Math.floor(m / 60); return h + ':' + ('0' + (m % 60)).slice(-2); }
  function note(text, kind) { var n = $('step-note'); n.textContent = text; n.className = 'note' + (kind ? ' ' + kind : ''); n.hidden = false; }
  function done(id) { $(id).classList.add('done'); }

  function render(s) {
    if (!s || !s.devices) return;
    $('clock').textContent = clock(s.clock);
    var h = '<thead><tr><th>Device</th><th>Site</th><th class="n">Readings</th><th class="n" title="The newest record the gateway confirmed: readings and key-value pairs count alike">Confirmed seq</th><th class="n">Waiting</th></tr></thead><tbody>';
    s.devices.forEach(function (d) {
      h += '<tr><td>' + d.id + '</td><td>' + d.site + '</td><td class="n">' + fmt(d.readings) + '</td><td class="n">' + fmt(d.confirmed) +
        '</td><td class="n">' + fmt(d.pending) + '</td></tr>';
    });
    $('fleet').innerHTML = h + '</tbody>';
    var g = s.gw, t = [
      [fmt(g.rows), 'readings in the file'],
      [fmt(g.txn), 'commits so far'],
      [fmt(g.batches), 'batches applied'],
      [(g.bytes / 1048576).toFixed(2) + ' MB', fmt(g.pages) + ' pages of 4 KB'],
      [g.perReading.toFixed(1), 'bytes of file per reading'],
      [fmt(g.cuts), 'power cuts survived']
    ];
    $('tiles').innerHTML = t.map(function (x) { return '<div class="tile"><b>' + x[0] + '</b><span>' + x[1] + '</span></div>'; }).join('');
  }

  function runSql(text) {
    var t = performance.now(), r = E.sql(text), ms = performance.now() - t;
    var meta = $('sql-meta'), plan = $('plan'), out = $('result');
    if (!r.ok) {
      meta.textContent = '';
      plan.hidden = true;
      out.innerHTML = '<tbody><tr class="empty"><td>' + esc(r.error || 'error') + '</td></tr></tbody>';
      return r;
    }
    meta.textContent = fmt(r.count) + (r.count === 1 ? ' row' : ' rows') + ', ' + fmt(r.read) + ' read from the table, ' + ms.toFixed(1) + ' ms' +
      (r.count > 50 ? ' (the first 50 shown)' : '');
    if (r.plan) {
      var p = r.plan.split('|');
      plan.innerHTML = '<span class="pill">' + esc(p[0]) + '</span>' + esc(p[1] || '');
      plan.hidden = false;
    } else plan.hidden = true;
    var h = '';
    if (r.cols) {
      h = '<thead><tr>' + r.cols.map(function (c, i) {
        var num = r.rows.length && typeof r.rows[0][i] === 'number';
        return '<th' + (num ? ' class="n"' : '') + '>' + esc(c) + '</th>';
      }).join('') + '</tr></thead><tbody>';
      r.rows.forEach(function (row) {
        h += '<tr>' + row.map(function (v) {
          var n = typeof v === 'number';
          return '<td' + (n ? ' class="n"' : '') + '>' + esc(n ? (Number.isInteger(v) ? v : +v.toFixed(4)) : v) + '</td>';
        }).join('') + '</tr>';
      });
      h += '</tbody>';
    } else h = '<tbody><tr class="empty"><td>No rows</td></tr></tbody>';
    out.innerHTML = h;
    return r;
  }

  function hex(s, cut) {
    var out = '';
    for (var i = 0; i < s.length; i += 2) out += (i && i / 2 === cut ? ' <i>|</i> ' : (i ? ' ' : '')) + s.substr(i, 2);
    return out;
  }

  function readBoth() {
    var i = +$('dev').value, reps = 1000, t, d, s, b;
    t = performance.now(); d = E.direct(i, reps); var dt = (performance.now() - t) / reps;
    var q = 'SELECT * FROM temps WHERE device = ' + (101 + i) + ' ORDER BY time DESC LIMIT 1';
    t = performance.now(); s = E.sql(q, reps); var st = (performance.now() - t) / reps;
    $('side').hidden = false;
    $('d-time').textContent = (dt * 1000).toFixed(1) + ' µs a read';
    $('d-what').textContent = d.ok ? 'seq ' + d.seq + ', time ' + clock(d.time - T0) + ', ' + d.temp + ' °C · one walk down the tree' : 'no reading';
    $('s-time').textContent = (st * 1000).toFixed(1) + ' µs a query';
    $('s-what').textContent = s.ok ? 'the same reading · plan: ' + (s.plan || '').replace('|', ', ') + ', ' + fmt(s.read) + ' rows read, then sorted' : s.error;
    b = E.bytes(i);
    if (b.ok) {
      $('bytes').hidden = false;
      $('b-dev').innerHTML = hex(b.header + b.onDevice, 12);
      $('b-gw').innerHTML = hex(b.onGateway, -1);
      $('b-note').textContent = b.same ? 'The payload is the same, byte for byte: ' + b.onGateway.length / 2 +
        ' bytes, the series number, then time, machine and temp, little-endian, as the device wrote them. The header is not stored: its sequence number is part of the key, and the rest can be rebuilt.'
        : 'The bytes differ.';
    }
    return { d: d, s: s, b: b, dt: dt, st: st };
  }

  function cut(mode) {
    var r = E.cut(mode);
    if (!r.ok) { note('The cut did not happen: ' + (r.error || ''), 'bad'); return r; }
    var li = document.createElement('li');
    var both = r.slot0 === 'complete' && r.slot1 === 'complete';
    li.innerHTML = '<b>Cut ' + (mode ? 'while writing the commit header' : 'while writing pages') + '</b> of transaction ' + fmt(r.inflight) +
      '. The file opened at transaction ' + fmt(r.after) + ': <b>' + r.kept + '</b>. ' +
      (both ? 'Both headers describe complete trees' : 'Header slots: ' + r.slot0 + ' and ' + r.slot1) + '. ' +
      (r.twice ? fmt(r.twice) + ' records arrived again and were skipped. ' : '') +
      (r.resent ? fmt(r.resent) + ' records were sent again. ' : '') +
      (r.devicesShort ? r.devicesShort + ' devices are short of readings.' : 'Every device\'s readings are all there, once.');
    if (r.devicesShort || !both) li.style.borderLeftColor = 'var(--red)';
    $('cutlog').insertBefore(li, $('cutlog').firstChild);
    render(E.status());
    return r;
  }

  function play() {
    if (playing) { clearInterval(playing); playing = null; $('btn-play').textContent = 'Play'; return; }
    $('btn-play').textContent = 'Pause';
    playing = setInterval(function () { render(E.tick(1)); }, 250);
  }

  function enable() {
    var k = E.sql('SELECT seq FROM temps WHERE device = 107 AND time = 1767227000');   /* the key of one reading */
    PRESETS[2][1] = 'SELECT * FROM temps WHERE device = 107 AND time = 1767227000 AND seq = ' + (k.ok && k.rows && k.rows[0] ? k.rows[0][0] : 0);
    ['go-run', 'go-sql', 'go-direct', 'go-cut', 'btn-play', 'btn-sql', 'btn-direct', 'btn-cut-pages', 'btn-cut-header', 'sql', 'dev']
      .forEach(function (id) { $(id).disabled = false; });
    PRESETS.forEach(function (p) {
      var b = document.createElement('button');
      b.type = 'button';
      b.textContent = p[0];
      b.className = 'seg-b';
      b.addEventListener('click', function () { $('sql').value = p[1]; runSql(p[1]); });
      var wrap = document.createElement('span');
      wrap.className = 'seg';
      wrap.appendChild(b);
      $('presets').appendChild(wrap);
    });
    for (var i = 0; i < 12; i++) {
      var o = document.createElement('option');
      o.value = i; o.textContent = 101 + i;
      if (i === 4) o.selected = true;
      $('dev').appendChild(o);
    }
  }

  window.AltSqlDbDemo.load().then(function (eng) {
    E = eng;
    var t = performance.now(), r = E.init();
    if (!r.ok) { $('engine-state').textContent = 'The modules did not start: ' + (r.error || ''); return; }
    $('ver').textContent = E.version();
    $('engine-state').textContent = 'Ready: twelve devices, an hour of readings, ' + (performance.now() - t).toFixed(0) + ' ms to start';
    render(E.status());
    enable();
    $('btn-play').addEventListener('click', play);
    $('btn-sql').addEventListener('click', function () { runSql($('sql').value); });
    $('sql').addEventListener('keydown', function (e) { if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') runSql($('sql').value); });
    $('btn-direct').addEventListener('click', readBoth);
    $('btn-cut-pages').addEventListener('click', function () { cut(0); });
    $('btn-cut-header').addEventListener('click', function () { cut(1); });
    document.body.setAttribute('data-state', 'ready');
    $('go-run').addEventListener('click', function () {
      var s = E.tick(60);
      render(s);
      done('go-run');
      document.body.setAttribute('data-run', String(s.gw.rows));
      note('Ten more minutes: ' + fmt(s.gw.rows) + ' readings from twelve devices in one file, ' + fmt(s.gw.txn) + ' commits so far.', 'good');
    });
    $('go-sql').addEventListener('click', function () {
      var q = PRESETS[0][1];
      $('sql').value = q;
      var r = runSql(q);
      done('go-sql');
      document.body.setAttribute('data-sql', (r.plan || '').split('|')[0] + ':' + r.count + ':' + r.read);
      note('The gateway read ' + fmt(r.read) + ' readings for this answer, one time range for each device, out of every reading in the file.', 'good');
      $('h-sql').scrollIntoView({ behavior: 'smooth', block: 'start' });
    });
    $('go-direct').addEventListener('click', function () {
      var r = readBoth();
      done('go-direct');
      document.body.setAttribute('data-direct', (r.b && r.b.same ? 'same' : 'differ') + ':' + (r.d.ok ? 'ok' : 'none'));
      note('The direct path read the newest reading in ' + (r.dt * 1000).toFixed(1) + ' µs, SQL in ' + (r.st * 1000).toFixed(1) +
        ' µs. The stored bytes are the device\'s own.', 'good');
      $('h-direct').scrollIntoView({ behavior: 'smooth', block: 'start' });
    });
    $('go-cut').addEventListener('click', function () {
      var r = cut(1);
      done('go-cut');
      document.body.setAttribute('data-cut', r.ok ? r.kept + ':' + r.devicesShort + ':' + r.slot0 + ':' + r.slot1 : 'failed');
      if (r.ok) note('The power went while the gateway wrote a commit header. It opened at ' + r.kept + ', and no device lost a reading.', r.devicesShort ? 'bad' : 'good');
      $('h-cut').scrollIntoView({ behavior: 'smooth', block: 'start' });
    });
  }).catch(function (e) {
    $('engine-state').textContent = 'The modules did not load: ' + e;
  });
})();
