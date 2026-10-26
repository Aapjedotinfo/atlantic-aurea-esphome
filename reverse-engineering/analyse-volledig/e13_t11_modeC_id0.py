"""T1.1 in DIP2-hoog-modus (aan/uit-ketel): lokale slave-antwoorden op ID0 (status) afhankelijk van setpoint en link-status."""
import sys; sys.path.insert(0,'.')
import harness as H, emu, ot
from e5_t11_ot import run
out=["== T1-C0. DIP2 hoog: antwoord op READ ID0 (master-status 0x0100 = CH aan) afhankelijk van ketelsetpoint ([0x6e]) en pomptoestand"]
for sp in (0,19,20,45):
    for label,fr in (("geen link (r22=0)",None),("link ok, stand>0 (r22.1)",H.link_frame([0x2d,0x14,0,0xb4,0x2d,0x2d,0x0e,0x23,0x1e,0x0a,0x00])),("link F0 (boiler-modus r22.0)",H.link_frame([0xF0,0x14,0,0xb4,0x2d,0x2d,0x00,0x14,0x14,0x14,0x00]))):
        reqs=[(0.3,ot.frame(1,1,sp<<8)),(0.8,ot.frame(0,0,0x0100))]
        c,b,line=run(reqs,1.4,dip2=1,boiler_fn=None,link_in=([(0.05,fr)] if fr else None))
        to_t=ot.decode(c.portlog,emu.PORTD,4)
        r=[ot.pretty(v) for t,v,ok in to_t if t>int(0.75*H.F)]
        out.append("  setpoint %2d, %-30s -> r22=%02x  antwoord op ID0: %s"%(sp,label,c.r[22],r[0] if r else "-"))
open("e13_t11_modeC_id0.txt","w",encoding="utf-8").write("\n".join(out)+"\n"); print("\n".join(out))
