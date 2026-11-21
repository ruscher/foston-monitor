import sys,re
L=open(sys.argv[1]).read().split('\n')
th={}
for i,l in enumerate(L):
    m=re.search(r'&(\S+)',l)
    if m and 'ldr      ip' in l:
        th[int(l[:8],16)]=m.group(1).split('!')[1]
out=[]
for l in L:
    m=re.search(r'\b(bl|b|blx)(\w*)\s+#0x([0-9a-f]+)',l)
    if m:
        t=int(m.group(3),16)
        if t in th: l+='   ; -> '+th[t]
    out.append(l)
open(sys.argv[2],'w').write('\n'.join(out))
