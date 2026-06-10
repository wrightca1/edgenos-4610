#!/usr/bin/env bash
# build-quagga.sh — cross-compile static Quagga (zebra + ospfd + bgpd) for the
# Edgecore AS4610-54T (armhf / arm-linux-gnueabihf). Output: routing/{zebra,
# ospfd,bgpd}-arm — the routing control plane. Once bcmd's netlink->chip L3 sync
# lands, routes these install in the kernel FIB get programmed into the chip for
# hardware forwarding (OSPF/BGP-learned routes -> ASIC).
#
# Adapted from the proven AS5610/PPC recipe (newnos/scripts/build-quagga.sh).
# Quagga 1.2.4 (not FRR): modern FRR's libyang/cmake make a static cross-build
# painful; Quagga gives working OSPF/BGP with a clean autoconf cross-build.
#
# Source: the git tree at dl/quagga-git (cloned from github.com/Quagga/quagga
# tag quagga-1.2.4 — savannah's release tarball is unreachable here). The git
# tree has no pre-generated ./configure, so we run ./bootstrap.sh in-container
# (the builder image already has autoconf/automake/libtool/gawk — no network).
set -e
IMG="${BUILDER_IMAGE:-edgenos/builder9:1.8-rootless}"
: "${DOCKER_HOST:=unix:///run/user/$(id -u)/docker.sock}"; export DOCKER_HOST
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -d "$HERE/dl/quagga-git" ] || {
    echo "missing dl/quagga-git — clone it first:" >&2
    echo "  git clone --depth 1 --branch quagga-1.2.4 https://github.com/Quagga/quagga.git $HERE/dl/quagga-git" >&2
    exit 1; }

docker run --rm -u root:0 -v "$HERE:/out" --entrypoint /bin/bash "$IMG" -c '
set -e
rm -rf /tmp/quagga && cp -r /out/dl/quagga-git /tmp/quagga && cd /tmp/quagga
# generate ./configure + Makefile.in (git tree lacks them)
./bootstrap.sh
# crypt() stubs: no static libcrypt for the cross target; crypt() is locally
# declared so cannot be defined-in-place. Both sites are VTY password hashing
# (unused here) — stub them out.
sed -i "s|strcmp (crypt(buf, passwd), passwd)|strcmp (buf, passwd)|" lib/vty.c
sed -i "s|return crypt (passwd, salt);|(void)salt; return (char *)passwd;|" lib/command.c
# -fcommon: GCC10+ -fno-common breaks prefix.h __packed multiple-definition.
./configure \
    --host=arm-linux-gnueabihf --build=x86_64-pc-linux-gnu CC=arm-linux-gnueabihf-gcc \
    --disable-vtysh --disable-doc \
    --enable-ospfd --enable-bgpd \
    --disable-ripd --disable-ripngd --disable-ospf6d \
    --disable-isisd --disable-pimd --disable-nhrpd --disable-babeld \
    --enable-user=root --enable-group=root --enable-vty-group=root \
    --enable-static --disable-shared \
    LDFLAGS="-static" CFLAGS="-O2 -fcommon" ac_cv_lib_readline_main=no
make -j"$(nproc)"
for b in zebra/zebra ospfd/ospfd bgpd/bgpd; do
    arm-linux-gnueabihf-strip "$b" -o /out/$(basename "$b")-arm
done
echo "==> built:"; ls -l /out/zebra-arm /out/ospfd-arm /out/bgpd-arm
file /out/zebra-arm
'
echo "==> Quagga armhf binaries at routing/{zebra,ospfd,bgpd}-arm"
