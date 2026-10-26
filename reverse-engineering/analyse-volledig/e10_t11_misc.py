"""T1.1: (1) K3/PC5 bij DIP2 hoog, (2) linkframes T1.1->M1.1, (3) link-timeout/noodbrug, (4) LED-knippercodes."""
import sys; sys.path.insert(0,'.')
import harness as H, emu, ot
from e5_t11_ot import Boiler, run, default_boiler
def part1():
    out=[]
    out.append("== T1-D. DIP2 hoog (aan/uit-ketel) + link-frame idx0=0xF0 (r22.0=1): volgt PC5 (K3) de ketelsetpoint?")
    fr=H.link_frame([0xF0,0x14,0,0xb4,0x2d,0x2d,0x00,0x14,0x14,0x14,0x00])
    for sp in (10,19,20,21,45,0):
        reqs=[(0.4,ot.frame(1,1,sp<<8)),(0.8,ot.frame(1,1,sp<<8))]
        c,b,line=run(reqs,1.3,dip2=1,boiler_fn=None,link_in=[(0.05,fr)])
        out.append("  setpoint ID1 = %2d C -> PORTC=%02x  (PC5=%d)  r22=%02x"%(sp,c.io[emu.PORTC],(c.io[emu.PORTC]>>5)&1,c.r[22]))
    # zonder F0: pomp actief
    reqs=[(0.4,ot.frame(1,1,45<<8))]
    c,b,line=run(reqs,1.0,dip2=1,boiler_fn=None)
    out.append("  zonder F0 (r22.0=0), setpoint 45 C -> PC5=%d"%((c.io[emu.PORTC]>>5)&1))
    return out
def part2():
    out=["== T1-L. link-frames T1.1 -> M1.1 (12 s; thermostaat vraagt ID1=45 en ID16=20; M1.1-frame met temperaturen op t=0.1 s)"]
    fr=H.link_frame([0x2d,0x14,0,0xb4,0x2d,0x2d,0x0e,0x23,0x1e,0x0a,0x00])
    reqs=[(0.3,ot.frame(1,1,0x2D00)),(0.6,ot.frame(1,16,0x1400))]+[(1.0+k,ot.frame(0,0,0x0100)) for k in range(12)]
    c,b,line=run(reqs,12.5,dip2=0,link_in=[(0.1,fr)])
    bs=[(t,v) for t,v in c.uart_tx_log]
    for i in range(0,len(bs)-11,12):
        f=[v for _,v in bs[i:i+12]]
        out.append("  t=%.2fs  %s  som_ok=%s"%(bs[i][0]/H.F," ".join("%02x"%x for x in f),(sum(f[:11])&0xFF)==f[11]))
    out.append("  RAM: [0x6d]=%02x [0x71]=%02x [0x72]=%02x [0x73]=%02x [0x6e]=%02x [0x77]=%02x [0x78]=%02x r22=%02x"%(tuple(c.mem[a] for a in (0x6d,0x71,0x72,0x73,0x6e,0x77,0x78))+(c.r[22],)))
    return out
def part3():
    out=["== T1-T. link-timeout: [0x6c] (s sinds laatste geldige M1.1-frame) op 0xee gezet; DIP2 laag/hoog"]
    for dip in (0,1):
        c=H.mk("T1.1"); c.aco_fn=lambda cy:1
        H.set_pin(c,emu.PINC,2,dip); H.set_pin(c,emu.PIND,5,0)
        c.run(20000); c.mem[0x6c]=0xee
        c.run(int(4*H.F))
        out.append("  DIP2=%d: na 4 s [0x6c]=%02x PORTB=%02x (PB2=%d) PORTC=%02x (PC4=%d) r22=%02x [0x73]=%02x uart-bytes=%d"%(dip,c.mem[0x6c],c.io[emu.PORTB],(c.io[emu.PORTB]>>2)&1,c.io[emu.PORTC],(c.io[emu.PORTC]>>4)&1,c.r[22],c.mem[0x73],len(c.uart_tx_log)))
        bs=[v for t,v in c.uart_tx_log][:12]
        out.append("      frame naar M1.1: "+" ".join("%02x"%x for x in bs))
    # herstel: geldig frame na timeout
    c=H.mk("T1.1"); c.aco_fn=lambda cy:1; H.set_pin(c,emu.PINC,2,0); H.set_pin(c,emu.PIND,5,0)
    c.run(20000); c.mem[0x6c]=0xee; c.run(int(3*H.F))
    fr=H.link_frame([0x2d,0x14,0,0xb4,0x2d,0x2d,0x0e,0x23,0x1e,0x0a,0x00]); H.send_uart9600(c,c.cyc+1000,fr); c.run(c.cyc+int(0.3*H.F))
    out.append("  herstel na geldig frame: PB2=%d r22=%02x [0x6c]=%02x"%((c.io[emu.PORTB]>>2)&1,c.r[22],c.mem[0x6c]))
    return out
def led(code):
    c=H.mk("T1.1"); c.aco_fn=lambda cy:1; H.set_pin(c,emu.PINC,2,0); H.set_pin(c,emu.PIND,5,0)
    c.run(20000); c.mem[0x73]=code
    c.run(int(42*H.F))
    ev=[];lvl=0
    for cy,a,v in c.portlog:
        if a==emu.PORTC and ((v>>4)&1)!=lvl: lvl=(v>>4)&1; ev.append((round(cy/H.F,2),lvl))
    return ev
if __name__=="__main__":
    which=sys.argv[1]
    if which=="p1": print("\n".join(part1()))
    elif which=="p2": print("\n".join(part2()))
    elif which=="p3": print("\n".join(part3()))
    else:
        code=int(which,0); print("LED-code %02x:"%code, led(code))
