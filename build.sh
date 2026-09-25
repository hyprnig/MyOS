#!/bin/bash
# Сборка MyOS (UEFI x86_64) из исходников.
# Нужны только gcc + binutils (ld), которые есть почти в любом Linux/WSL.
set -e

gcc -ffreestanding -fno-stack-protector -fno-stack-check -fshort-wchar \
    -mno-red-zone -fpic -maccumulate-outgoing-args -fno-ident \
    -c main.c -o main.o

ld -nostdlib -shared -Bsymbolic \
   -m i386pep --subsystem 10 -e efi_main \
   -o BOOTX64.EFI main.o

mkdir -p esp/EFI/BOOT
cp BOOTX64.EFI esp/EFI/BOOT/BOOTX64.EFI

echo "Готово: BOOTX64.EFI собран и разложен в esp/EFI/BOOT/BOOTX64.EFI"
