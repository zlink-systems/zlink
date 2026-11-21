#!/usr/bin/env bash
case "$1" in
  restore) dotnet restore "${FRAMEWORK_SOLUTION}" ;;
  Debug|Release) dotnet build "${FRAMEWORK_SOLUTION}" -c "$1" --no-restore ;;
  *) exit 2 ;;
esac
