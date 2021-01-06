#!/bin/bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
export PATH=$PATH:$DIR/../../tools/fpga-binutils/mingw32/bin/

if ! type "riscv32-unknown-elf-as" > /dev/null; then
  echo "defaulting to riscv64-linux"
  ARCH="riscv64-linux"
else
  ARCH="riscv32-unknown"
fi

echo "using $ARCH"

$ARCH-elf-gcc -fno-unroll-loops -O0 -fno-pic -march=rv32i -mabi=ilp32 -S $1 -o build/code.s
$ARCH-elf-gcc -fno-unroll-loops -O0 -fno-pic -march=rv32i -mabi=ilp32 -c -o build/code.o $1

$ARCH-elf-as -march=rv32i -mabi=ilp32 -o crt0.o crt0.s

CPU=${2:-0}

$ARCH-elf-ld -m elf32lriscv -b elf32-littleriscv -Tconfig_cpu$CPU.ld --no-relax -o build/code.elf build/code.o

$ARCH-elf-objcopy -O verilog build/code.elf build/code$CPU.hex

# uncomment to see the actual code, usefull for debugging
$ARCH-elf-objcopy.exe -O binary build/code.elf build/code.bin
$ARCH-elf-objdump.exe -D -b binary -m riscv build/code.bin 