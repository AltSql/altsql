/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * SQL layer (gateway builds)
 *
 * A small recursive-descent parser builds an expression tree in the
 * working memory; the executor streams rows from the log through WHERE,
 * then grouping, HAVING, ordering and LIMIT. Each time-series is a table
 * whose columns come from its schema; "kv" is the key-value store.
 * ====================================================================== */
#if ALTSQL_ENABLE_SQL

#define AS_TMP_SIZE   4096u      /* per-row scratch for LOWER/UPPER */
#define AS_MAXITEMS   64
#define AS_MAXGROUP   16
#define AS_MAXDEPTH   200        /* deepest expression: parsing and evaluating
                                    recurse, so this bounds the stack they use */
#define AS_MAXNODES   10000      /* largest expression once aliases are expanded:
                                    bounds the work done for each row */

typedef struct as_arena { uint8_t *base; size_t cap, used; } as_arena;

static void *as_alloc(as_arena *a, size_t n) {
    void *p;
    n = (n + 7u) & ~(size_t)7u;
    if (n > a->cap - a->used) return NULL;
    p = a->base + a->used;
    a->used += n;
    return p;
}

/* ---- Values ----------------------------------------------------------------- */
static int as_isnum(const altsql_value *v) { return v->type == ALTSQL_INTEGER || v->type == ALTSQL_REAL; }
static double as_num(const altsql_value *v) {
    return v->type == ALTSQL_INTEGER ? (double)v->u.i : v->type == ALTSQL_REAL ? v->u.r : 0.0;
}

/* Order: NULL < numbers < text. Integers and reals compare by value. */
static int as_cmp(const altsql_value *a, const altsql_value *b) {
    if (a->type == ALTSQL_NULL || b->type == ALTSQL_NULL)
        return (a->type != ALTSQL_NULL) - (b->type != ALTSQL_NULL);
    if (as_isnum(a) && as_isnum(b)) {
        if (a->type == ALTSQL_INTEGER && b->type == ALTSQL_INTEGER)
            return (a->u.i > b->u.i) - (a->u.i < b->u.i);
        { double x = as_num(a), y = as_num(b); return (x > y) - (x < y); }
    }
    if (as_isnum(a)) return -1;
    if (as_isnum(b)) return 1;
    {
        int n = a->len < b->len ? a->len : b->len, c = memcmp(a->u.s, b->u.s, (size_t)n);
        return c ? (c > 0) - (c < 0) : (a->len > b->len) - (a->len < b->len);
    }
}

static int as_truth(const altsql_value *v) {
    return (v->type == ALTSQL_INTEGER && v->u.i != 0) || (v->type == ALTSQL_REAL && v->u.r != 0.0);
}

static uint32_t as_vhash(const altsql_value *v) {
    uint8_t b[8];
    if (v->type == ALTSQL_TEXT) return as_hash((const uint8_t *)v->u.s, (uint32_t)v->len) ^ 0x5bd1e995u;
    if (v->type == ALTSQL_NULL) return 0x9e3779b9u;
    if (v->type == ALTSQL_REAL && v->u.r >= -9.2e18 && v->u.r <= 9.2e18 && v->u.r == (double)(int64_t)v->u.r) {
        as_put64(b, (uint64_t)(int64_t)v->u.r);          /* 2.0 groups with 2 */
    } else if (v->type == ALTSQL_REAL) {
        uint64_t x;
        memcpy(&x, &v->u.r, 8);
        as_put64(b, x);
    } else {
        as_put64(b, (uint64_t)v->u.i);
    }
    return as_hash(b, 8);
}

static int as_vcopy(as_arena *A, altsql_value *dst, const altsql_value *src) {
    *dst = *src;
    if (src->type == ALTSQL_TEXT && src->len > 0) {
        char *p = (char *)as_alloc(A, (size_t)src->len);
        if (!p) return ALTSQL_NOMEM;
        memcpy(p, src->u.s, (size_t)src->len);
        dst->u.s = p;
    }
    return ALTSQL_OK;
}

static void as_setint(altsql_value *v, int64_t i) { v->type = ALTSQL_INTEGER; v->len = 0; v->u.i = i; }
static void as_setreal(altsql_value *v, double r) { v->type = ALTSQL_REAL; v->len = 0; v->u.r = r; }
static void as_setnull(altsql_value *v) { v->type = ALTSQL_NULL; v->len = 0; v->u.i = 0; }

/* ---- Lexer --------------------------------------------------------------------- */
enum { K_END, K_ID, K_INT, K_REAL, K_STR, K_OP };
enum { OP_LE = 'l', OP_GE = 'g', OP_NE = 'n' };

typedef struct as_tok { int k, op; const char *s; size_t n; int64_t i; double r; } as_tok;

typedef struct as_parser {
    altsql *db;
    const char *p, *pend;          /* pend: end of the token consumed last */
    as_tok t;
    as_arena *A;
    int rc;
    int nest;                      /* recursion depth of the expression parser */
} as_parser;

static int as_perr(as_parser *P, const char *msg) {
    if (!P->rc) {
        size_t n = P->t.k == K_END ? 3 : (P->t.n > 24 ? 24 : P->t.n);
        as_err2(P->db, ALTSQL_SYNTAX, msg, P->t.k == K_END ? "end" : P->t.s, n);
        P->rc = ALTSQL_SYNTAX;
    }
    return P->rc;
}

static void as_lex(as_parser *P) {
    const char *s = P->p;
    as_tok *t = &P->t;
    P->pend = s;
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        if (s[0] == '-' && s[1] == '-') { while (*s && *s != '\n') s++; continue; }
        break;
    }
    t->s = s;
    t->op = 0;
    if (!*s) { t->k = K_END; t->n = 0; P->p = s; return; }
    if (as_is_ident_start((unsigned char)*s)) {
        while (as_is_ident_char((unsigned char)*s)) s++;
        t->k = K_ID;
    } else if ((*s >= '0' && *s <= '9') || (*s == '.' && s[1] >= '0' && s[1] <= '9')) {
        char *e;
        int real = 0;
        const char *q = s;
        while (*q >= '0' && *q <= '9') q++;
        if (*q == '.' || *q == 'e' || *q == 'E') real = 1;
        if (!real) {
            errno = 0;
            t->i = strtoll(s, &e, 10);
            if (e == q && errno != ERANGE) { t->k = K_INT; s = q; }
            else real = 1;
        }
        if (real) { t->r = strtod(s, &e); t->k = K_REAL; s = e; }
    } else if (*s == '\'') {
        s++;
        while (*s && !(*s == '\'' && s[1] != '\'')) s += (*s == '\'') ? 2 : 1;
        if (!*s) { t->k = K_END; t->n = 0; P->p = s; as_perr(P, "unterminated string near "); return; }
        s++;
        t->k = K_STR;
    } else {
        t->k = K_OP;
        t->op = *s++;
        if (t->op == '<' && *s == '=') { t->op = OP_LE; s++; }
        else if (t->op == '>' && *s == '=') { t->op = OP_GE; s++; }
        else if (t->op == '<' && *s == '>') { t->op = OP_NE; s++; }
        else if (t->op == '!' && *s == '=') { t->op = OP_NE; s++; }
        else if (t->op == '=' && *s == '=') { s++; }
        else if (!strchr("(),*+-/%=<>;", t->op)) { t->n = 1; P->p = s; as_perr(P, "unexpected character "); return; }
    }
    t->n = (size_t)(s - t->s);
    P->p = s;
}

static int as_kw(const as_parser *P, const char *kw) {
    return P->t.k == K_ID && as_ieq(P->t.s, P->t.n, kw, strlen(kw));
}
static int as_isop(const as_parser *P, int op) { return P->t.k == K_OP && P->t.op == op; }
static int as_accept_kw(as_parser *P, const char *kw) { if (as_kw(P, kw)) { as_lex(P); return 1; } return 0; }
static int as_accept_op(as_parser *P, int op) { if (as_isop(P, op)) { as_lex(P); return 1; } return 0; }
static int as_expect_kw(as_parser *P, const char *kw) { if (as_accept_kw(P, kw)) return 1; as_perr(P, "syntax error near "); return 0; }
static int as_expect_op(as_parser *P, int op) { if (as_accept_op(P, op)) return 1; as_perr(P, "syntax error near "); return 0; }

static int as_reserved(const as_parser *P) {
    static const char *const kws[] = { "SELECT", "FROM", "WHERE", "GROUP", "BY", "HAVING", "ORDER",
        "LIMIT", "OFFSET", "ASC", "DESC", "AND", "OR", "NOT", "LIKE", "IS", "NULL", "BETWEEN",
        "AS", "VALUES", "INTO", 0 };
    int i;
    for (i = 0; kws[i]; i++) if (as_kw(P, kws[i])) return 1;
    return 0;
}

/* ---- Expressions ---------------------------------------------------------------- */
enum { E_LIT = 1, E_COL, E_NAME, E_NEG, E_NOT, E_BIN, E_AND, E_OR, E_ISNULL, E_BETWEEN, E_LIKE, E_FN, E_AGG, E_STAR };
enum { F_COUNT = 1, F_SUM, F_AVG, F_MIN, F_MAX, F_ABS, F_ROUND, F_LENGTH, F_LOWER, F_UPPER };

typedef struct as_expr {
    uint8_t k, op, notf, fn;
    int16_t idx;
    struct as_expr *a, *b, *c;
    altsql_value v;
    const char *s0;
    size_t sn;
    uint16_t depth;                /* measured once names are resolved; 0 = not yet */
    uint32_t size;                 /* nodes, counting shared alias trees each time */
} as_expr;

static as_expr *as_node(as_parser *P, int k, const char *s0) {
    as_expr *e = (as_expr *)as_alloc(P->A, sizeof *e);
    if (!e) { if (!P->rc) P->rc = as_err(P->db, ALTSQL_NOMEM, "out of working memory"); return NULL; }
    memset(e, 0, sizeof *e);
    e->k = (uint8_t)k;
    e->s0 = s0;
    return e;
}
static void as_span(as_parser *P, as_expr *e) { if (e) e->sn = (size_t)(P->pend - e->s0); }

static as_expr *as_parse_expr(as_parser *P);

static int as_fn_id(const char *s, size_t n) {
    static const char *const f[] = { "", "COUNT", "SUM", "AVG", "MIN", "MAX", "ABS", "ROUND", "LENGTH", "LOWER", "UPPER" };
    int i;
    for (i = 1; i <= F_UPPER; i++) if (as_ieq(s, n, f[i], strlen(f[i]))) return i;
    return 0;
}

static as_expr *as_parse_primary(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e;
    if (P->rc) return NULL;
    if (P->t.k == K_INT || P->t.k == K_REAL) {
        e = as_node(P, E_LIT, s0);
        if (!e) return NULL;
        if (P->t.k == K_INT) as_setint(&e->v, P->t.i); else as_setreal(&e->v, P->t.r);
        as_lex(P);
        as_span(P, e);
        return e;
    }
    if (P->t.k == K_STR) {
        size_t i, n = 0;
        char *d;
        e = as_node(P, E_LIT, s0);
        d = (char *)as_alloc(P->A, P->t.n);
        if (!e || !d) { if (!P->rc) P->rc = as_err(P->db, ALTSQL_NOMEM, "out of working memory"); return NULL; }
        for (i = 1; i + 1 < P->t.n; i++) {
            d[n++] = P->t.s[i];
            if (P->t.s[i] == '\'') i++;
        }
        e->v.type = ALTSQL_TEXT;
        e->v.u.s = d;
        e->v.len = (int)n;
        as_lex(P);
        as_span(P, e);
        return e;
    }
    if (as_accept_kw(P, "NULL")) {
        e = as_node(P, E_LIT, s0);
        if (e) { as_setnull(&e->v); as_span(P, e); }
        return e;
    }
    if (as_accept_op(P, '(')) {
        e = as_parse_expr(P);
        if (!as_expect_op(P, ')')) return NULL;
        return e;
    }
    if (P->t.k == K_ID && !as_reserved(P)) {
        const char *name = P->t.s;
        size_t n = P->t.n;
        as_lex(P);
        if (as_accept_op(P, '(')) {
            int fn = as_fn_id(name, n);
            if (!fn) { P->t.s = name; P->t.n = n; as_perr(P, "unknown function "); return NULL; }
            e = as_node(P, E_FN, s0);
            if (!e) return NULL;
            e->fn = (uint8_t)fn;
            if (fn == F_COUNT && as_isop(P, '*')) {
                e->a = as_node(P, E_STAR, P->t.s);
                as_lex(P);
            } else {
                e->a = as_parse_expr(P);
                if (as_accept_op(P, ',')) e->b = as_parse_expr(P);
            }
            if (!as_expect_op(P, ')')) return NULL;
            if (!e->a || (fn != F_ROUND && e->b)) { as_perr(P, "wrong number of arguments near "); return NULL; }
            as_span(P, e);
            return e;
        }
        e = as_node(P, E_NAME, s0);
        if (e) e->sn = n;
        return e;
    }
    as_perr(P, "syntax error near ");
    return NULL;
}

/* Every recursive step of the parser goes through here, so hostile input
 * such as 100,000 nested brackets gives an error instead of a stack overflow. */
static int as_nest_in(as_parser *P) {
    if (++P->nest <= AS_MAXDEPTH) return 1;
    if (!P->rc) P->rc = as_err(P->db, ALTSQL_SYNTAX, "expression nested too deeply");
    return 0;
}

static as_expr *as_parse_unary(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e = NULL;
    if (as_accept_op(P, '-')) {
        e = as_node(P, E_NEG, s0);
        if (as_nest_in(P) && e) { e->a = as_parse_unary(P); as_span(P, e); }
        P->nest--;
        return P->rc ? NULL : e;
    }
    if (as_accept_op(P, '+')) {
        if (as_nest_in(P)) e = as_parse_unary(P);
        P->nest--;
        return P->rc ? NULL : e;
    }
    return as_parse_primary(P);
}

static as_expr *as_bin(as_parser *P, int k, int op, as_expr *a, as_expr *b, const char *s0) {
    as_expr *e = as_node(P, k, s0);
    if (!e) return NULL;
    e->op = (uint8_t)op;
    e->a = a;
    e->b = b;
    as_span(P, e);
    return e;
}

static as_expr *as_parse_mul(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e = as_parse_unary(P);
    while (!P->rc && P->t.k == K_OP && (P->t.op == '*' || P->t.op == '/' || P->t.op == '%')) {
        int op = P->t.op;
        as_lex(P);
        e = as_bin(P, E_BIN, op, e, as_parse_unary(P), s0);
    }
    return e;
}

static as_expr *as_parse_add(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e = as_parse_mul(P);
    while (!P->rc && P->t.k == K_OP && (P->t.op == '+' || P->t.op == '-')) {
        int op = P->t.op;
        as_lex(P);
        e = as_bin(P, E_BIN, op, e, as_parse_mul(P), s0);
    }
    return e;
}

static as_expr *as_parse_cmp(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e = as_parse_add(P);
    int neg;
    if (P->rc) return NULL;
    if (P->t.k == K_OP && strchr("=<>", P->t.op ? P->t.op : 1)) {
        int op = P->t.op;
        as_lex(P);
        return as_bin(P, E_BIN, op, e, as_parse_add(P), s0);
    }
    if (P->t.k == K_OP && (P->t.op == OP_LE || P->t.op == OP_GE || P->t.op == OP_NE)) {
        int op = P->t.op;
        as_lex(P);
        return as_bin(P, E_BIN, op, e, as_parse_add(P), s0);
    }
    if (as_accept_kw(P, "IS")) {
        as_expr *r = as_node(P, E_ISNULL, s0);
        if (!r) return NULL;
        r->notf = (uint8_t)as_accept_kw(P, "NOT");
        if (!as_expect_kw(P, "NULL")) return NULL;
        r->a = e;
        as_span(P, r);
        return r;
    }
    neg = as_accept_kw(P, "NOT");
    if (as_accept_kw(P, "LIKE")) {
        as_expr *r = as_bin(P, E_LIKE, 0, e, as_parse_add(P), s0);
        if (r) r->notf = (uint8_t)neg;
        return r;
    }
    if (as_accept_kw(P, "BETWEEN")) {
        as_expr *r = as_node(P, E_BETWEEN, s0);
        if (!r) return NULL;
        r->notf = (uint8_t)neg;
        r->a = e;
        r->b = as_parse_add(P);
        if (!as_expect_kw(P, "AND")) return NULL;
        r->c = as_parse_add(P);
        as_span(P, r);
        return r;
    }
    if (neg) { as_perr(P, "expected LIKE or BETWEEN near "); return NULL; }
    return e;
}

static as_expr *as_parse_not(as_parser *P) {
    const char *s0 = P->t.s;
    if (as_accept_kw(P, "NOT")) {
        as_expr *e = as_node(P, E_NOT, s0);
        if (as_nest_in(P) && e) { e->a = as_parse_not(P); as_span(P, e); }
        P->nest--;
        return P->rc ? NULL : e;
    }
    return as_parse_cmp(P);
}

static as_expr *as_parse_and(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e = as_parse_not(P);
    while (!P->rc && as_accept_kw(P, "AND")) e = as_bin(P, E_AND, 0, e, as_parse_not(P), s0);
    return e;
}

/* 1 if the tree is deeper than AS_MAXDEPTH. Stops descending at the limit,
 * so it never recurses deeper than that itself. A long chain such as
 * 1+1+1+... is built by a loop, not by recursion, so it is checked here. */
static int as_too_deep(const as_expr *e, int d) {
    if (!e) return 0;
    if (d > AS_MAXDEPTH) return 1;
    return as_too_deep(e->a, d + 1) || as_too_deep(e->b, d + 1) || as_too_deep(e->c, d + 1);
}

static as_expr *as_parse_expr(as_parser *P) {
    const char *s0 = P->t.s;
    as_expr *e = NULL;
    if (as_nest_in(P)) {
        e = as_parse_and(P);
        while (!P->rc && as_accept_kw(P, "OR")) e = as_bin(P, E_OR, 0, e, as_parse_and(P), s0);
    }
    P->nest--;
    if (!P->rc && P->nest == 0 && as_too_deep(e, 1))
        P->rc = as_err(P->db, ALTSQL_SYNTAX, "expression nested too deeply");
    return P->rc ? NULL : e;
}

/* ---- Evaluation ------------------------------------------------------------------ */
typedef struct as_acc {
    int64_t n, isum;
    double rsum;
    int real, has, cap[2];
    altsql_value mn, mx;
    char *buf[2];                 /* text buffers for MIN and MAX, reused */
} as_acc;
typedef struct as_ctx { const altsql_value *row; as_acc *acc; as_arena *tmp; as_arena *perm; } as_ctx;

static int as_like(const char *s, size_t sn, const char *p, size_t pn) {
    size_t si = 0, pi = 0, sp = (size_t)-1, ss = 0;
    while (si < sn) {
        if (pi < pn && p[pi] == '%') { sp = pi++; ss = si; }
        else if (pi < pn && (p[pi] == '_' || as_lower((unsigned char)p[pi]) == as_lower((unsigned char)s[si]))) { si++; pi++; }
        else if (sp != (size_t)-1) { pi = sp + 1; si = ++ss; }
        else return 0;
    }
    while (pi < pn && p[pi] == '%') pi++;
    return pi == pn;
}

static double as_round(double x, int64_t n) {
    double m = 1.0, y;
    int64_t i;
    if (n < 0) n = 0;
    if (n > 15) n = 15;
    for (i = 0; i < n; i++) m *= 10.0;
    y = x * m;
    if (!(y > -9.0e18 && y < 9.0e18)) return x;         /* too big to round, or not a number */
    y = (double)(int64_t)(y >= 0 ? y + 0.5 : y - 0.5);
    return y / m;
}

static int as_eval(as_ctx *c, const as_expr *e, altsql_value *o);

static int as_mul_overflows(int64_t a, int64_t b) {
    if (a > 0) return b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a;
    return b > 0 ? a < INT64_MIN / b : (a != 0 && b < INT64_MAX / a);
}

/* Integer arithmetic stays integer; on overflow the result becomes real. */
static int as_arith(int op, const altsql_value *x, const altsql_value *y, altsql_value *o) {
    double a, b;
    if (!as_isnum(x) || !as_isnum(y)) { as_setnull(o); return 0; }
    if (x->type == ALTSQL_INTEGER && y->type == ALTSQL_INTEGER) {
        int64_t i = x->u.i, j = y->u.i;
        switch (op) {
        case '+':
            if ((j > 0 && i > INT64_MAX - j) || (j < 0 && i < INT64_MIN - j)) break;
            as_setint(o, i + j); return 0;
        case '-':
            if ((j < 0 && i > INT64_MAX + j) || (j > 0 && i < INT64_MIN + j)) break;
            as_setint(o, i - j); return 0;
        case '*':
            if (as_mul_overflows(i, j)) break;
            as_setint(o, i * j); return 0;
        case '/':
            if (j == 0) as_setnull(o);
            else if (j == -1 && i == INT64_MIN) break;
            else as_setint(o, i / j);
            return 0;
        default:
            if (j == 0) as_setnull(o);
            else as_setint(o, j == -1 ? 0 : i % j);
            return 0;
        }
    }
    a = as_num(x);
    b = as_num(y);
    switch (op) {
    case '+': as_setreal(o, a + b); return 0;
    case '-': as_setreal(o, a - b); return 0;
    case '*': as_setreal(o, a * b); return 0;
    case '/': if (b == 0.0) as_setnull(o); else as_setreal(o, a / b); return 0;
    default:
        if (b == 0.0) as_setnull(o);
        else {
            double q = a / b;
            if (!(q > -9.0e18 && q < 9.0e18)) as_setnull(o);
            else as_setreal(o, a - (double)(int64_t)q * b);
        }
        return 0;
    }
}

static int as_eval(as_ctx *c, const as_expr *e, altsql_value *o) {
    altsql_value x, y, z;
    int rc;
    switch (e->k) {
    case E_LIT: *o = e->v; return 0;
    case E_COL:
        if (c->row) *o = c->row[e->idx]; else as_setnull(o);
        return 0;
    case E_AGG: {
        const as_acc *a = &c->acc[e->idx];
        switch (e->fn) {
        case F_COUNT: as_setint(o, a->n); return 0;
        case F_SUM:
            if (!a->has) as_setnull(o);
            else if (a->real) as_setreal(o, a->rsum);
            else as_setint(o, a->isum);
            return 0;
        case F_AVG:
            if (!a->has || !a->n) as_setnull(o);
            else as_setreal(o, (a->real ? a->rsum : (double)a->isum) / (double)a->n);
            return 0;
        case F_MIN: if (a->has) *o = a->mn; else as_setnull(o); return 0;
        default:    if (a->has) *o = a->mx; else as_setnull(o); return 0;
        }
    }
    case E_NEG:
        if ((rc = as_eval(c, e->a, &x)) != 0) return rc;
        if (x.type == ALTSQL_INTEGER && x.u.i != INT64_MIN) as_setint(o, -x.u.i);
        else if (x.type == ALTSQL_INTEGER) as_setreal(o, -(double)x.u.i);
        else if (x.type == ALTSQL_REAL) as_setreal(o, -x.u.r);
        else as_setnull(o);
        return 0;
    case E_NOT:
        if ((rc = as_eval(c, e->a, &x)) != 0) return rc;
        if (x.type == ALTSQL_NULL) as_setnull(o); else as_setint(o, !as_truth(&x));
        return 0;
    case E_AND: case E_OR: {
        int isand = e->k == E_AND, xn, yn, xt, yt;
        if ((rc = as_eval(c, e->a, &x)) != 0) return rc;
        xn = x.type == ALTSQL_NULL;
        xt = as_truth(&x);
        if (!xn && xt != isand) { as_setint(o, xt); return 0; }     /* short circuit */
        if ((rc = as_eval(c, e->b, &y)) != 0) return rc;
        yn = y.type == ALTSQL_NULL;
        yt = as_truth(&y);
        if (!yn && yt != isand) { as_setint(o, yt); return 0; }
        if (xn || yn) as_setnull(o); else as_setint(o, isand);
        return 0;
    }
    case E_BIN:
        if ((rc = as_eval(c, e->a, &x)) != 0) return rc;
        if ((rc = as_eval(c, e->b, &y)) != 0) return rc;
        if (strchr("+-*/%", e->op)) return as_arith(e->op, &x, &y, o);
        if (x.type == ALTSQL_NULL || y.type == ALTSQL_NULL) { as_setnull(o); return 0; }
        rc = as_cmp(&x, &y);
        switch (e->op) {
        case '=':   as_setint(o, rc == 0); break;
        case OP_NE: as_setint(o, rc != 0); break;
        case '<':   as_setint(o, rc < 0); break;
        case OP_LE: as_setint(o, rc <= 0); break;
        case '>':   as_setint(o, rc > 0); break;
        default:    as_setint(o, rc >= 0); break;
        }
        return 0;
    case E_ISNULL:
        if ((rc = as_eval(c, e->a, &x)) != 0) return rc;
        as_setint(o, (x.type == ALTSQL_NULL) != e->notf);
        return 0;
    case E_BETWEEN:
        if ((rc = as_eval(c, e->a, &x)) != 0 || (rc = as_eval(c, e->b, &y)) != 0 || (rc = as_eval(c, e->c, &z)) != 0) return rc;
        if (x.type == ALTSQL_NULL || y.type == ALTSQL_NULL || z.type == ALTSQL_NULL) { as_setnull(o); return 0; }
        as_setint(o, (as_cmp(&x, &y) >= 0 && as_cmp(&x, &z) <= 0) != e->notf);
        return 0;
    case E_LIKE:
        if ((rc = as_eval(c, e->a, &x)) != 0 || (rc = as_eval(c, e->b, &y)) != 0) return rc;
        if (x.type != ALTSQL_TEXT || y.type != ALTSQL_TEXT) { as_setnull(o); return 0; }
        as_setint(o, as_like(x.u.s, (size_t)x.len, y.u.s, (size_t)y.len) != e->notf);
        return 0;
    case E_FN:
        if ((rc = as_eval(c, e->a, &x)) != 0) return rc;
        switch (e->fn) {
        case F_ABS:
            if (x.type == ALTSQL_INTEGER && x.u.i == INT64_MIN) as_setreal(o, -(double)x.u.i);
            else if (x.type == ALTSQL_INTEGER) as_setint(o, x.u.i < 0 ? -x.u.i : x.u.i);
            else if (x.type == ALTSQL_REAL) as_setreal(o, x.u.r < 0 ? -x.u.r : x.u.r);
            else as_setnull(o);
            return 0;
        case F_ROUND: {
            int64_t n = 0;
            if (e->b) {
                if ((rc = as_eval(c, e->b, &y)) != 0) return rc;
                if (y.type == ALTSQL_INTEGER) n = y.u.i;
                else {                              /* as_round keeps 0 to 15 digits */
                    double d = as_num(&y);
                    n = d >= 15.0 ? 15 : d > 0.0 ? (int64_t)d : 0;
                }
            }
            if (!as_isnum(&x)) as_setnull(o); else as_setreal(o, as_round(as_num(&x), n));
            return 0;
        }
        case F_LENGTH:
            if (x.type == ALTSQL_TEXT) as_setint(o, x.len);
            else if (x.type == ALTSQL_NULL) as_setnull(o);
            else {
                char nb[40];
                as_setint(o, (int64_t)as_fmt_value(&x, nb, sizeof nb));
            }
            return 0;
        default: {        /* LOWER, UPPER */
            char *d;
            int i;
            if (x.type != ALTSQL_TEXT) { *o = x; return 0; }
            d = (char *)as_alloc(c->tmp, (size_t)x.len + 1);
            if (!d) return ALTSQL_NOMEM;
            for (i = 0; i < x.len; i++) {
                int ch = (unsigned char)x.u.s[i];
                d[i] = (char)(e->fn == F_LOWER ? as_lower(ch) : (ch >= 'a' && ch <= 'z' ? ch - 32 : ch));
            }
            *o = x;
            o->u.s = d;
            return 0;
        }
        }
    default:
        as_setnull(o);
        return 0;
    }
}

/* ---- Queries ------------------------------------------------------------------------ */
typedef struct as_group {
    struct as_group *next, *order;
    uint32_t hash;
    int hasrep;
    altsql_value *keys, *rep;
    as_acc *acc;
} as_group;

typedef struct as_q {
    altsql *db;
    as_arena *A, tmp;
    int kind;                               /* 0 no table, 1 series, 2 kv */
    as_series *S;
    int ncols;
    const char *cname[AS_MAXCOLS];
    size_t cnlen[AS_MAXCOLS];
    as_expr *items[AS_MAXITEMS];
    const char *names[AS_MAXITEMS];
    const char *alias[AS_MAXITEMS];         /* "AS name" of each item, or NULL */
    size_t alen[AS_MAXITEMS];
    int nitems;
    as_expr *where, *having;
    as_expr *group[AS_MAXGROUP];
    int ngroup;
    as_expr *order[AS_MAXGROUP];
    int odesc[AS_MAXGROUP], norder;
    as_expr *aggs[AS_MAXITEMS];
    int naggs, grouped;
    int64_t limit, offset, emitted, skipped, tmin;
    int stop;
    altsql_row_cb cb;
    void *cbctx;
    as_group **tab, *first, *last;
    uint32_t nbuckets;
    altsql_value **rows;
    size_t nrows, caprows;
    altsql_value *outv;
} as_q;

static void as_colname_store(void *ctx, int idx, const char *name, size_t n) {
    as_q *q = (as_q *)ctx;
    q->cname[idx] = name;
    q->cnlen[idx] = n;
}

static int as_has_agg(const as_expr *e) {
    if (!e) return 0;
    if (e->k == E_AGG) return 1;
    return as_has_agg(e->a) || as_has_agg(e->b) || as_has_agg(e->c);
}

/* Depth and size of a resolved expression. An alias is resolved by sharing
 * the item's tree, so a chain of aliases (b = a + a, c = b + b, ...) can
 * stand for a huge expression; each node is measured once, so this stays
 * quick, and the caller refuses expressions that are too deep or too big. */
static void as_measure(as_expr *e) {
    as_expr *kid[3];
    uint32_t size = 1, d = 0;
    int i;
    if (e->depth) return;
    kid[0] = e->a; kid[1] = e->b; kid[2] = e->c;
    for (i = 0; i < 3; i++) {
        if (!kid[i]) continue;
        as_measure(kid[i]);
        if (kid[i]->depth > d) d = kid[i]->depth;
        size += kid[i]->size;
        if (size > AS_MAXNODES) size = AS_MAXNODES + 1;
    }
    e->depth = (uint16_t)(d < AS_MAXDEPTH ? d + 1 : AS_MAXDEPTH + 1);
    e->size = size;
}

static int as_checked(as_parser *P, as_q *q, as_expr *e) {
    if (!e) return 0;
    as_measure(e);
    if (e->depth > AS_MAXDEPTH || e->size > AS_MAXNODES)
        return P->rc = as_err(q->db, ALTSQL_SYNTAX, "expression too large once aliases are expanded");
    return 0;
}

/* Resolves names to columns (or to select items by alias) and collects
 * aggregates. in_agg: inside an aggregate call. */
static int as_resolve(as_parser *P, as_q *q, as_expr *e, int allow_agg, int in_agg) {
    int i, rc;
    if (!e) return 0;
    if (e->k == E_NAME) {
        for (i = 0; i < q->ncols; i++) {
            if (as_ieq(e->s0, e->sn, q->cname[i], q->cnlen[i])) {
                e->k = E_COL;
                e->idx = (int16_t)i;
                return 0;
            }
        }
        for (i = 0; i < q->nitems && !in_agg; i++) {
            if (q->alias[i] && as_ieq(e->s0, e->sn, q->alias[i], q->alen[i]) && q->items[i]->k != E_NAME) {
                if (!allow_agg && as_has_agg(q->items[i]))
                    return P->rc = as_err(q->db, ALTSQL_SYNTAX, "aggregate functions are not allowed here");
                *e = *q->items[i];            /* the item is already resolved */
                return 0;
            }
        }
        return P->rc = as_err2(q->db, ALTSQL_SCHEMA, "no such column: ", e->s0, e->sn);
    }
    if (e->k == E_FN && e->fn <= F_MAX) {
        if (!allow_agg || in_agg) return P->rc = as_err(q->db, ALTSQL_SYNTAX, "aggregate functions are not allowed here");
        if (q->naggs >= AS_MAXITEMS) return P->rc = as_err(q->db, ALTSQL_NOMEM, "too many aggregates");
        e->k = E_AGG;
        e->idx = (int16_t)q->naggs;
        q->aggs[q->naggs++] = e;
        if (e->a && e->a->k != E_STAR) return as_resolve(P, q, e->a, allow_agg, 1);
        return 0;
    }
    if ((rc = as_resolve(P, q, e->a, allow_agg, in_agg)) != 0) return rc;
    if ((rc = as_resolve(P, q, e->b, allow_agg, in_agg)) != 0) return rc;
    return as_resolve(P, q, e->c, allow_agg, in_agg);
}

/* Name of a select item: alias, column name, or the expression text. */
static const char *as_strdup_n(as_arena *A, const char *s, size_t n) {
    char *d = (char *)as_alloc(A, n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* An integer literal or an alias in GROUP BY / ORDER BY refers to a select item. */
static int as_item_ref(as_q *q, const as_expr *e) {
    int i;
    if (e->k == E_LIT && e->v.type == ALTSQL_INTEGER && e->v.u.i >= 1 && e->v.u.i <= q->nitems)
        return (int)e->v.u.i - 1;
    if (e->k == E_NAME)
        for (i = 0; i < q->nitems; i++)
            if (q->alias[i] && as_ieq(e->s0, e->sn, q->alias[i], q->alen[i])) return i;
    return -1;
}

/* Lower bound on the time column implied by WHERE, used to skip sectors. */
static void as_q_bounds(as_q *q, const as_expr *e) {
    const as_expr *lit = NULL;
    int op, flip = 0;
    int64_t L;
    if (!e || q->kind != 1) return;
    if (e->k == E_AND) { as_q_bounds(q, e->a); as_q_bounds(q, e->b); return; }
    if (e->k == E_BETWEEN && !e->notf && e->a->k == E_COL && e->a->idx == 0 && e->b->k == E_LIT) {
        lit = e->b;
        op = OP_GE;
    } else if (e->k == E_BIN) {
        if (e->a->k == E_COL && e->a->idx == 0 && e->b->k == E_LIT) lit = e->b;
        else if (e->b->k == E_COL && e->b->idx == 0 && e->a->k == E_LIT) { lit = e->a; flip = 1; }
        op = e->op;
        if (flip) op = op == '<' ? '>' : op == '>' ? '<' : op == OP_LE ? OP_GE : op == OP_GE ? OP_LE : op;
    } else {
        return;
    }
    if (!lit || !as_isnum(&lit->v) || !(op == '>' || op == OP_GE || op == '=')) return;
    if (lit->v.type == ALTSQL_REAL) {
        if (!(lit->v.u.r > -9.0e18 && lit->v.u.r < 9.0e18)) return;
        L = (int64_t)lit->v.u.r - 1;
    } else {
        L = lit->v.u.i;
    }
    if (L > q->tmin) q->tmin = L;
}

/* Keeps a copy of v as the running MIN (which 0) or MAX (which 1). */
static int as_acc_keep(as_arena *A, as_acc *a, int which, const altsql_value *v) {
    altsql_value *dst = which ? &a->mx : &a->mn;
    *dst = *v;
    if (v->type == ALTSQL_TEXT && v->len > 0) {
        if (a->cap[which] < v->len) {
            int c = v->len < 32 ? 32 : v->len;
            a->buf[which] = (char *)as_alloc(A, (size_t)c);
            if (!a->buf[which]) return ALTSQL_NOMEM;
            a->cap[which] = c;
        }
        memcpy(a->buf[which], v->u.s, (size_t)v->len);
        dst->u.s = a->buf[which];
    }
    return ALTSQL_OK;
}

static int as_acc_add(as_arena *A, as_acc *a, int fn, const altsql_value *v, int star) {
    if (fn == F_COUNT) { if (star || v->type != ALTSQL_NULL) a->n++; return 0; }
    if (v->type == ALTSQL_NULL) return 0;
    if (fn == F_SUM || fn == F_AVG) {
        if (!as_isnum(v)) return 0;
        a->n++;
        if (v->type == ALTSQL_INTEGER && !a->real) {
            int64_t x = v->u.i;
            if ((x > 0 && a->isum > INT64_MAX - x) || (x < 0 && a->isum < INT64_MIN - x)) {
                a->rsum = (double)a->isum + (double)x;      /* would overflow: go real */
                a->real = 1;
            } else {
                a->isum += x;
            }
        } else {
            if (!a->real) { a->rsum = (double)a->isum; a->real = 1; }
            a->rsum += as_num(v);
        }
        a->has = 1;
        return 0;
    }
    if (fn == F_MIN && a->has && as_cmp(v, &a->mn) >= 0) return 0;
    if (fn == F_MAX && a->has && as_cmp(v, &a->mx) <= 0) return 0;
    a->has = 1;
    return as_acc_keep(A, a, fn == F_MAX, v);
}

static int as_q_emit(as_q *q, const altsql_value *v) {
    if (q->skipped < q->offset) { q->skipped++; return 0; }
    if (q->limit >= 0 && q->emitted >= q->limit) { q->stop = 1; return 0; }
    q->emitted++;
    if (q->cb && q->cb(q->cbctx, q->nitems, v, q->names)) q->stop = 1;
    if (q->limit >= 0 && q->emitted >= q->limit) q->stop = 1;
    return 0;
}

static int as_rowcmp(const as_q *q, const altsql_value *a, const altsql_value *b) {
    int i, c;
    for (i = 0; i < q->norder; i++) {
        c = as_cmp(&a[q->nitems + i], &b[q->nitems + i]);
        if (c) return q->odesc[i] ? -c : c;
    }
    return 0;
}

/* Copies the current output into row, reusing its text space when it fits. */
static int as_q_fill(as_q *q, altsql_value *row, int reuse) {
    int i, n = q->nitems + q->norder;
    for (i = 0; i < n; i++) {
        const altsql_value *v = &q->outv[i];
        if (reuse && v->type == ALTSQL_TEXT && row[i].type == ALTSQL_TEXT && v->len <= row[i].len) {
            char *d = (char *)(size_t)row[i].u.s;       /* our own copy, made earlier */
            memcpy(d, v->u.s, (size_t)v->len);
            row[i].len = v->len;
            continue;
        }
        if (as_vcopy(q->A, &row[i], v)) return as_err(q->db, ALTSQL_NOMEM, "out of working memory");
    }
    return 0;
}

/* Max-heap on the ORDER BY keys: the root is the worst row kept. */
static void as_heap_down(as_q *q, size_t i) {
    for (;;) {
        size_t c = 2 * i + 1;
        altsql_value *t;
        if (c >= q->nrows) return;
        if (c + 1 < q->nrows && as_rowcmp(q, q->rows[c + 1], q->rows[c]) > 0) c++;
        if (as_rowcmp(q, q->rows[c], q->rows[i]) <= 0) return;
        t = q->rows[i]; q->rows[i] = q->rows[c]; q->rows[c] = t;
        i = c;
    }
}

static void as_heap_up(as_q *q, size_t i) {
    while (i > 0) {
        size_t p = (i - 1) / 2;
        altsql_value *t;
        if (as_rowcmp(q, q->rows[i], q->rows[p]) <= 0) return;
        t = q->rows[i]; q->rows[i] = q->rows[p]; q->rows[p] = t;
        i = p;
    }
}

/* Evaluates the select items (and ORDER BY keys) for one row or group.
 * With ORDER BY, rows are kept for sorting; with ORDER BY and LIMIT only
 * the best OFFSET + LIMIT rows are kept, so memory stays small. */
static int as_q_out(as_q *q, as_ctx *c) {
    int i, rc, n = q->nitems + q->norder;
    int64_t keep = -1;
    for (i = 0; i < q->nitems; i++) if ((rc = as_eval(c, q->items[i], &q->outv[i])) != 0) return rc;
    if (!q->norder) return as_q_emit(q, q->outv);
    for (i = 0; i < q->norder; i++) if ((rc = as_eval(c, q->order[i], &q->outv[q->nitems + i])) != 0) return rc;
    if (q->limit >= 0 && q->offset <= INT64_MAX - q->limit) keep = q->limit + q->offset;
    if (keep == 0) return 0;
    if (keep > 0 && (int64_t)q->nrows == keep) {                /* top-N: replace the worst */
        if (as_rowcmp(q, q->outv, q->rows[0]) >= 0) return 0;
        if ((rc = as_q_fill(q, q->rows[0], 1)) != 0) return rc;
        as_heap_down(q, 0);
        return 0;
    }
    if (q->nrows == q->caprows) {
        size_t nc = q->caprows ? q->caprows * 2 : 256;
        altsql_value **nr = (altsql_value **)as_alloc(q->A, nc * sizeof *nr);
        if (!nr) return as_err(q->db, ALTSQL_NOMEM, "result too large to sort; add LIMIT or raise working memory");
        if (q->nrows) memcpy(nr, q->rows, q->nrows * sizeof *nr);
        q->rows = nr;
        q->caprows = nc;
    }
    {
        altsql_value *row = (altsql_value *)as_alloc(q->A, (size_t)n * sizeof *row);
        if (!row) return as_err(q->db, ALTSQL_NOMEM, "result too large to sort; add LIMIT or raise working memory");
        if ((rc = as_q_fill(q, row, 0)) != 0) return rc;
        q->rows[q->nrows++] = row;
        if (keep > 0) as_heap_up(q, q->nrows - 1);
    }
    return 0;
}

static as_group *as_q_newgroup(as_q *q, uint32_t h, const altsql_value *keys, const altsql_value *row) {
    as_group *g = (as_group *)as_alloc(q->A, sizeof *g);
    int i;
    if (!g) return NULL;
    memset(g, 0, sizeof *g);
    g->hash = h;
    g->keys = (altsql_value *)as_alloc(q->A, sizeof(altsql_value) * (size_t)(q->ngroup + 1));
    g->rep = (altsql_value *)as_alloc(q->A, sizeof(altsql_value) * (size_t)(q->ncols + 1));
    g->acc = (as_acc *)as_alloc(q->A, sizeof(as_acc) * (size_t)(q->naggs + 1));
    if (!g->keys || !g->rep || !g->acc) return NULL;
    memset(g->acc, 0, sizeof(as_acc) * (size_t)(q->naggs + 1));
    for (i = 0; i < q->ngroup; i++) if (as_vcopy(q->A, &g->keys[i], &keys[i])) return NULL;
    for (i = 0; i < q->ncols; i++) {
        if (row) { if (as_vcopy(q->A, &g->rep[i], &row[i])) return NULL; }
        else as_setnull(&g->rep[i]);
    }
    g->hasrep = row != NULL;
    if (q->last) q->last->order = g; else q->first = g;
    q->last = g;
    return g;
}

static int as_q_group(as_q *q, const altsql_value *row) {
    altsql_value keys[AS_MAXGROUP];
    as_ctx c;
    as_group *g;
    uint32_t h = 0;
    int i, rc;
    c.row = row; c.acc = NULL; c.tmp = &q->tmp; c.perm = q->A;
    for (i = 0; i < q->ngroup; i++) {
        if ((rc = as_eval(&c, q->group[i], &keys[i])) != 0) return rc;
        h = h * 31u + as_vhash(&keys[i]);
    }
    if (q->ngroup == 0) g = q->first;
    else {
        for (g = q->tab[h & (q->nbuckets - 1)]; g; g = g->next) {
            if (g->hash != h) continue;
            for (i = 0; i < q->ngroup; i++) {
                if (as_cmp(&g->keys[i], &keys[i]) != 0 || g->keys[i].type != keys[i].type) {
                    if (!(as_isnum(&g->keys[i]) && as_isnum(&keys[i]) && as_cmp(&g->keys[i], &keys[i]) == 0)) break;
                }
            }
            if (i == q->ngroup) break;
        }
        if (!g) {
            g = as_q_newgroup(q, h, keys, row);
            if (!g) return as_err(q->db, ALTSQL_NOMEM, "too many groups for working memory");
            g->next = q->tab[h & (q->nbuckets - 1)];
            q->tab[h & (q->nbuckets - 1)] = g;
        }
    }
    if (!g->hasrep && row) {         /* bare columns next to aggregates: first row */
        for (i = 0; i < q->ncols; i++)
            if (as_vcopy(q->A, &g->rep[i], &row[i])) return as_err(q->db, ALTSQL_NOMEM, "out of working memory");
        g->hasrep = 1;
    }
    for (i = 0; i < q->naggs; i++) {
        const as_expr *ae = q->aggs[i];
        altsql_value v;
        if (ae->a && ae->a->k == E_STAR) { as_acc_add(q->A, &g->acc[i], ae->fn, NULL, 1); continue; }
        if ((rc = as_eval(&c, ae->a, &v)) != 0) return rc;
        if (as_acc_add(q->A, &g->acc[i], ae->fn, &v, 0)) return as_err(q->db, ALTSQL_NOMEM, "out of working memory");
    }
    return 0;
}

static int as_q_row(as_q *q, const altsql_value *row) {
    as_ctx c;
    altsql_value w;
    int rc;
    q->tmp.used = 0;
    c.row = row; c.acc = NULL; c.tmp = &q->tmp; c.perm = q->A;
    if (q->where) {
        if ((rc = as_eval(&c, q->where, &w)) != 0) return rc;
        if (!as_truth(&w)) return 0;
    }
    if (q->grouped) return as_q_group(q, row);
    return as_q_out(q, &c);
}

static int as_q_scan(as_q *q) {
    altsql *db = q->db;
    altsql_value row[AS_MAXCOLS];
    as_iter it;
    as_rec r;
    uint32_t k;
    int rc = 0;
    for (k = 0; k < db->used && !q->stop; k++) {
        if (q->kind == 1 && q->tmin != AS_TIME_NONE && db->sec[as_run_sector(db, k)].max_time < q->tmin) continue;
        as_iter_start(&it, k, k);
        while (!q->stop && (rc = as_next(db, &it, &r)) == 1) {
            if (q->kind == 1) {
                if (r.type != AS_R_ROW || r.len < 2 || as_get16(r.p) != q->S->id) continue;
                if (as_row_decode(q->S, r.p, r.len, row)) continue;
                if (row[0].u.i < q->tmin) continue;
            } else {
                uint32_t klen;
                if (r.type != AS_R_PUT || r.len < 1) continue;
                klen = r.p[0];
                if (!klen || 1u + klen > r.len || r.p[1] == 0x01) continue;
                if (as_slot_of(db, as_hash(r.p + 1, klen), r.addr) == AS_ALL) continue;
                row[0].type = ALTSQL_TEXT; row[0].u.s = (const char *)r.p + 1; row[0].len = (int)klen;
                row[1].type = ALTSQL_TEXT; row[1].u.s = (const char *)r.p + 1 + klen; row[1].len = (int)(r.len - 1 - klen);
            }
            if ((rc = as_q_row(q, row)) != 0) return rc;
        }
        if (rc < 0) return rc;
    }
    return 0;
}

/* Bottom-up merge sort of collected rows by the ORDER BY keys. */

static int as_q_sort(as_q *q) {
    size_t n = q->nrows, w, i;
    altsql_value **src = q->rows, **dst = (altsql_value **)as_alloc(q->A, (n + 1) * sizeof *dst), **t;
    if (!dst) return as_err(q->db, ALTSQL_NOMEM, "result too large to sort; add LIMIT or raise working memory");
    for (w = 1; w < n; w *= 2) {
        for (i = 0; i < n; i += 2 * w) {
            size_t a = i, am = i + w < n ? i + w : n, b = am, bm = i + 2 * w < n ? i + 2 * w : n, o = i;
            while (a < am && b < bm) dst[o++] = as_rowcmp(q, src[b], src[a]) < 0 ? src[b++] : src[a++];
            while (a < am) dst[o++] = src[a++];
            while (b < bm) dst[o++] = src[b++];
        }
        t = src; src = dst; dst = t;
    }
    q->rows = src;
    return 0;
}

static int as_is_const(const as_expr *e) {
    if (!e) return 1;
    if (e->k == E_NAME || e->k == E_STAR || (e->k == E_FN && e->fn <= F_MAX)) return 0;
    return as_is_const(e->a) && as_is_const(e->b) && as_is_const(e->c);
}

static int as_select(as_parser *P, altsql_row_cb cb, void *cbctx) {
    altsql *db = P->db;
    as_q *q = (as_q *)as_alloc(P->A, sizeof *q);
    const char *tname = NULL;
    size_t tlen = 0;
    char *schema = NULL;
    int i, star = 0, rc;
    if (!q) return as_err(db, ALTSQL_NOMEM, "out of working memory");
    memset(q, 0, sizeof *q);
    q->db = db;
    q->A = P->A;
    q->limit = -1;
    q->tmin = AS_TIME_NONE;
    q->cb = cb;
    q->cbctx = cbctx;
    q->tmp.base = db->work;             /* per-row scratch: start of working memory */
    q->tmp.cap = AS_TMP_SIZE;
    q->tmp.used = 0;

    /* select list */
    if (as_accept_op(P, '*')) star = 1;
    else {
        do {
            as_expr *e;
            if (q->nitems >= AS_MAXITEMS) return as_err(db, ALTSQL_NOMEM, "too many result columns");
            e = as_parse_expr(P);
            if (!e) return P->rc;
            if (as_accept_kw(P, "AS") || (P->t.k == K_ID && !as_reserved(P))) {
                if (P->t.k != K_ID) return as_perr(P, "expected a name after AS near ");
                q->alias[q->nitems] = P->t.s;
                q->alen[q->nitems] = P->t.n;
                as_lex(P);
            }
            q->items[q->nitems++] = e;
        } while (as_accept_op(P, ','));
    }
    if (as_accept_kw(P, "FROM")) {
        if (P->t.k != K_ID) return as_perr(P, "expected a table name near ");
        tname = P->t.s;
        tlen = P->t.n;
        as_lex(P);
    }
    if (as_accept_kw(P, "WHERE")) { q->where = as_parse_expr(P); if (P->rc) return P->rc; }
    if (as_accept_kw(P, "GROUP")) {
        if (!as_expect_kw(P, "BY")) return P->rc;
        do {
            if (q->ngroup >= AS_MAXGROUP) return as_err(db, ALTSQL_NOMEM, "too many GROUP BY terms");
            q->group[q->ngroup++] = as_parse_expr(P);
            if (P->rc) return P->rc;
        } while (as_accept_op(P, ','));
    }
    if (as_accept_kw(P, "HAVING")) { q->having = as_parse_expr(P); if (P->rc) return P->rc; }
    if (as_accept_kw(P, "ORDER")) {
        if (!as_expect_kw(P, "BY")) return P->rc;
        do {
            if (q->norder >= AS_MAXGROUP) return as_err(db, ALTSQL_NOMEM, "too many ORDER BY terms");
            q->order[q->norder] = as_parse_expr(P);
            if (P->rc) return P->rc;
            q->odesc[q->norder] = as_accept_kw(P, "DESC");
            if (!q->odesc[q->norder]) as_accept_kw(P, "ASC");
            q->norder++;
        } while (as_accept_op(P, ','));
    }
    if (as_accept_kw(P, "LIMIT")) {
        if (P->t.k != K_INT) return as_perr(P, "LIMIT needs a whole number near ");
        q->limit = P->t.i;
        as_lex(P);
        if (as_accept_kw(P, "OFFSET")) {
            if (P->t.k != K_INT) return as_perr(P, "OFFSET needs a whole number near ");
            q->offset = P->t.i;
            as_lex(P);
        }
    }
    if (P->t.k != K_END && !as_isop(P, ';')) return as_perr(P, "syntax error near ");

    /* table and its columns */
    if (tname) {
        if (as_ieq(tname, tlen, "kv", 2)) {
            q->kind = 2;
            q->ncols = 2;
            q->cname[0] = "key"; q->cnlen[0] = 3;
            q->cname[1] = "value"; q->cnlen[1] = 5;
            if (!db->kv_complete) return as_err(db, ALTSQL_NOMEM, "key index too small to list keys; raise kv_slots");
        } else {
            uint8_t types[AS_MAXCOLS];
            q->kind = 1;
            q->S = as_series_find(db, tname, tlen);
            if (!q->S) return as_err2(db, ALTSQL_SCHEMA, "no such table: ", tname, tlen);
            schema = (char *)as_alloc(P->A, AS_MAXSCHEMA + 1);
            if (!schema) return as_err(db, ALTSQL_NOMEM, "out of working memory");
            if ((rc = as_series_schema(db, q->S, schema, AS_MAXSCHEMA + 1)) != 0) return rc;
            q->ncols = as_schema_parse(schema, strlen(schema), types, as_colname_store, q);
            if (q->ncols < 1) return as_err(db, ALTSQL_CORRUPT, "series schema damaged");
        }
    }
    if (star) {
        if (!q->kind) return as_err(db, ALTSQL_SYNTAX, "SELECT * needs a FROM clause");
        for (i = 0; i < q->ncols; i++) {
            as_expr *e = as_node(P, E_COL, q->cname[i]);
            if (!e) return P->rc;
            e->idx = (int16_t)i;
            e->sn = q->cnlen[i];
            q->items[i] = e;
        }
        q->nitems = q->ncols;
    }

    /* resolve names; GROUP BY and ORDER BY may name a select item */
    {   /* an item may use the aliases of the items before it, not after */
        const char *saved[AS_MAXITEMS];
        for (i = 0; i < q->nitems; i++) { saved[i] = q->alias[i]; q->alias[i] = NULL; }
        for (i = 0; i < q->nitems; i++) {
            if (as_resolve(P, q, q->items[i], 1, 0) || as_checked(P, q, q->items[i])) return P->rc;
            q->alias[i] = saved[i];
        }
    }
    for (i = 0; i < q->ngroup; i++) {
        int ref = as_item_ref(q, q->group[i]);
        if (ref >= 0) {
            if (as_has_agg(q->items[ref])) return as_err(db, ALTSQL_SYNTAX, "GROUP BY cannot use an aggregate");
            q->group[i] = q->items[ref];
        } else if (as_resolve(P, q, q->group[i], 0, 0) || as_checked(P, q, q->group[i])) {
            return P->rc;
        }
    }
    for (i = 0; i < q->norder; i++) {
        int ref = as_item_ref(q, q->order[i]);
        if (ref >= 0) q->order[i] = q->items[ref];
        else if (as_resolve(P, q, q->order[i], 1, 0) || as_checked(P, q, q->order[i])) return P->rc;
    }
    if (q->where && (as_resolve(P, q, q->where, 0, 0) || as_checked(P, q, q->where))) return P->rc;
    if (q->having && (as_resolve(P, q, q->having, 1, 0) || as_checked(P, q, q->having))) return P->rc;
    q->grouped = q->ngroup > 0 || q->naggs > 0;
    if (q->having && !q->grouped) return as_err(db, ALTSQL_SYNTAX, "HAVING needs GROUP BY or an aggregate");

    /* result column names */
    for (i = 0; i < q->nitems; i++) {
        const as_expr *e = q->items[i];
        q->names[i] = q->alias[i] ? as_strdup_n(P->A, q->alias[i], q->alen[i])
                                  : as_strdup_n(P->A, e->s0, e->sn);
        if (!q->names[i]) return as_err(db, ALTSQL_NOMEM, "out of working memory");
    }
    q->outv = (altsql_value *)as_alloc(P->A, sizeof(altsql_value) * (size_t)(q->nitems + q->norder + 1));
    if (!q->outv) return as_err(db, ALTSQL_NOMEM, "out of working memory");
    if (q->grouped) {
        q->nbuckets = 4096;
        q->tab = (as_group **)as_alloc(P->A, sizeof(as_group *) * q->nbuckets);
        if (!q->tab) return as_err(db, ALTSQL_NOMEM, "out of working memory");
        memset(q->tab, 0, sizeof(as_group *) * q->nbuckets);
        if (q->ngroup == 0 && !as_q_newgroup(q, 0, NULL, NULL)) return as_err(db, ALTSQL_NOMEM, "out of working memory");
    }
    as_q_bounds(q, q->where);

    /* run */
    if (q->kind) rc = as_q_scan(q);
    else rc = as_q_row(q, NULL);
    if (rc) return rc < 0 ? rc : as_err(db, rc, "query failed");
    if (q->grouped) {
        as_group *g;
        for (g = q->first; g && !q->stop; g = g->order) {
            as_ctx c;
            altsql_value w;
            q->tmp.used = 0;
            c.row = g->rep; c.acc = g->acc; c.tmp = &q->tmp; c.perm = q->A;
            if (q->having) {
                if ((rc = as_eval(&c, q->having, &w)) != 0) return rc;
                if (!as_truth(&w)) continue;
            }
            if ((rc = as_q_out(q, &c)) != 0) return rc;
        }
    }
    if (q->norder) {
        size_t r;
        if ((rc = as_q_sort(q)) != 0) return rc;
        for (r = 0; r < q->nrows && !q->stop; r++) as_q_emit(q, q->rows[r]);
    }
    return ALTSQL_OK;
}

static int as_type_from_sql(const char *s, size_t n) {
    if (as_ieq(s, n, "TIME", 4) || as_ieq(s, n, "TIMESTAMP", 9)) return AS_T_TIME;
    if (as_ieq(s, n, "INT", 3) || as_ieq(s, n, "SMALLINT", 8) || as_ieq(s, n, "TINYINT", 7)) return AS_T_INT;
    if (as_ieq(s, n, "INTEGER", 7) || as_ieq(s, n, "BIGINT", 6) || as_ieq(s, n, "LONG", 4)) return AS_T_LONG;
    if (as_ieq(s, n, "FLOAT", 5)) return AS_T_FLOAT;
    if (as_ieq(s, n, "REAL", 4) || as_ieq(s, n, "DOUBLE", 6)) return AS_T_REAL;
    if (as_ieq(s, n, "TEXT", 4) || as_ieq(s, n, "VARCHAR", 7) || as_ieq(s, n, "CHAR", 4)) return AS_T_TEXT;
    return 0;
}

static int as_create(as_parser *P) {
    char name[AS_NAMELEN], schema[AS_MAXSCHEMA + 1];
    size_t sl = 0;
    int ifne = 0, col = 0, rc;
    if (!as_expect_kw(P, "TABLE")) return P->rc;
    if (as_accept_kw(P, "IF")) {
        if (!as_expect_kw(P, "NOT") || !as_expect_kw(P, "EXISTS")) return P->rc;
        ifne = 1;
    }
    if (P->t.k != K_ID || P->t.n >= AS_NAMELEN) return as_perr(P, "expected a table name of up to 23 characters near ");
    memcpy(name, P->t.s, P->t.n);
    name[P->t.n] = 0;
    as_lex(P);
    if (!as_expect_op(P, '(')) return P->rc;
    do {
        const char *cn, *tn;
        size_t cl;
        int t;
        if (P->t.k != K_ID) return as_perr(P, "expected a column name near ");
        cn = P->t.s; cl = P->t.n;
        as_lex(P);
        if (P->t.k != K_ID || !(t = as_type_from_sql(P->t.s, P->t.n)))
            return as_perr(P, "expected a type (TIME, INT, INTEGER, FLOAT, REAL, TEXT) near ");
        as_lex(P);
        if (as_accept_op(P, '(')) {                       /* VARCHAR(n): size ignored */
            while (P->t.k != K_END && !as_isop(P, ')')) as_lex(P);
            if (!as_expect_op(P, ')')) return P->rc;
        }
        if (col == 0 && t != AS_T_TIME)
            return as_err(P->db, ALTSQL_SCHEMA, "the first column must be TIME: it holds each row's timestamp");
        if (col > 0 && t == AS_T_TIME) t = AS_T_LONG;    /* further timestamps are plain integers */
        col++;
        tn = as_type_name(t);
        if (sl + cl + strlen(tn) + 3 > AS_MAXSCHEMA) return as_err(P->db, ALTSQL_TOOBIG, "table definition too long");
        if (sl) schema[sl++] = ',';
        memcpy(schema + sl, cn, cl); sl += cl;
        schema[sl++] = ':';
        memcpy(schema + sl, tn, strlen(tn)); sl += strlen(tn);
    } while (as_accept_op(P, ','));
    if (!as_expect_op(P, ')')) return P->rc;
    /* the whole statement is read before anything is written */
    if (P->rc) return P->rc;
    if (P->t.k != K_END && !as_isop(P, ';')) return as_perr(P, "syntax error near ");
    schema[sl] = 0;
    rc = altsql_ts_create(P->db, name, schema);
    if (rc == ALTSQL_EXISTS && ifne) { P->db->err[0] = 0; return ALTSQL_OK; }
    return rc;
}

static int as_insert(as_parser *P) {
    altsql *db = P->db;
    const char *tn;
    size_t tl;
    as_series *S = NULL;
    int map[AS_MAXCOLS], nlist = 0, ncols, i, rc, iskv;
    char schema[AS_MAXSCHEMA + 1];
    const char *cname[AS_MAXCOLS];
    size_t cnlen[AS_MAXCOLS];
    if (!as_expect_kw(P, "INTO")) return P->rc;
    if (P->t.k != K_ID) return as_perr(P, "expected a table name near ");
    tn = P->t.s; tl = P->t.n;
    as_lex(P);
    iskv = as_ieq(tn, tl, "kv", 2);
    if (iskv) ncols = 2;
    else {
        uint8_t types[AS_MAXCOLS];
        as_q tmp;
        S = as_series_find(db, tn, tl);
        if (!S) return as_err2(db, ALTSQL_SCHEMA, "no such table: ", tn, tl);
        if ((rc = as_series_schema(db, S, schema, sizeof schema)) != 0) return rc;
        memset(&tmp, 0, sizeof tmp);
        ncols = as_schema_parse(schema, strlen(schema), types, as_colname_store, &tmp);
        for (i = 0; i < ncols; i++) { cname[i] = tmp.cname[i]; cnlen[i] = tmp.cnlen[i]; }
    }
    for (i = 0; i < ncols; i++) map[i] = i;
    if (as_accept_op(P, '(')) {
        do {
            int j, found = -1;
            if (P->t.k != K_ID) return as_perr(P, "expected a column name near ");
            for (j = 0; j < ncols; j++) {
                const char *nm = iskv ? (j ? "value" : "key") : cname[j];
                size_t nl = iskv ? (j ? 5 : 3) : cnlen[j];
                if (as_ieq(P->t.s, P->t.n, nm, nl)) found = j;
            }
            if (found < 0) return as_err2(db, ALTSQL_SCHEMA, "no such column: ", P->t.s, P->t.n);
            if (nlist >= ncols) return as_err(db, ALTSQL_SCHEMA, "too many columns");
            map[nlist++] = found;
            as_lex(P);
        } while (as_accept_op(P, ','));
        if (!as_expect_op(P, ')')) return P->rc;
        if (nlist != ncols) return as_err(db, ALTSQL_SCHEMA, "INSERT must give every column");
    }
    if (!as_expect_kw(P, "VALUES")) return P->rc;
    do {
        altsql_value in[AS_MAXCOLS], v[AS_MAXCOLS];
        as_ctx c;
        int n = 0;
        size_t mark = P->A->used;
        c.row = NULL; c.acc = NULL; c.tmp = P->A; c.perm = P->A;
        if (!as_expect_op(P, '(')) return P->rc;
        do {
            as_expr *e;
            if (n >= ncols) return as_err(db, ALTSQL_SCHEMA, "too many values");
            e = as_parse_expr(P);
            if (!e) return P->rc;
            if (!as_is_const(e)) return as_err2(db, ALTSQL_SCHEMA, "values must be constants: ", e->s0, e->sn);
            if ((rc = as_eval(&c, e, &in[n])) != 0) return rc;
            n++;
        } while (as_accept_op(P, ','));
        if (!as_expect_op(P, ')')) return P->rc;
        /* rows are written one by one as they are read: a row is written
         * only once what follows it is known to be valid */
        if (P->rc) return P->rc;
        if (P->t.k != K_END && !as_isop(P, ';') && !as_isop(P, ',')) return as_perr(P, "syntax error near ");
        if (n != ncols) return as_err(db, ALTSQL_SCHEMA, "wrong number of values");
        for (i = 0; i < ncols; i++) v[map[i]] = in[i];
        if (iskv) {
            char kb[AS_MAXKEY + 1], nb[40];
            const void *val;
            size_t vl;
            if (v[0].type != ALTSQL_TEXT || v[0].len < 1 || v[0].len > (int)AS_MAXKEY) return as_err(db, ALTSQL_SCHEMA, "kv key must be text of 1 to 200 bytes");
            memcpy(kb, v[0].u.s, (size_t)v[0].len);
            kb[v[0].len] = 0;
            if (v[1].type == ALTSQL_TEXT) { val = v[1].u.s; vl = (size_t)v[1].len; }
            else if (v[1].type != ALTSQL_NULL) { vl = as_fmt_value(&v[1], nb, sizeof nb); val = nb; }
            else { val = ""; vl = 0; }
            rc = altsql_put(db, kb, val, vl);
        } else {
            char nm[AS_NAMELEN];
            memcpy(nm, S->name, sizeof nm);
            rc = altsql_ts_append(db, nm, v, ncols);
        }
        P->A->used = mark;
        if (rc) return rc;
    } while (as_accept_op(P, ','));
    return ALTSQL_OK;
}

static int as_exec(altsql *db, const char *sql, altsql_row_cb cb, void *ctx);

/* Every error comes with a message, whatever path produced it. */
int altsql_exec(altsql *db, const char *sql, altsql_row_cb cb, void *ctx) {
    int rc;
    if (!db || !sql) return ALTSQL_MISUSE;
    rc = as_exec(db, sql, cb, ctx);
    if (rc < 0 && !db->err[0]) as_err(db, rc, rc == ALTSQL_SYNTAX ? "syntax error" : "statement failed");
    return rc;
}

static int as_exec(altsql *db, const char *sql, altsql_row_cb cb, void *ctx) {
    as_parser P;
    as_arena A;
    if (as_ready(db)) return ALTSQL_MISUSE;
    if (db->work_size < AS_TMP_SIZE * 2) return as_err(db, ALTSQL_NOMEM, "SQL needs more working memory (mem_size)");
    memset(&P, 0, sizeof P);
    P.db = db;
    P.p = sql;
    P.A = &A;
    db->err[0] = 0;
    as_lex(&P);
    while (!P.rc) {
        int rc;
        while (as_accept_op(&P, ';')) {}
        if (P.rc || P.t.k == K_END) return P.rc;
        A.base = db->work + AS_TMP_SIZE;
        A.cap = db->work_size - AS_TMP_SIZE;
        A.used = 0;
        if (as_accept_kw(&P, "SELECT")) {
            rc = as_select(&P, cb, ctx);
        } else if (as_accept_kw(&P, "CREATE")) {
            rc = as_create(&P);
        } else if (as_accept_kw(&P, "INSERT")) {
            rc = as_insert(&P);
        } else {
            return as_perr(&P, "expected SELECT, CREATE or INSERT near ");
        }
        if (rc) {
            if (rc < 0 && !db->err[0]) as_err(db, rc, rc == ALTSQL_NOMEM ? "out of working memory" : "statement failed");
            return rc;
        }
        if (P.rc) return P.rc;
        if (P.t.k != K_END && !as_isop(&P, ';')) return as_perr(&P, "syntax error near ");
    }
    return P.rc;
}

#endif /* ALTSQL_ENABLE_SQL */
