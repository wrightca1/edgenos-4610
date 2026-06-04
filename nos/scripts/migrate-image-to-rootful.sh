#!/usr/bin/env bash
# Copy the patched builder image from the rootless daemon's store into the
# rootful (system) daemon's store. The two daemons have separate image stores.
# Run once, after `sudo systemctl start docker` and once you're in the docker
# group (you are). No sudo needed here (docker-group access to the socket).
set -euo pipefail
IMG="edgenos/builder9:1.8-rootless"
ROOTLESS="unix:///run/user/$(id -u)/docker.sock"
ROOTFUL="unix:///var/run/docker.sock"

if DOCKER_HOST="$ROOTFUL" docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "$IMG already present in rootful store."; exit 0
fi
echo "Transferring $IMG from rootless -> rootful (10.6 GB, local)…"
DOCKER_HOST="$ROOTLESS" docker save "$IMG" | DOCKER_HOST="$ROOTFUL" docker load
echo "done."
