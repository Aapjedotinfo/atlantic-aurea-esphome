"""Alle instructies die r30/r31 (Z) schrijven, en alle LPM-plaatsen, voor beide chips: statisch bewijs voor punt 4."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import avrlib as A, analyze as Z
out=[]
for chip in ("M1.1","T1.1"):
    flash,_=Z.read_flash(chip); words=Z.words_of(flash)
    live,_=Z.trace(words,Z.seeds_for(words))
    allins=live
    out.append("== %s"%chip)
    zw=[]
    for a,i in sorted(allins.items()):
        n,o=i["name"],i["ops"]; d=o.get("d"); txt=A.mnemonic_text(i,None,a,numeric_io=True)
        wr=False
        if n in ("ldi","mov","add","adc","sub","sbc","subi","sbci","andi","ori","lsr","lsl","ror","asr","swap","inc","dec","com","neg","eor","and","or","pop","in","lds","ld_zp","ld_mz","ld_xp","ld_yp","ldd_y","ldd_z","ld_x","ld_my","ld_mx","mul") and d in (30,31): wr=True
        if n in ("movw","adiw","sbiw") and d in ((30,) if n!="movw" else (30,)): wr=True
        if n.startswith("ld_") and n in("ld_zp","ld_mz"): wr=True   # wijzigt Z via post-inc/pre-dec
        if n in ("st_zp","st_mz","lpm_zp"): wr=True
        if wr: zw.append((a*2,txt))
    ldi_const=[]
    pend={}
    for a,i in sorted(allins.items()):
        n,o=i["name"],i["ops"]
        if n=="ldi" and o["d"]==30: pend["lo"]=(a,o["K"])
        if n=="ldi" and o["d"]==31 and "lo" in pend and a-pend["lo"][0]<=2:
            ldi_const.append((pend["lo"][0]*2,(o["K"]<<8)|pend["lo"][1])); pend={}
    flashrange=[(a,z) for a,z in ldi_const if z>=0x0100]
    out.append("LDI Z-paren met waarde >= 0x0100 (kandidaat flash-adres): %s"%([("0x%04x->0x%04x"%(a,z)) for a,z in flashrange] or "geen"))
    out.append("Aantal LDI Z-paren met RAM/IO-waarde (<0x100): %d"%(len(ldi_const)-len(flashrange)))
    nonldi=[(a,t) for a,t in zw if not t.startswith("ldi")]
    out.append("Overige schrijvers van r30/r31 (niet-LDI):")
    for a,t in nonldi: out.append("   0x%04x  %s"%(a,t))
    lp=[(a*2,A.mnemonic_text(i,None,a,True)) for a,i in sorted(allins.items()) if i["name"] in ("lpm","lpm_z","lpm_zp","elpm_z","elpm_zp","spm")]
    out.append("LPM/SPM-plaatsen: %s"%(lp or "geen"))
    ind=[(a*2,A.mnemonic_text(i,None,a,True)) for a,i in sorted(allins.items()) if i["name"] in ("ijmp","icall","jmp","call")]
    out.append("IJMP/ICALL/JMP/CALL: %s"%(ind or "geen"))
    out.append("")
open(os.path.join(HERE,"z_patterns.txt"),"w",encoding="utf-8").write("\n".join(out)+"\n")
print("\n".join(out))
