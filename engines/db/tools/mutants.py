#!/usr/bin/env python3
# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
"""Mutation check for AltSql DB: plants one deliberate bug at a time in
altsql_db.h and runs the test suite (quick settings) against it, fastest
tests first, stopping at the first that fails. A good test suite catches
every one. A mutant counts as caught when a test fails, crashes, or runs for
more than three minutes.

Usage: python3 tools/mutants.py      (from engines/db; SQLite in build/sqlite3.o
       is used when there, for the comparison against SQLite)
"""
import os, shutil, subprocess, sys, tempfile, time

MUTANTS = [
    # ---- storage: commits, headers, free list, tree
    ("storage", "a commit writes its header before its pages are synced",
     "    db->nsync++;\n    if (db->f.sync(db->f.ctx)) { rc = ALTSQL_IOERR; goto fail; }\n    nh.txn = db->cur;",
     "    nh.txn = db->cur;"),
    ("storage", "every commit header goes to slot 0",
     "if (db->f.write(db->f.ctx, (uint64_t)(db->cur & 1) * db->ps, h, ASD_HS))",
     "if (db->f.write(db->f.ctx, 0, h, ASD_HS))"),
    ("storage", "opening prefers the older of two good headers",
     "i = ok[0] && (!ok[1] || x[0].txn > x[1].txn) ? 0 : 1;",
     "i = ok[0] && (!ok[1] || x[0].txn < x[1].txn) ? 0 : 1;"),
    ("storage", "a header's checksum is not checked",
     "as_get32(h + 8) != 2 || as_get32(h + 60) != as_crc32(0, h, 60)) return 0;",
     "as_get32(h + 8) != 2) return 0;"),
    ("storage", "a page freed by the last commit is reused at once",
     "        db->hopg[db->nho] = pg;\n        db->hotag[db->nho++] = db->cur;\n    }\n    return ALTSQL_OK;",
     "        db->avpg[db->nav] = pg;\n        db->avtag[db->nav++] = 0;\n    }\n    return ALTSQL_OK;"),
    ("storage", "the newest free list is not copied at commit: its pages are lost",
     "    while ((old = db->nxnext) != 0) {", "    db->nxleft = 0;\n    while ((old = db->nxnext) != 0 && 0) {"),
    ("storage", "the header records no entries in the newest list",
     "    nh.nxcount = db->hcount;", "    nh.nxcount = 0;"),
    ("storage", "loading the ready list forgets to count the entries taken",
     "    db->flnext = asd_x(p);\n    db->fltail -= n;", "    db->flnext = asd_x(p);"),
    ("storage", "a spilled page of reusable entries is linked into the newest list",
     "    if (hold) {\n        asd_set_x(p, db->hhead);", "    if (1) {\n        asd_set_x(p, db->hhead);"),
    ("storage", "leaf search returns the slot after an equal key",
     "        if (r < 0) lo = mid + 1;\n        else { hi = mid; if (r == 0) *found = 1; }",
     "        if (r <= 0) lo = mid + 1;\n        if (r > 0) hi = mid; if (r == 0) *found = 1;"),
    ("storage", "branch search sends a key equal to a separator left",
     "        if (asd_cmp(k, kn, ck, cn) < 0) hi = mid; else lo = mid + 1;",
     "        if (asd_cmp(k, kn, ck, cn) <= 0) hi = mid; else lo = mid + 1;"),
    ("storage", "a split's separator is one byte too short",
     "    *seplen = i < bn ? i + 1 : bn;", "    *seplen = i < bn ? i : bn;"),
    ("storage", "a large value is read one page short",
     "        n = vn - done < db->ovd ? vn - done : db->ovd;\n        memcpy(out + done, db->sc + ASD_PH, n);\n        done += n;",
     "        n = vn - done < db->ovd ? vn - done : db->ovd;\n        memcpy(out + done, db->sc + ASD_PH, n);\n        done += n;\n        if (done + db->ovd >= vn) break;"),
    ("storage", "a rollback keeps the transaction's root and page count",
     "        db->nmaps = 0;\n    }\n    asd_end(db);\n    return ALTSQL_OK;", "        db->nmaps = 0;\n    }\n    db->tx = 0;\n    db->failed = 0;\n    return ALTSQL_OK;"),
    ("storage", "a cursor does not find its place again after a write",
     "    if (c->gen != db->gen) {                /* the tree changed: find the place again */",
     "    if (0) {                /* the tree changed: find the place again */"),
    # ---- tables and keys
    ("tables", "negative whole numbers sort after positive ones in keys",
     "    p[0] = (uint8_t)(v < 0 ? 0x87 - n : 0x88 + n);", "    p[0] = (uint8_t)(v < 0 ? 0x88 + n : 0x87 - n);"),
    ("tables", "negative reals keep their bits in keys",
     "        b = (b >> 63) ? ~b : (b | ((uint64_t)1 << 63));", "        b = b | ((uint64_t)1 << 63);"),
    ("tables", "a zero byte in a text key is not escaped",
     "        if (!s[i]) p[o++] = 0xFF;", "        (void)0;"),
    ("tables", "a row's text is stored one byte short",
     "            if (v[i].len) memcpy(dst + off, v[i].u.s, (size_t)v[i].len);\n            off += (uint32_t)v[i].len;",
     "            if (v[i].len) memcpy(dst + off, v[i].u.s, (size_t)v[i].len - 1);\n            off += (uint32_t)v[i].len;"),
    # ---- sync
    ("sync", "records the gateway already has are applied again",
     "        if (seq > last) {", "        if (1) {"),
    ("sync", "a record's checksum is not checked",
     "if (len - off - AS_RH < plen || as_crc32(as_crc32(0, h + 1, 7), h + AS_RH, plen) != as_get32(h + 8) ||",
     "if (len - off - AS_RH < plen ||"),
    ("sync", "a device with another layout is let in",
     "        if (!same) {\n            snprintf(msg, sizeof msg, \"series %s: this device's layout differs from the table's\", S.name);",
     "        if (0) {\n            snprintf(msg, sizeof msg, \"series %s: this device's layout differs from the table's\", S.name);"),
    ("sync", "the device's position is not saved with its records",
     "        if ((rc = asd_write(db, 1, k, kn, v, 4)) != 0) goto fail;\n    }\n    if (own && (rc = altsql_db_commit(db)) != 0) return rc;",
     "    }\n    if (own && (rc = altsql_db_commit(db)) != 0) return rc;"),
    # ---- SQL
    ("sql", "a point lookup is planned with one key column missing",
     "    if (s->neq == T->nkey && !rowid) s->plan = ASD_P_POINT;", "    if (s->neq + 1 >= T->nkey && !rowid) s->plan = ASD_P_POINT;"),
    ("sql", "a range scan stops before its upper bound",
     "            int r = memcmp(c.key, end, n);\n            if (r > 0) break;", "            int r = memcmp(c.key, end, n);\n            if (r >= 0) break;"),
    ("sql", "UPDATE and DELETE act on rows WHERE leaves out",
     "        if (!as_truth(&w)) return ALTSQL_OK;\n    }\n    return s->act(s, row);", "        (void)w;\n    }\n    return s->act(s, row);"),
    ("sql", "a key list reads a repeated value twice",
     "            if (i && as_cmp(&v[i - 1], &v[i]) == 0) continue;", "            (void)0;"),
    ("sql", "a key list takes its values in the order written",
     "            for (j = i; j > 0 && as_cmp(&v[j - 1], &x) > 0; j--) v[j] = v[j - 1];", "            for (j = i; 0; j--) v[j] = v[j - 1];"),
    ("sql", "an UPDATE writes the old values back",
     "    if (!u->keyset) return asd_write(s->db, 1, s->ck, s->ckn, u->pack, vn);",
     "    if (!u->keyset) { vn = asd_rpack(s->T.types, s->T.ncols, row, u->pack); return asd_write(s->db, 1, s->ck, s->ckn, u->pack, vn); }"),
    ("sql", "an UPDATE of keys leaves the old rows",
     "        if ((rc = asd_write(db, 0, u->store + at + 6, kn, 0, 0)) != 0) return rc;", "        (void)0;"),
    ("sql", "NOT IN becomes OR of <>",
     "                asd_tx(o, neg ? \" AND \" : \" OR \", neg ? 5 : 4);", "                asd_tx(o, \" OR \", 4);"),
    ("sql", "a quote in a bound text value is not doubled",
     "            if (v->u.s[i] == '\\'') asd_tx(o, \"'\", 1);", "            (void)0;"),
    ("sql", "a prepared SELECT that goes on gives its last row again",
     "    if (rc == ALTSQL_OK && past && found) rc = altsql_db_next(&c);", "    (void)past;"),
    ("sql", "a prepared SELECT stops at the end of its first batch",
     "        if (!st->nrows) st->err = ALTSQL_TOOBIG; else st->more = 1;", "        if (!st->nrows) st->err = ALTSQL_TOOBIG;"),
    ("sql", "a sorted prepared SELECT skips one row too few",
     "    st->skip = st->stream ? 0 : st->delivered;", "    st->skip = st->stream ? 0 : st->delivered ? st->delivered - 1 : 0;"),
    ("sql", "INSERT does not look for a key already there",
     "            if (rc == ALTSQL_OK) { asd_unpin(db, fi); rc = asd_err(db, ALTSQL_EXISTS, \"a row with that key is already there\"); break; }",
     "            if (rc == ALTSQL_OK) { asd_unpin(db, fi); rc = ALTSQL_NOTFOUND; }"),
    ("sql", "DROP TABLE leaves the table's rows",
     "    s->plan = ASD_P_FULL;\n    s->act = asd_del_act;\n    s->actx = &u;\n    rc = asd_sq_scan(s);",
     "    s->plan = ASD_P_FULL;\n    s->act = asd_del_act;\n    s->actx = &u;\n    rc = ALTSQL_OK;"),
]

TESTS = ["test_rows", "test_db", "test_sql", "test_vs_sqlite", "test_sync", "test_crash", "test_fault"]

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    header = open("altsql_db.h").read()
    sqlite = os.path.exists("build/sqlite3.o")
    tmp = tempfile.mkdtemp(prefix="db-mutants-")
    caught = applied = 0
    t_all = time.time()
    print("%-3s %-8s %-64s %s" % ("#", "part", "planted bug", "result"))
    try:
        for i, (part, what, old, new) in enumerate(MUTANTS, 1):
            n = header.count(old)
            if n != 1:
                print("%-3d %-8s %-64s NOT APPLIED (%d matches)" % (i, part, what, n), flush=True)
                continue
            applied += 1
            with open(os.path.join(tmp, "altsql_db.h"), "w") as f:
                f.write(header.replace(old, new))
            verdict = "MISSED"
            for t in TESTS:
                if t == "test_vs_sqlite" and not sqlite:
                    continue
                exe = os.path.join(tmp, t)
                cmd = ["cc", "-std=c99", "-O1", "-D_POSIX_C_SOURCE=200809L", "-I" + tmp, "-I../../core/dist", "-Itests",
                       "-I../../../third/sqlite", "tests/%s.c" % t, "-o", exe]
                cmd += (["build/sqlite3.o", "-lpthread"] if t == "test_vs_sqlite" else []) + ["-lm"]
                b = subprocess.run(cmd, capture_output=True, text=True)
                if b.returncode != 0:
                    verdict = "caught at compile time"
                    break
                try:
                    r = subprocess.run([exe, "quick"], capture_output=True, text=True, timeout=180)
                    if r.returncode:
                        verdict = "caught by %s (%s)" % (t, "test failed" if r.returncode == 1 else "crash %d" % r.returncode)
                        break
                except subprocess.TimeoutExpired:
                    verdict = "caught by %s (ran too long)" % t
                    break
            if verdict != "MISSED":
                caught += 1
            print("%-3d %-8s %-64s %s" % (i, part, what, verdict), flush=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d of %d planted bugs caught (%.0f s)" % (caught, applied, time.time() - t_all))
    return 0 if caught == applied == len(MUTANTS) else 1

if __name__ == "__main__":
    sys.exit(main())
