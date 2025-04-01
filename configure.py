import os
import sys

sys.argv.pop(0)

os.system(f'cmake --preset debug { " ".join(sys.argv) } -DENABLE_CLANGTIDY=FALSE -DENABLE_CPPCHECK=FALSE')

