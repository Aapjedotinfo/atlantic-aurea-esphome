# Tabellen in M1.1 (0x0BAA-0x0C4F), gedecodeerd door tables.py

Elk 10-bit-gekaderd slot -> bytes; crc16 = CRC-16/CCITT-FALSE over alle telegrambytes incl. de 2 CRC-bytes (0 = geldig).

```
orphan1  0x0baa raw=98 90 21 08 02 ff 80 27 9a 4f framing_ok=True bytes=19 02 08 00 ff 00 9e c9 crc16=0000 staartbits=1
orphan2  0x0bb4 raw=98 b0 21 08 02 00 80 2e cb 9b framing_ok=True bytes=19 03 08 00 00 00 37 67 crc16=0000 staartbits=1
A=ID1    0x0bbe raw=98 a0 23 08 02 00 80 20 08 02 00 80 25 5a b3 framing_ok=True bytes=19 01 0c 00 00 00 00 00 00 00 aa 35 crc16=0000 staartbits=1
pad      0x0bcd raw=00
C=ID3    0x0bce raw=98 b0 21 09 36 40 80 28 39 67 framing_ok=True bytes=19 03 08 b2 02 00 c1 9a crc16=0000 staartbits=1
D=ID0    0x0bd8 raw=98 80 21 08 02 00 80 29 ba b7 framing_ok=True bytes=19 00 08 00 00 00 d9 b5 crc16=0000 staartbits=1
B[0]     0x0be2 raw=98 90 21 08 02 00 80 2b 99 b3 framing_ok=True bytes=19 02 08 00 00 00 9d 36 crc16=0000 staartbits=1
B[1]     0x0bec raw=98 90 21 0a 02 80 80 29 9b b3 framing_ok=True bytes=19 02 08 01 01 00 99 37 crc16=0000 staartbits=1
B[2]     0x0bf6 raw=98 90 21 0a 02 40 80 23 38 9b framing_ok=True bytes=19 02 08 01 02 00 cc 64 crc16=0000 staartbits=1
B[3]     0x0c00 raw=98 90 21 0a 02 c0 80 2f fa ab framing_ok=True bytes=19 02 08 01 03 00 ff 55 crc16=0000 staartbits=1
B[4]     0x0c0a raw=98 90 21 0a 02 20 80 26 69 0f framing_ok=True bytes=19 02 08 01 04 00 66 c2 crc16=0000 staartbits=1
B[5]     0x0c14 raw=98 90 21 0a 02 a0 80 2a ab 3f framing_ok=True bytes=19 02 08 01 05 00 55 f3 crc16=0000 staartbits=1
B[6]     0x0c1e raw=98 90 21 0a 02 60 80 20 08 17 framing_ok=True bytes=19 02 08 01 06 00 00 a0 crc16=0000 staartbits=1
B[7]     0x0c28 raw=98 90 21 0a 02 e0 80 2c ca 27 framing_ok=True bytes=19 02 08 01 07 00 33 91 crc16=0000 staartbits=1
B[8]     0x0c32 raw=98 90 21 0a 02 10 80 2c 4b d7 framing_ok=True bytes=19 02 08 01 08 00 23 af crc16=0000 staartbits=1
B[9]     0x0c3c raw=98 90 21 0a 02 90 80 20 89 e7 framing_ok=True bytes=19 02 08 01 09 00 10 9e crc16=0000 staartbits=1
B[10]    0x0c46 raw=98 90 21 0a 02 50 80 2a 2a cf framing_ok=True bytes=19 02 08 01 0a 00 45 cd crc16=0000 staartbits=1
```
