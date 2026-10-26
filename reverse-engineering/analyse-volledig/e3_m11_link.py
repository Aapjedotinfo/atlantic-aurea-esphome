import sys; sys.path.insert(0,'.')
import harness as H, emu
def fresh(t1_frame=None, replies=None, secs=6.0, pre=None, hook=None, low_after=40.0):
    c=H.mk("M1.1")
    rep = replies or (lambda f: {2:H.reply91(2,H.temp_payload(35.0,30.0,10.0),14),1:H.reply91(1,[0,0,0,0,1,0,0,0,0],14),3:H.reply91(3,[0]*9+[20,5],21),0:H.reply91(0,[0]*9,13)}.get(f[1]))
    od=H.Outdoor(c,rep,low_after_ms=low_after)
    if pre: pre(c)
    if t1_frame:
        # T1.1 stuurt elke 1 s
        for k in range(int(secs)):
            H.send_uart9600(c,int((0.2+k)*H.F),t1_frame)
    n=int(secs/0.05)
    for i in range(n):
        c.run(int((i+1)*0.05*H.F)); od.poll()
        if hook: hook(c,(i+1)*0.05)
    return c,od
def ram(c,*a): return [c.mem[x] for x in a]
if __name__=="__main__":
    fr=H.link_frame([45,20,0,0,0, 0,0,0,0,0,0])
    c,od=fresh(fr,secs=8)
    print("RAM 6e,6f,70,71,72,73,74,65,64,66:",[hex(x) for x in ram(c,0x6e,0x6f,0x70,0x71,0x72,0x73,0x74,0x65,0x64,0x66)])
    print("PORTB=%02x r23=%02x r24=%02x"%(c.io[emu.PORTB],c.r[23],c.r[24]))
    print("UART tx naar T1.1:",[(round(a/H.F,3),"%02x"%b) for a,b in c.uart_tx_log][:36])
    fr6=H.tx666_frames(c)
    print([(round(a/8e6,2),"".join("%02x"%x for x in f[3:6])) for a,f,ok in fr6 if f[1]==2][:8])
