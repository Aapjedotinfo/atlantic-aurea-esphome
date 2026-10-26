"""5-graden-regel end-to-end: (1) koude start, (2) koud worden tijdens bedrijf (met versnelde minuutklok)."""
import sys; sys.path.insert(0,'.')
import harness as H, emu
from e3_m11_link import ram
from e8_m11_e2e import rep_factory, link_frames
out=[]
def P(*a):
    s=" ".join(str(x) for x in a); out.append(s); print(s,flush=True)
def build(outside_fn, t1_start, secs, accel=None, preset=None):
    c=H.mk("M1.1")
    H.set_pin(c,emu.PINC,0,0)
    cur={'out':outside_fn(0)}
    def rep(f):
        t=c.cyc/H.F
        o=outside_fn(t)
        return {2:H.reply91(2,H.temp_payload(35.0,30.0,o),14),1:H.reply91(1,[0]*9,14),3:H.reply91(3,[0]*9+[20,5],21)}.get(f[1],H.reply91(0,[0]*9,13))
    od=H.Outdoor(c,rep)
    fr=H.link_frame([45,20,0,0,0,0,0,0,0,0,0])
    for k in range(int(secs)):
        if k>=t1_start: H.send_uart9600(c,int((0.2+k)*H.F),fr)
    log=[]
    for i in range(int(secs/0.05)):
        c.run(int((i+1)*0.05*H.F)); od.poll()
        t=(i+1)*0.05
        if accel and abs(t-round(t))<1e-9 and int(round(t))%accel[0]==0:   # versnelde minuutklok: [0x63] bijna 59
            c.mem[0x63]=58
        if preset and abs(t-preset[0])<1e-9: 
            for a,v in preset[1].items(): c.mem[a]=v
        if abs(t-round(t))<1e-9 and int(round(t))%2==0: log.append((t,c.io[emu.PORTB]&1,c.mem[0x65],c.mem[0x73],c.r[23],c.mem[0x6e]))
    return c,od,log
P("== K1. koude start: buiten 3,0 C vanaf het begin bekend; T1.1 vraagt pas vanaf t=8 s (45 C, kamer 20)")
c,od,log=build(lambda t:3.0,8,18)
for t,rel,h,o,r23,st in log[::3]: P("t=%4.1f relais=%d [0x65]=%02x [0x73]=%02x r23=%02x [0x6e]=%02x"%(t,rel,h,o,r23,st))
lf=link_frames(c); P("laatste frames naar T1.1:",[" ".join("%02x"%x for x in f) for t,f,ok in lf[-2:]])
P("")
P("== K1b. idem maar warm (12 C): ter vergelijking")
c,od,log=build(lambda t:12.0,8,18)
for t,rel,h,o,r23,st in log[::4]: P("t=%4.1f relais=%d [0x65]=%02x [0x73]=%02x r23=%02x [0x6e]=%02x"%(t,rel,h,o,r23,st))
P("")
P("== K2. bedrijf bij 12 C (t<16 s), daarna buiten 3,0 C; versnelde minuutklok ([0x63]=58 elke 2 s); [0x65] handmatig op 0x64 gezet op t=24 s")
c,od,log=build(lambda t:(12.0 if t<16 else 3.0),0,140,accel=(2,),preset=(24.0,{0x65:0x64}))
for t,rel,h,o,r23,st in log[::4]: P("t=%5.1f relais=%d [0x65]=%02x [0x73]=%02x r23=%02x [0x6e]=%02x"%(t,rel,h,o,r23,st))
lf=link_frames(c); P("laatste frames naar T1.1:",[" ".join("%02x"%x for x in f) for t,f,ok in lf[-2:]])
open("e9_m11_cold.txt","w",encoding="utf-8").write("\n".join(out)+"\n")
