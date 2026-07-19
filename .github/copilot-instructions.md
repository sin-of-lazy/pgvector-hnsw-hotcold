# Copilot Instructions for This Repository

## Build, test, and analysis commands

### Build and install (Linux/macOS)
```sh
make
make install
```

### Build and install (Windows, from x64 Native Tools Command Prompt)
```cmd
nmake /F Makefile.win
nmake /F Makefile.win install
```

### Run tests
```sh
make installcheck
make prove_installcheck
```

### Run a single test
```sh
make installcheck REGRESS=hnsw_vector
make prove_installcheck PROVE_TESTS=test/t/001_ivfflat_wal.pl
```

Windows regression test:
```cmd
nmake /F Makefile.win installcheck
```

Single Windows regression test:
```cmd
nmake /F Makefile.win installcheck REGRESS=hnsw_vector
```

### Lint/static analysis
There is no standalone lint target in this repo. CI relies on strict compiler warnings and also runs `scan-build` in the macOS workflow.

## High-level architecture

- This is a PostgreSQL extension (`vector`) built with PGXS. The extension entrypoint is `_PG_init()` in `src/vector.c`, which initializes type modules and both ANN index families.
- SQL-to-C wiring lives in `sql/vector.sql`. It registers:
  - vector data types/functions/operators
  - index access methods (`hnsw`, `ivfflat`)
  - operator classes that map distance operators to access methods and support functions
- ANN implementation is split into two families:
  - **HNSW**: `src/hnsw*.{c,h}` (handler/init/costing, build, insert, scan, utils, vacuum)
  - **IVFFlat**: `src/ivf*.{c,h}` + `src/ivfflat.c` (same lifecycle split)
- Additional vector representations are implemented separately:
  - `vector` (float32 dense): `src/vector.{c,h}`
  - `halfvec`: `src/halfvec.*`, `src/halfutils.*`
  - `bit`: `src/bitvec.*`, `src/bitutils.*`
  - `sparsevec`: `src/sparsevec.*`
- Upgrade path is migration-script based:
  - canonical schema: `sql/vector.sql`
  - versioned migrations: `sql/vector--X--Y.sql`
  - current release SQL is produced as `sql/vector--0.8.4.sql` from `sql/vector.sql` during build.

## Key conventions specific to this codebase

- Keep PostgreSQL extension wiring consistent across C and SQL:
  - adding/changing a SQL-visible function or operator requires matching entries in `sql/vector.sql` and corresponding C symbols.
- Keep index option semantics split correctly:
  - build-time/index storage options are reloptions (for example `m`, `ef_construction`, `lists`)
  - query-time knobs are GUCs (for example `hnsw.ef_search`, `ivfflat.probes`, iterative scan settings)
- Follow the module lifecycle pattern for index AM changes:
  - handler/init/cost model in `hnsw.c`/`ivfflat.c`
  - build path in `hnswbuild.c`/`ivfbuild.c`
  - insert path in `hnswinsert.c`/`ivfinsert.c`
  - scan path in `hnswscan.c`/`ivfscan.c`
  - maintenance path in `hnswvacuum.c`/`ivfvacuum.c`
- Regression tests are SQL/expected-output pairs:
  - inputs in `test/sql/*.sql`
  - expected outputs in `test/expected/*.out`
  - test names should align so `REGRESS=<name>` works.
- TAP tests are Perl tests in `test/t/*.pl`; prefer adding focused TAP coverage for behavior that is difficult to express in plain SQL regression tests.
- Cross-platform build behavior matters:
  - Unix-like builds use `Makefile` + PGXS
  - Windows builds use `Makefile.win` and require `PGROOT`.
