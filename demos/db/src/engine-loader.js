/* AltSql DB demo engine: AltSql DB and twelve devices running AltSql Core,
 * compiled to WebAssembly. Loaded from the text below, so the page also works
 * when opened straight from a folder on disk. Nothing here talks to a server.
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0 */
(function (global) {
  'use strict';
  var WASM = '__WASM_BASE64__';

  function bytes(b64) {
    var bin = atob(b64), n = bin.length, out = new Uint8Array(n);
    for (var i = 0; i < n; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  function load() {
    return WebAssembly.instantiate(bytes(WASM), { wasi_snapshot_preview1: stubs() }).then(function (res) {
      var x = res.instance.exports;
      var td = new TextDecoder('utf-8'), te = new TextEncoder();
      if (x._initialize) x._initialize();
      function str(p) {
        var m = new Uint8Array(x.memory.buffer), e = p;
        while (m[e]) e++;
        return td.decode(m.subarray(p, e));
      }
      function json(p) { return JSON.parse(str(p)); }
      function put(s) {
        var b = te.encode(s).subarray(0, 8000), p = x.demo_in();
        new Uint8Array(x.memory.buffer).set(b, p);
        return b.length;
      }
      return {
        version: function () { return str(x.demo_version()); },
        init: function () { return json(x.demo_init()); },
        status: function () { return json(x.demo_status()); },
        tick: function (n) { return json(x.demo_tick(n | 0)); },
        sql: function (s, reps) { var n = put(s); return json(x.demo_sql(n, (reps | 0) || 1)); },
        direct: function (i, reps) { return json(x.demo_direct(i | 0, (reps | 0) || 1)); },
        bytes: function (i) { return json(x.demo_bytes(i | 0)); },
        cut: function (mode) { return json(x.demo_cut(mode | 0)); }
      };
    });
  }

  /* The engines ask the system for nothing; these answer if they ever do. */
  function stubs() {
    var f = function () { return 52; };   /* ENOSYS */
    return new Proxy({}, { get: function () { return f; } });
  }

  global.AltSqlDbDemo = { load: load };
})(typeof window !== 'undefined' ? window : globalThis);
