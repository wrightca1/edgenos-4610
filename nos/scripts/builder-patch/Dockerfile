# EdgeNOS-4610 rootless-tolerant ONL builder.
# Bakes a patched docker_shell (non-fatal binfmt_misc mount) into the stock
# ONL stretch/armhf builder so it runs under rootless Docker. See BUILD_ENV.md.
FROM dentproject/builder9:1.8
COPY docker_shell /usr/bin/docker_shell
COPY docker_shell /bin/docker_shell
# sudo shim (ahead of /usr/bin/sudo in PATH) that preserves LD_PRELOAD so the
# initrd/RFS steps stay inside the fakeroot session — see sudo-shim header.
COPY sudo-shim /usr/local/bin/sudo
RUN chmod 0755 /usr/bin/docker_shell /bin/docker_shell /usr/local/bin/sudo
