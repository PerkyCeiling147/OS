#!/bin/sh
#
# qemu-compat.sh - print the QEMU option that hands bin/ucore.img to the
# firmware/kernel.
#
# Usage:
#   sh tools/qemu-compat.sh [image]     -> prints the loader flags on stdout
#   QEMU_LOADER_MODE=kernel|loader sh tools/qemu-compat.sh [image]
#                                       -> force one form (default: auto)
#
# Why this exists
# ---------------
# The lab used to poke the raw image into RAM with
#     -device loader,file=bin/ucore.img,addr=0x80200000
# which only works while OpenSBI jumps to the hard-coded 0x80200000.
#
# Newer QEMU (8.2 packages as well as 9+) builds the fw_dynamic_info block
# with next_addr = 0 when no -kernel is given, and OpenSBI then jumps to
# address 0 instead: the kernel never executes a single instruction.
# Symptoms are the OpenSBI banner printing "Domain0 Next Address : 0x0",
# and `-d int` showing a stream of fault_fetch at epc=0.
#
# Passing the image with -kernel makes QEMU load it at the very same
# 0x80200000 but also hands OpenSBI the right next_addr.
#
# QEMU_LOADER_MODE=auto (the default) therefore uses -kernel for QEMU >= 8,
# whose packages already ship fw_dynamic (Ubuntu's 8.2.2 does), and keeps the
# legacy -device loader form for older toolchains.

qemu=${QEMU:-qemu-system-riscv64}
image=${1:-bin/ucore.img}
mode=${QEMU_LOADER_MODE:-auto}

if [ "$mode" = auto ]; then
    version=$("$qemu" --version 2>/dev/null |
              sed -n '1s/.*version \([0-9][0-9]*\).*/\1/p')
    case "$version" in
        ''|*[!0-9]*) mode=loader ;;   # QEMU missing/unparsable: keep legacy form
        *) if [ "$version" -ge 8 ]; then mode=kernel; else mode=loader; fi ;;
    esac
fi

case "$mode" in
    kernel) printf '%s\n' "-kernel $image" ;;
    loader) printf '%s\n' "-device loader,file=$image,addr=0x80200000" ;;
    *)
        echo "qemu-compat.sh: unknown QEMU_LOADER_MODE=$mode (expected auto, kernel or loader)" >&2
        exit 1
        ;;
esac
