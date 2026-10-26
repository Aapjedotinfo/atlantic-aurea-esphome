#!/bin/sh
# Bouwt de kerntests met avr-gcc (atmega328p) en draait ze in de AVR-simulator van avr-gdb.
# De teststapel staat op 0x20FF (de tests zelf hebben meer RAM nodig dan de 2 kB van de chip; de echte firmware: zie README).
# Gebruik: sh bouw_test_avr.sh [uitmap]
A="/c/Program Files (x86)/Arduino/hardware/tools/avr/bin"
D="$(cd "$(dirname "$0")" && pwd)"
O="${1:-/tmp/avrtest}"; mkdir -p "$O"
"$A/avr-gcc.exe" -mmcu=atmega328p -DF_CPU=8000000UL -Os -g -std=gnu11 -Wall -Wl,--defsym=__stack=0x20FF -o "$O/t.elf" \
  "$D/test_kern.c" "$D/sim.c" "$D/../proto.c" "$D/../tabel.c" "$D/../brug.c" || exit 1
"$A/avr-size.exe" "$O/t.elf"
cat > "$O/g.txt" <<'GDB'
target sim
load
break test_end
run
printf "geslaagd=%d mislukt=%d\n", t_pass, t_fail
printf "falende regels: "
print/d fail_lines
GDB
"$A/avr-gdb.exe" --batch -x "$O/g.txt" "$O/t.elf"
