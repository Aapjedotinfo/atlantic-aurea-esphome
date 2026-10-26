import sys,time; sys.path.insert(0,'.')
import emu
def mk(chip="M1.1"):
    f="../Haddon %s - ATMEGA8@DIP28.BIN"%chip
    return emu.CPU(open(f,"rb").read()[:8192],chip)
def edges(c,io,bit,start=0):
    out=[];last=None
    # reconstruct pin level over time from portlog of port 'io'
    lvl=None
    for cy,a,v in c.portlog:
        if a!=io: continue
        b=(v>>bit)&1
        if b!=lvl:
            out.append((cy,b)); lvl=b
    return out
if __name__=="__main__":
    c=mk(); c.run(int(8e6*float(sys.argv[1]) if len(sys.argv)>1 else 24e6))
    e=edges(c,emu.PORTC,3)
    print("PORTC.3 flanken:",len(e))
    prev=None
    for cy,b in e[:60]:
        print(cy, b, "" if prev is None else "dt=%.2f bit"%((cy-prev)/12000)); prev=cy
    print("PORTB.3 flanken:",len(edges(c,emu.PORTB,3)))
