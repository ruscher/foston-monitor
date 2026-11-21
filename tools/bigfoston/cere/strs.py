import sys,re
from mod import module
img,e32,secs=module(sys.argv[1])
for s in secs:
    b=s['blob']
    for mm in re.finditer(rb'(?:[\x20-\x7e]\x00){4,}',b):
        print('%08x W %s'%(s['realaddr']+mm.start(), mm.group().decode('utf-16le')))
    for mm in re.finditer(rb'[\x20-\x7e]{6,}',b):
        print('%08x A %s'%(s['realaddr']+mm.start(), mm.group().decode()))
