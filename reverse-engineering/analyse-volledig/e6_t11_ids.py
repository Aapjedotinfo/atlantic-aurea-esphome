"""Sweep: elke OpenTherm Data-ID als READ-DATA en WRITE-DATA door T1.1; wat gaat naar de ketel (PD3) en wat naar de thermostaat (PD4)?
Modi: A = DIP2 laag (OT-ketel), r22.0=0 ; B = DIP2 laag, r22.0=1 (doorgeven na link-fout/F0) ; C = DIP2 hoog (aan/uit-ketel)."""
import sys; sys.path.insert(0,'.')
import harness as H, emu, ot
from e5_t11_ot import Boiler, run
IDS=list(range(0,0x41))+[0x73,0x7f]
def boiler_fn(v):
    did=(v>>16)&0xFF; mt=(v>>28)&7
    return ot.frame(4 if mt==0 else 5,did,0xAB00|did)      # herkenbare data
def sweep(mode, t_each=0.45):
    reqs=[];t=0.25
    for did in IDS:
        for mt in (0,1):
            reqs.append((t,ot.frame(mt,did,0x1234))); t+=t_each
    pre=None
    link_in=None
    if mode=="B":
        # geldig link-frame met byte0=0xF0 (M1.1 'niet beschikbaar') -> r22.0 = 1
        fr=H.link_frame([0xF0,0x14,0,0xb4,0x2d,0x2d,0x00,0x14,0x14,0x14,0x00])
        link_in=[(0.05,fr)]
    c,b,line=run(reqs,t+0.5,dip2=(1 if mode=="C" else 0),boiler_fn=boiler_fn,link_in=link_in)
    to_b=ot.decode(c.portlog,emu.PORTD,3); to_t=ot.decode(c.portlog,emu.PORTD,4)
    rows=[]
    for (tq,rq) in reqs:
        tq_c=int(tq*H.F)
        # eerste ketel-frame binnen 0.4 s na vraag en thermostaat-antwoord
        kb=[x for x in to_b if tq_c<=x[0]<tq_c+int(0.4*H.F)]
        kt=[x for x in to_t if tq_c+int(0.03*H.F)<=x[0]<tq_c+int(0.43*H.F)]
        rows.append((rq,kb[0][1] if kb else None,kt[0][1] if kt else None))
    return rows,c
if __name__=="__main__":
    mode=sys.argv[1]
    rows,c=sweep(mode)
    out=[]
    for rq,kb,kt in rows:
        out.append("%-28s | ketel: %-28s | thermostaat: %s"%(ot.pretty(rq),ot.pretty(kb) if kb is not None else "-",ot.pretty(kt) if kt is not None else "-"))
    open("e6_modus%s.txt"%mode,"w",encoding="utf-8").write("\n".join(out)+"\n")
    print("\n".join(out[:12]))
    print("UART-tx naar M1.1:",[(round(a/8e6,2),'%02x'%b) for a,b in c.uart_tx_log][:30])
    print("r22=%02x PORTB=%02x PORTC=%02x"%(c.r[22],c.io[emu.PORTB],c.io[emu.PORTC]))
