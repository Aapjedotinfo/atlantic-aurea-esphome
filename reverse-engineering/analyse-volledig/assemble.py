#!/usr/bin/env python3
"""Voegt deel0..deel9 samen tot RAPPORT.md."""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
parts = ["deel0_inleiding", "deel1_dekking", "deel2_functies", "deel3_protocol", "deel4_wees", "deel5_b202",
         "deel6_dip", "deel7_emulatie", "deel8_open", "deel9_bestanden"]
txt = "\n\n".join(open(os.path.join(HERE, p + ".md"), encoding="utf-8").read().strip() for p in parts) + "\n"
open(os.path.join(HERE, "RAPPORT.md"), "w", encoding="utf-8").write(txt)
print("RAPPORT.md:", len(txt), "tekens,", txt.count("\n"), "regels")
