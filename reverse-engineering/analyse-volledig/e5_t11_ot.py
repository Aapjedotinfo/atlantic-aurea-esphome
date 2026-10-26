import sys; sys.path.insert(0,'.')
import harness as H, emu, ot
PD3,PD4=3,4
class Boiler:
    """Simpele OpenTherm-ketel op PD3 (ontvangt van T1.1) / PD5 (zendt naar T1.1, lijn geinverteerd: rust laag)."""
    def __init__(self,c,reply_fn,delay_ms=15):
        self.c,self.fn,self.d=c,reply_fn,delay_ms*8000; self.seen=0; self.log=[]
        self.st=ot.Stream(emu.PORTD,PD3)
        H.set_pin(c,emu.PIND,5,0)
    def poll(self):
        for t,v,ok in self.st.poll(self.c):
            r=self.fn(v) if ok else None
            self.log.append((t,v,ok,r))
            if r is not None:
                e,te=ot.edges(r,max(self.c.cyc+1,t+34*ot.BITC+self.d),idle=0)
                for cy,lv in e: H.sched_pin(self.c,cy,emu.PIND,5,lv)
def default_boiler(v):
    did=(v>>16)&0xFF; mt=(v>>28)&7
    if mt in (0,):   # READ-DATA
        data={0:0x0200,25:0x3200,28:0x2800,18:0x0180,19:0x0000,26:0x0000,27:0x1400,17:0x0000,5:0x0000,3:0x0100,2:0x0000}.get(did,0)
        return ot.frame(4,did,data)
    if mt==1: return ot.frame(5,did,v&0xFFFF)
    return ot.frame(7,did,0)
def run(reqs,secs,dip2=0,boiler_fn=default_boiler,link_in=None,pre=None,verbose=True):
    c=H.mk("T1.1")
    line=ot.Line(1)
    c.aco_fn=lambda cy: line.get(cy)
    if dip2: H.set_pin(c,emu.PINC,2,1)
    else: H.set_pin(c,emu.PINC,2,0)
    b=Boiler(c,boiler_fn) if boiler_fn else None
    for t,v in reqs:
        e,_=ot.edges(v,int(t*H.F)); line.add(e)
    if pre: pre(c)
    if link_in:
        for t,data in link_in: H.send_uart9600(c,int(t*H.F),data)
    n=int(secs/0.01)
    for i in range(n):
        c.run(int((i+1)*0.01*H.F))
        if b: b.poll()
    return c,b,line
def report(c,b,reqs):
    to_boiler=ot.decode(c.portlog,emu.PORTD,3); to_th=ot.decode(c.portlog,emu.PORTD,4)
    print("T1.1 -> ketel (PD3):",[(round(t/8e6,3),ot.pretty(v),ok) for t,v,ok in to_boiler])
    print("ketel -> T1.1 (PD5):",[(round(t/8e6,3),ot.pretty(v) if v else None) for t,_,ok,v in (b.log if b else [])])
    print("T1.1 -> thermostaat (PD4):",[(round(t/8e6,3),ot.pretty(v),ok) for t,v,ok in to_th])
if __name__=="__main__":
    reqs=[(0.20,ot.frame(0,0,0x0100)),(0.50,ot.frame(1,1,0x2D00)),(0.80,ot.frame(1,16,0x1400)),(1.10,ot.frame(0,25,0)),(1.40,ot.frame(0,28,0)),(1.70,ot.frame(0,5,0)),(2.0,ot.frame(0,3,0))]
    c,b,line=run(reqs,2.6)
    report(c,b,reqs)
    print("UART tx:",[(round(a/8e6,3),'%02x'%x) for a,x in c.uart_tx_log])
    print("RAM 6c..79:",[hex(c.mem[a]) for a in range(0x66,0x7b)], "r22=%02x r24=%02x r25=%02x"%(c.r[22],c.r[24],c.r[25]))
