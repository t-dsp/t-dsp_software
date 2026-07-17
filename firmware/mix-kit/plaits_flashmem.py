# plaits_flashmem.py — add the plaits_flashmem.ld INSERT fragment to the link so
# the Plaits engine (libTDspPlaits2.a) runs from FLASH instead of ITCM. See the
# .ld for why. Applied only to the teensy41_plaits env (extra_scripts).
Import("env")
import os

ld = os.path.join(env.subst("$PROJECT_DIR"), "plaits_flashmem.ld").replace("\\", "/")
env.Append(LINKFLAGS=["-Wl,-T," + ld])
