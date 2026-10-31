"""Makes Pagila's upstream files load on PostgreSQL 16 without pgvector.

Upstream Pagila now needs PostgreSQL 18 (uuidv7(), VIRTUAL columns), pgvector (the
film_embedding table) and a PostgreSQL 17 setting (transaction_timeout).
This keeps everything else byte for byte and:
  - uses gen_random_uuid() for the uuid column defaults,
  - makes the one VIRTUAL generated column STORED (VIRTUAL is PostgreSQL 18),
  - drops the vector extension and the film_embedding table with its data,
    index, trigger and constraints,
  - drops the line SET transaction_timeout = 0;

  python3 -I adapt_pagila.py IN_SCHEMA IN_DATA OUT_SCHEMA OUT_DATA
"""
import sys

def statements(text):
    cur = []
    for line in text.splitlines(keepends=True):
        cur.append(line)
        if line.rstrip().endswith(';') and not line.startswith('--'):
            yield ''.join(cur); cur = []
    if cur:
        yield ''.join(cur)

def schema(text):
    out = []
    for st in statements(text):
        body = '\n'.join(l for l in st.splitlines() if not l.startswith('--'))
        if 'film_embedding' in body or 'EXTENSION' in body and 'vector' in body:
            # keep the comment lines, drop the statement
            out.append(''.join(l for l in st.splitlines(keepends=True) if l.startswith('--') or not l.strip()))
            continue
        st = st.replace('uuidv7()', 'gen_random_uuid()')
        st = st.replace(') VIRTUAL', ') STORED')   # virtual generated columns: PostgreSQL 18
        out.append(st)
    return ''.join(out)

def data(text):
    out, skip = [], False
    for line in text.splitlines(keepends=True):
        if line.startswith('SET transaction_timeout'):
            continue
        if line.startswith('COPY public.film_embedding '):
            skip = True; continue
        if skip:
            if line.rstrip('\n') == '\\.':
                skip = False
            continue
        out.append(line)
    return ''.join(out)

i_s, i_d, o_s, o_d = sys.argv[1:5]
with open(i_s, encoding='utf-8') as f: s = f.read()
with open(i_d, encoding='utf-8') as f: d = f.read()
with open(o_s, 'w', encoding='utf-8') as f: f.write(schema(s))
with open(o_d, 'w', encoding='utf-8') as f: f.write(data(d))
