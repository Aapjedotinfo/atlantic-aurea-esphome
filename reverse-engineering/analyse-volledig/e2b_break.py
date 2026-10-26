import sys; sys.path.insert(0,'.')
import harness as H, emu
def trial(low_ms, label, nb=14):
    c=H.mk("M1.1")
    H.set_pin(c,emu.PINC,4,1)
    fr=H.reply91(2,H.temp_payload(35.0,30.0,10.0),msglen=14)[:nb]
    t=H.send_bytes_8n1(c,emu.PINC,4,int(0.46*H.F),fr,gap_bits=0)
    if low_ms:
        H.sched_pin(c,t,emu.PINC,4,0); H.sched_pin(c,t+int(low_ms*8000),emu.PINC,4,1)
    c.run(int(4*H.F))
    ram=lambda a:c.mem[a]
    print("%-28s [71]=%02x [72]=%02x [73]=%02x [66]=%02x r17=%02x frames_tx=%d uart=%d"%(label,ram(0x71),ram(0x72),ram(0x73),ram(0x66),c.r[17],len(H.tx666_frames(c)),len(c.uart_tx_log)))
for ms in (0,10,31,33,40,80): trial(ms,"laag %d ms na frame"%ms)
