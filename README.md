# AltSql

**A native hybrid of key-value and SQL, from the sensor to the gateway.**

AltSql is one small engine and one set of records. On the device it keeps
key-value and time-series data in flash. On the gateway it answers SQL. The
records are the same on both, byte for byte, so there's nothing to translate
in between. The device keeps working and deciding when the network is gone,
and it sends less.

**Status: 0.1.0-alpha.** Written to prove the design and measure it.
Everything runs on a PC in simulation; nothing has run on a microcontroller
or a gateway yet. File formats may still change. Not for production.

## What's here

| Folder | What it is |
|---|---|
| [`core/`](core/) | **AltSql Core**, the engine: key-value and time-series on the device, SQL on the gateway. The whole engine is one C99 file, `core/dist/altsql.h`. |
| [`engines/db/`](engines/db/) | **AltSql DB**, the database for a gateway that serves a whole fleet: one file for every device, read by key with no SQL step, or with SQL. Built on AltSql Core. |
| [`demos/db/`](demos/db/) | The AltSql DB browser demo. Open `demos/db/app/index.html`; it runs straight from the folder. |

## Build and test

Linux, gcc or clang:

```sh
cd core && make test
cd engines/db && make test
```

Each folder's README says what each test checks, what was measured and the
known limits. The logs behind the numbers are in `core/results/alpha/` and
`engines/db/results/`.

## Try it in the browser

- AltSql Core: https://altsql.com/demo/core/
- AltSql DB: https://altsql.com/demo/db/

More about AltSql and its other modules: https://altsql.com

## Security

See [SECURITY.md](SECURITY.md).

## License

Apache License 2.0. Copyright 2026 AltSql.com. See [LICENSE](LICENSE) and
[NOTICE](NOTICE). Everything in this repository is under Apache 2.0 unless a
file says otherwise.
