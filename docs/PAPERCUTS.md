# papercuts 🫛

Every time fingers expect something psh doesn't do, it goes here.
When this list stops growing, that's v1.0.

## open

- `$((...))` doesn't expand `$1`/`$?` etc. inside the expression —
  only bare variable names. bash allows both. Workaround: assign to
  a local first. (Bit the duration plugin.)

- `omp enable <plugin>` only lasts the session; the "add to
  OMP_PLUGINS in ~/.pshrc to keep" hint is easy to miss. Should it
  offer to edit ~/.pshrc itself? (Bit us on day one: python plugin
  looked broken, was just never persisted.)

## fixed

- venv auto-activation only recognized `.venv`; real projects also
  use `venv`. Finder now checks both. (935a64d)
