"""Controleert dat sub_0308 geen kopbyte (0x91), lengte of CRC valideert: alleen ID (byte 1) en payload tellen."""
import sys; sys.path.insert(0,'.')
import harness as H, emu
def trial(label, mutate, id_=2):
    c=H.mk("M1.1"); H.set_pin(c,emu.PINC,4,1)
    fr=H.reply91(id_,H.temp_payload(35.0,30.0,10.0),14) if id_==2 else H.reply91(1,[0,0,0,0x41,1,0,0,0,0],14)
    fr=mutate(list(fr))
    t=H.send_bytes_8n1(c,emu.PINC,4,int(0.46*H.F),fr,gap_bits=0)
    H.sched_pin(c,t,emu.PINC,4,0); H.sched_pin(c,t+int(40*8000),emu.PINC,4,1)
    c.run(int(2.5*H.F))
    return "%-46s [0x71]=%02x [0x72]=%02x [0x73]=%02x [0x74]=%02x [0x66]=%02x"%(label,c.mem[0x71],c.mem[0x72],c.mem[0x73],c.mem[0x74],c.mem[0x66])
out=[]
out.append(trial("ID2 correct",lambda f:f))
out.append(trial("ID2 kopbyte 0x91 -> 0x00",lambda f:[0]+f[1:]))
out.append(trial("ID2 lengtebyte -> 0x00",lambda f:f[:2]+[0]+f[3:]))
out.append(trial("ID2 CRC-bytes -> 0x00 0x00",lambda f:f[:-2]+[0,0]))
out.append(trial("ID2 payload-byte (byte 4) beschadigd, CRC fout",lambda f:f[:4]+[0x7f]+f[5:]))
out.append(trial("ID1 correct, storingscode 0x41",lambda f:f,id_=1))
out.append(trial("ID1 kopbyte 0x00, CRC 0, code 0x41",lambda f:[0]+f[1:-2]+[0,0],id_=1))
out.append(trial("ID=3 met temperaturen (wordt genegeerd)",lambda f:[f[0],3]+f[2:]))
out.append(trial("ID=0x12 (onbekend)",lambda f:[f[0],0x12]+f[2:]))
open("e12_rx_nocheck.txt","w",encoding="utf-8").write("\n".join(out)+"\n"); print("\n".join(out))
