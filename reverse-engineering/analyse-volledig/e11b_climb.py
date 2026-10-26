"""Opklimmen tot de maximale stand: doel 55 C, aanvoer 20 C, versnelde stapklok. Controleert dat [0x6E] op 40 stopt (index 10)
en dat alle 11 ID2-varianten B[0..10] op de lijn verschijnen (mits de stand ze doorloopt) en nooit een andere waarde."""
import sys; sys.path.insert(0,'.')
import harness as H, emu, tables, analyze as Z
from e8_m11_e2e import rep_factory
c=H.mk("M1.1"); c.trace_pcs=True; H.set_pin(c,emu.PINC,0,0)
od=H.Outdoor(c,rep_factory(10.0,flow=20.0,ret=18.0))
fr=H.link_frame([55,20,0,0,0,0,0,0,0,0,0])
for k in range(100): H.send_uart9600(c,int((0.2+k)*H.F),fr)
seen=[]; 
for i in range(int(100/0.05)):
    t=(i+1)*0.05
    c.run(int(t*H.F)); od.poll()
    if i%20==0: c.mem[0x64]=0xfa
    if not seen or seen[-1]!=c.mem[0x6e]: seen.append(c.mem[0x6e])
frs=H.tx666_frames(c)
variants=sorted({tuple(f[3:5]) for a,f,ok in frs if f[1]==2})
allowed=set()
for name,a,n in tables.SLOTS:
    if name.startswith("B["):
        by,ok,_=tables.decode_slot(a,n); allowed.add(tuple(by[:8]))
bad=[(round(a/H.F,1),f) for a,f,ok in frs if f[1]==2 and (not ok or tuple(f) not in allowed)]
lp=sorted(c.lpm_log)
_f,_=Z.read_flash('M1.1'); _w=Z.words_of(_f); LIVE=Z.trace(_w,Z.seeds_for(_w))[0]
out=["[0x6E]-verloop (opeenvolgende waarden): %s"%seen,
"maximale [0x6E] = %d (0x%02x) ; stand op de lijn = %d"%(max(seen),max(seen),max(seen)>>2),
"ID2-varianten (modus,stand) op de lijn: %s"%variants,
"ID2-telegrammen die niet exact een tabelvariant zijn: %s"%(bad[:5] or "geen"),
"LPM-adressen: 0x%04x .. 0x%04x ; buiten 0x0bbe-0x0c4f: %s"%(lp[0],lp[-1],[hex(z) for z in lp if not 0xbbe<=z<=0xc4f] or "geen"),
"uitgevoerde PC's buiten live-code: %s"%([hex(p*2) for p in c.exec_pcs if p>=19 and p not in LIVE] or "geen")]
open("e11b_climb.txt","w",encoding="utf-8").write("\n".join(out)+"\n"); print("\n".join(out))
