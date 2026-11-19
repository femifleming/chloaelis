#!/bin/sh
# SWIG 4.3 dropped -no-old-metatable-bindings (that behaviour became the only one). The ER-301
# makefiles still pass it, so it is taken out for a SWIG that no longer knows it.
REAL_SWIG="${REAL_SWIG:-swig}"
if "$REAL_SWIG" -lua -help 2>&1 | grep -q -- -no-old-metatable-bindings; then
  exec "$REAL_SWIG" "$@"
fi
for a in "$@"; do
  shift
  [ "$a" = "-no-old-metatable-bindings" ] || set -- "$@" "$a"
done
exec "$REAL_SWIG" "$@"
