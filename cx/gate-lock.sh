#!/bin/bash
# usage: gate-lock.sh [--lang cpp|java|dotnet|node] <command...>
# One full gate per language at a time; different languages run in parallel.
lock=/root/jobs/fx/cx/gate.lock
if [ "$1" = "--lang" ]; then lock=/root/jobs/fx/cx/gate-$2.lock; shift 2; fi
exec flock "$lock" "$@"
