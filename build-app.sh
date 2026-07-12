#!/usr/bin/env bash
set -e

MUSL_DIR="musl-1.2.6"
MUSL_INC="$MUSL_DIR/sysroot/usr/local/musl/include"
MUSL_LIB="$MUSL_DIR/lib/libc.a"

LWIP_DIR="lwip-2.2.0"
LWIP_INC="$LWIP_DIR/src/include"
LWIP_PORT="lwip_port"

cd app

if [ ! -f "$MUSL_DIR/config.mak" ]; then
	echo "error: $MUSL_DIR is not configured -- run its ./configure once first (see $MUSL_DIR/config.mak)." >&2
	exit 1
fi

if [ ! -d "$LWIP_DIR" ]; then
	echo "error: $LWIP_DIR is missing -- run scripts/get-lwip.sh from this directory first." >&2
	exit 1
fi

# BareMetal apps are flat binaries loaded at a fixed high-canonical
# address (0xFFFF800000000000, see c.ld), with no syscall trap and no
# dynamic linker. PIC/PIE codegen (GOT-relative loads with nothing to
# resolve them against) and the small code model's 32-bit-relative
# address assumptions both break under that, hence -fno-pic -fno-pie
# -mcmodel=large. See posix_shim.c and crt0.c for the syscall/startup
# side of the port.
CFLAGS="-c -m64 -nostdlib -nostartfiles -nodefaultlibs -ffreestanding -fno-pic -fno-pie -mcmodel=large -falign-functions=16 -fomit-frame-pointer -mno-red-zone -fno-builtin -fno-stack-protector -nostdinc -isystem $MUSL_INC"

# lwIP headers pull in musl's (via -isystem above) for size_t/
# stdint/etc., plus its own lwip/ and netif/ trees, plus our port's
# lwipopts.h and arch/{cc,sys_arch}.h -- see net_glue.c/net_shim.c.
LWIP_CFLAGS="$CFLAGS -I $LWIP_INC -I $LWIP_PORT"

# Build musl's libc.a, and the merged header sysroot posix_shim.c/
# helloc.c compile against.
make -C "$MUSL_DIR" lib/libc.a
make -C "$MUSL_DIR" install-headers DESTDIR="$(pwd)/$MUSL_DIR/sysroot"

gcc $CFLAGS -o crt0.o crt0.c
gcc $CFLAGS -o posix_shim.o posix_shim.c
gcc $CFLAGS -o bmfs.o bmfs.c
gcc $LWIP_CFLAGS -o net_glue.o net_glue.c
gcc $LWIP_CFLAGS -o net_shim.o net_shim.c
gcc $CFLAGS -o libBareMetal.o libBareMetal.c
gcc $CFLAGS -o helloc.o helloc.c
gcc $CFLAGS -o webc.o webc.c

# lwIP core: IPv4 + Ethernet + ARP + DHCP + TCP (+ UDP, which DHCP
# needs internally) only -- no IPv6, no AutoIP/IGMP/raw sockets/ACD
# (see lwip_port/lwipopts.h), so their source files aren't built.
LWIP_SRCS="
	core/def.c core/inet_chksum.c core/init.c core/ip.c core/mem.c
	core/memp.c core/netif.c core/pbuf.c core/stats.c core/sys.c
	core/tcp.c core/tcp_in.c core/tcp_out.c core/timeouts.c core/udp.c
	core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c
	core/ipv4/ip4_addr.c core/ipv4/ip4.c core/ipv4/ip4_frag.c
	netif/ethernet.c
"
LWIP_OBJS=""
for src in $LWIP_SRCS; do
	obj="lwip_$(basename "$src" .c).o"
	gcc $LWIP_CFLAGS -o "$obj" "$LWIP_DIR/src/$src"
	LWIP_OBJS="$LWIP_OBJS $obj"
done

ld -T c.ld -o helloc.app crt0.o posix_shim.o bmfs.o net_glue.o net_shim.o \
	libBareMetal.o helloc.o $LWIP_OBJS "$MUSL_LIB"

ld -T c.ld -o webc.app crt0.o posix_shim.o bmfs.o net_glue.o net_shim.o \
	libBareMetal.o webc.o $LWIP_OBJS "$MUSL_LIB"

cp helloc.app webc.app ../
cd ..
