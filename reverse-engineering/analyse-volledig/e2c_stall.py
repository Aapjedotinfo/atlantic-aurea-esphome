import sys; sys.path.insert(0,'.')
import harness as H, emu
c=H.mk("M1.1")
H.set_pin(c,emu.PINC,4,1)
# antwoord op ELK CB-telegram (idle-hoog daarna, geen lange laag)
def rep(f):
    i=f[1]
    if i==2: return H.reply91(2,H.temp_payload(35.0,30.0,10.0),14)
    if i==1: return H.reply91(1,[0,0,0,0,1,0,0,0,0],14)
    if i==3: return H.reply91(3,[0]*9+[20,5],21)
    return H.reply91(0,[0]*9,13)
od=H.Outdoor(c,rep)
ram=lambda a:c.mem[a]
last=0
for s in range(1,71):
    H.run_with(c,1.0,od) if False else None
    for k in range(20):
        c.run(int(((s-1)+ (k+1)/20)*H.F)); od.poll()
    if s%5==0 or s<4: print("t=%2ds [66]=%02x r17=%02x [6d]=%02x [69]=%02x [71]=%02x [74]=%02x frames=%d replies=%d uart=%d PORTB=%02x"%(s,ram(0x66),c.r[17],ram(0x6d),ram(0x69),ram(0x71),ram(0x74),len(H.tx666_frames(c)),len(od.log),len(c.uart_tx_log),c.io[emu.PORTB]))
fr=H.tx666_frames(c)
print([(round(a/8e6,1),f[1]) for a,f,ok in fr])
