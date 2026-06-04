#!/usr/bin/env bash
# Build the rootless-tolerant ONL builder image used by ../build.sh.
# One-time (or whenever scripts/builder-patch/docker_shell changes).
set -euo pipefail
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
docker build -t edgenos/builder9:1.8-rootless "$HERE/builder-patch"
echo "built edgenos/builder9:1.8-rootless"
