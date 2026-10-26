"""Fuzz-invarianten op M1.1: willekeurige T1.1-frames, buitenunit-antwoorden (ook met fout kop/CRC), schakelaars en versnelde klokken.
Controleert na elke 50 ms: [0x6e] in 0..40; alle LPM-adressen in 0x0bbe..0x0c4f; uitgevoerde PC's alleen op gedecodeerde (live) instructies;
alle 666-baud telegrammen hebben CRC 0 en zijn gelijk aan een van de 14 tabelwaarden. Uitvoer: e11_fuzz_<seed>.txt"""
import sys, random; sys.path.insert(0,'.')
import harness as H, emu, uartdec as U, tables, analyze as Z
seed=int(sys.argv[1]); secs=int(sys.argv[2]) if len(sys.argv)>2 else 90
rnd=random.Random(seed)
flash,_=Z.read_flash("M1.1"); words=Z.words_of(flash)
live,_=Z.trace(words,Z.seeds_for(words))
allowed=set()
for name,a,n in tables.SLOTS:
    if name=="pad" or name.startswith("orphan"): continue
    by,ok,_=tables.decode_slot(a,n); ln=by[2]; allowed.add(tuple(by[:ln]))
c=H.mk("M1.1"); c.trace_pcs=True
H.set_pin(c,emu.PINC,0,0)
params={'out':10.0,'flow':35.0,'ret':30.0,'fault':0,'corrupt':0}
def rep(f):
    i=f[1]; p=params
    if i==2: fr=H.reply91(2,H.temp_payload(p['flow'],p['ret'],p['out']),14)
    elif i==1: fr=H.reply91(1,[0,0,0,p['fault'],1,0,0,0,0],14)
    elif i==3: fr=H.reply91(3,[0]*9+[20,5],21)
    else: fr=H.reply91(0,[0]*9,13)
    if p['corrupt']:
        fr=list(fr)
        k=rnd.randrange(3)
        if k==0: fr[0]=rnd.randrange(256)
        elif k==1: fr[-1]^=0xFF
        else: fr[rnd.randrange(3,len(fr))]=rnd.randrange(256)
    return fr
od=H.Outdoor(c,rep)
viol=[]; maxst=0; sts=set(); pcs_bad=set(); lpm_seen=set()
nxt_t1=0.3
for i in range(int(secs/0.05)):
    t=(i+1)*0.05
    c.run(int(t*H.F)); od.poll()
    if t>=nxt_t1:
        b0=rnd.choice([0,rnd.randrange(256),rnd.randrange(20,70),rnd.randrange(34,56)])
        b1=rnd.choice([0,rnd.randrange(1,40)])
        fr=H.link_frame([b0,b1,rnd.randrange(256),rnd.randrange(256),rnd.randrange(256)]+[rnd.randrange(256) for _ in range(6)])
        H.send_uart9600(c,c.cyc+2000,fr); nxt_t1=t+rnd.uniform(0.5,3.0)
    if i%40==0:
        params.update(out=rnd.uniform(-30,40),flow=rnd.uniform(-5,75),ret=rnd.uniform(-5,75),fault=rnd.choice([0,0,0,1,0x41,0x46,0x7d,rnd.randrange(256)]),corrupt=rnd.choice([0,0,0,1]))
        H.set_pin(c,emu.PINC,0,rnd.choice([0,0,0,1])); H.set_pin(c,emu.PINC,1,rnd.choice([1,1,1,0]))
    if i%20==0: c.mem[0x63]=58            # versnelde minuutklok
    if i%60==0: c.mem[0x64]=rnd.choice([0xfa,0x1e,0x1e])   # stappen versnellen
    sts.add(c.mem[0x6e]); maxst=max(maxst,c.mem[0x6e])
    if not (c.mem[0x6e]==0 or 4<=c.mem[0x6e]<=40): viol.append("t=%.2f [0x6e]=%d"%(t,c.mem[0x6e]))
bad_pc=sorted(a for a in c.exec_pcs if a not in live and a>=19)
fr=H.tx666_frames(c)
badfr=[(round(a/H.F,2),f) for a,f,ok in fr if (not ok) or tuple(f) not in allowed]
lpmz=sorted(c.lpm_log)
res=["seed=%d simulatietijd=%d s ; [0x6e] waarden gezien: %s ; max=%d"%(seed,secs,sorted(sts),maxst),
 "schendingen [0x6e]-bereik: %s"%(viol[:5] or "geen"),
 "uitgevoerde PC's buiten live-code: %s (aantal verschillende PC's uitgevoerd: %d van %d live)"%([hex(a*2) for a in bad_pc] or "geen",len(c.exec_pcs),len(live)),
 "LPM-adressen: min 0x%04x max 0x%04x aantal %d ; buiten 0x0bbe..0x0c4f: %s"%(lpmz[0],lpmz[-1],len(lpmz),[hex(z) for z in lpmz if not 0xbbe<=z<=0xc4f] or "geen"),
 "666-baud telegrammen: %d ; afwijkend/CRC fout: %s"%(len(fr),badfr[:3] or "geen"),
 "PORTB-flanken PB0 (relais): %d ; UART-frames naar T1.1: %d bytes"%(sum(1 for cy,a,v in c.portlog if a==emu.PORTB),len(c.uart_tx_log))]
open("e11_fuzz_%d.txt"%seed,"w",encoding="utf-8").write("\n".join(res)+"\n")
print("\n".join(res))
