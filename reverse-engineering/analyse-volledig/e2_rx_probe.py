import sys; sys.path.insert(0,'.')
import harness as H, emu
def trial(nbytes, extra_start_ms=None, gap_after_ms=300, secs=3.0, label=""):
    c=H.mk("M1.1")
    H.set_pin(c,emu.PINC,4,1)
    fr=H.reply91(2,H.temp_payload(35.0,30.0,10.0),msglen=14)
    fr=fr[:nbytes]
    t0=int(0.46*H.F)
    t=H.send_bytes_8n1(c,emu.PINC,4,t0,fr,gap_bits=0)
    if extra_start_ms is not None:
        # tweede frame (zelfde) na extra_start_ms
        H.send_bytes_8n1(c,emu.PINC,4,t+int(extra_start_ms*8000),fr,gap_bits=0)
    c.run(int(secs*H.F))
    ram=lambda a:c.mem[a]
    print("%-34s [71]=%02x [72]=%02x [73]=%02x [66]=%02x r17=%02x X=%02x%02x frames_tx=%d"%(label,ram(0x71),ram(0x72),ram(0x73),ram(0x66),c.r[17],c.r[27],c.r[26],len(H.tx666_frames(c))))
trial(14,None,label="14 bytes, geen vervolg")
trial(10,None,label="10 bytes, geen vervolg")
trial(11,None,label="11 bytes")
trial(14,300,label="14 B + 2e frame na 300 ms")
trial(14,20,label="14 B + 2e frame na 20 ms")
trial(14,60,label="14 B + 2e frame na 60 ms")
