"""Wat gebeurt er met een lopend ID2-telegram als de firmware zelf [0x6E] op 0 zet (T1.1-frame met kamersetpoint 0) halverwege het telegram?
Referentie: normale ID2 bij stand 14 (index 3) = 19 02 08 01 03 00 FF 55; bij stand 0 = 19 02 08 00 00 00 9D 36."""
import sys; sys.path.insert(0,'.')
import harness as H, emu
out=["== E14. T1.1-frame 'geen vraag' (byte1=0) komt aan op x% van een lopend ID2-telegram (stand 14 -> 0 door link_rx_frame, zonder [0x6D].0-controle)"]
# 1e ID2 start bij 0,3155 + 1,0494 k ; we gebruiken de 2e ID2 (k=1) bij 1,3648 s
for frac in (-0.5,0.05,0.2,0.35,0.5,0.65,0.8,0.95,1.3):
    c=H.mk("M1.1"); H.set_pin(c,emu.PINC,0,0)
    H.send_uart9600(c,int(0.2*H.F),H.link_frame([45,20,0,0,0,0,0,0,0,0,0]))      # start: vraag
    t_id2=1.3648+1.0494            # 3e ID2-telegram start
    t_arrive=t_id2+frac*0.12       # 12e byte komt hier aan
    H.send_uart9600(c,int(t_arrive*H.F)-12*11*H.BIT9600,H.link_frame([45,0,0,0,0,0,0,0,0,0,0]))   # b1=0
    c.run(int(4.0*H.F))
    fr=[(a,f,ok) for a,f,ok in H.tx666_frames(c) if f[1]==2]
    near=[x for x in fr if abs(x[0]/H.F-t_id2)<0.08]
    if near:
        a,f,ok=near[0]; out.append("aankomst op %+4d%% van ID2 -> ID2 op de lijn: %s crc_ok=%s"%(int(frac*100)," ".join("%02x"%x for x in f),ok))
    else:
        out.append("aankomst op %+4d%%: geen ID2 gevonden rond %.2f s"%(int(frac*100),t_id2))
open("e14_mid_change.txt","w",encoding="utf-8").write("\n".join(out)+"\n"); print("\n".join(out))
