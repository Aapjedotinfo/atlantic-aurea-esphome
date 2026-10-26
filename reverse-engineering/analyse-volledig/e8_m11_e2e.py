"""End-to-end scenario's op M1.1 met gesimuleerde T1.1 (9600 baud) en buitenunit (666 baud), plus fuzz-invariantentest."""
import sys, random; sys.path.insert(0,'.')
import harness as H, emu, uartdec as U
from e3_m11_link import fresh, ram
out=[]
def P(*a): 
    s=" ".join(str(x) for x in a); out.append(s); print(s,flush=True)
def link_frames(c):
    """Groepeer UART-tx-bytes (M1.1 -> T1.1) in 12-byte frames."""
    b=[(t,v) for t,v in c.uart_tx_log]
    fr=[]
    for i in range(0,len(b)-11,12):
        f=[v for _,v in b[i:i+12]]
        fr.append((b[i][0],f,(sum(f[:11])&0xFF)==f[11]))
    return fr
def rep_factory(outside, flow=35.0, ret=30.0, fault=0, idx4=1):
    def rep(f):
        i=f[1]
        if i==2: return H.reply91(2,H.temp_payload(flow,ret,outside),14)
        if i==1: return H.reply91(1,[0,0,0,fault,idx4,0,0,0,0],14)
        if i==3: return H.reply91(3,[0]*9+[20,5],21)
        return H.reply91(0,[0]*9,13)
    return rep
def pre0(c): H.set_pin(c,emu.PINC,0,0)
if __name__=="__main__":
    P("== S1/S2: 5-graden-regel end-to-end (T1.1 vraagt 45 C, PC0 laag = vrijgegeven, 14 s)")
    for outside in (4.9, 5.0, 0.0, -0.1, -7.5, 25.0):
        fr=H.link_frame([45,20,0,0,0,0,0,0,0,0,0])
        c,od=fresh(fr,replies=rep_factory(outside),secs=14,pre=pre0)
        lf=link_frames(c)
        last=lf[-1][1] if lf else None
        P("buiten=%5.1f C -> [0x73]=%02x ; PB0(relais)=%d ; [0x65]=%02x ; r23=%02x ; [0x6e]=%02x ; laatste frame naar T1.1: %s"%(outside,c.mem[0x73],c.io[emu.PORTB]&1,c.mem[0x65],c.r[23],c.mem[0x6e],
            " ".join("%02x"%x for x in last) if last else "geen"))
    P("")
    P("== S3: storingscode uit ID1-antwoord (byte 6 van het 0x91-frame) -> [0x74] en PB1")
    for fault in (0,1,0x41,0x46,0x7d):
        fr=H.link_frame([45,20,0,0,0,0,0,0,0,0,0])
        c,od=fresh(fr,replies=rep_factory(25.0,fault=fault),secs=8,pre=pre0)
        lf=link_frames(c)
        P("fault=%02x -> [0x74]=%02x PB1=%d relais=%d [0x6e]=%02x ; laatste frame idx10=%s"%(fault,c.mem[0x74],(c.io[emu.PORTB]>>1)&1,c.io[emu.PORTB]&1,c.mem[0x6e], "%02x"%lf[-1][1][10] if lf else "-"))
    P("")
    P("== S4: normale start; frame M1.1 -> T1.1 (12 bytes) veldwaarden")
    fr=H.link_frame([45,20,0,0,0,0,0,0,0,0,0])
    c,od=fresh(fr,replies=rep_factory(10.0),secs=10,pre=pre0)
    for t,f,ok in link_frames(c)[:3]:
        P("t=%.2fs"%(t/H.F)," ".join("%02x"%x for x in f),"som_ok=",ok)
    frs=H.tx666_frames(c)
    P("666-frames (ID,modus,stand):",[(f[1],"%02x%02x"%(f[3],f[4])) for a,f,ok in frs if f[1]==2][:6], "alle CRC ok:",all(ok for a,f,ok in frs))
    open("e8_m11_e2e.txt","w",encoding="utf-8").write("\n".join(out)+"\n")
