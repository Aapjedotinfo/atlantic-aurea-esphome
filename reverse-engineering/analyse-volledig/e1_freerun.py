import sys; sys.path.insert(0,'.')
import harness as H, emu
secs=int(sys.argv[1]) if len(sys.argv)>1 else 40
c=H.mk("M1.1"); c.log_isr=False
ram=lambda a:c.mem[a]
print("t[s]  pc    r17 r18 r20 r22 r23 r24 r25 | 66  6c  6d  6e  6a  6f  70  71 72 73 74 65 64")
for s in range(secs):
    c.run((s+1)*H.F)
    print("%4d %04x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x | PORTB=%02x PORTC=%02x"%(
        s+1,c.pc*2,c.r[17],c.r[18],c.r[20],c.r[22],c.r[23],c.r[24],c.r[25],ram(0x66),ram(0x6c),ram(0x6d),ram(0x6e),ram(0x6a),ram(0x6f),ram(0x70),ram(0x71),ram(0x72),ram(0x73),ram(0x74),ram(0x65),ram(0x64),c.io[emu.PORTB],c.io[emu.PORTC]))
fr=H.tx666_frames(c)
print("frames op 666 baud:",len(fr))
for st,f,ok in fr[:40]:
    print("t=%.4fs  %s  crc=%s"%(st/H.F," ".join("%02x"%x for x in f),"OK" if ok else "FOUT"))
print("UART-tx naar T1.1:",[(round(a/H.F,3),hex(b)) for a,b in c.uart_tx_log][:40])
