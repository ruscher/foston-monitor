import sys, struct, re
from capstone import *
from mod import module, Mem
from exps import exports
name=sys.argv[1]
img,e32,secs=module(name); vb=e32[3]; m=Mem(secs)
# IAT names
iat={}
imp=vb+e32[13]; i=0
cache={}
while True:
    ilt,ts,fw,nm,iatr=[m.u32(imp+20*i+4*k) for k in range(5)]
    if not nm: break
    dll=m.cstr(vb+nm)
    if dll not in cache: cache[dll]=exports(dll)
    # IAT address in RAM: data section mapping
    j=0
    while True:
        v=m.u32(vb+ilt+4*j)
        if not v: break
        o=v&0xffff
        # find section containing rva iatr
        for s in secs:
            if s['rva']<=iatr+4*j<s['rva']+len(s['blob']):
                addr=s['realaddr']+iatr+4*j-s['rva']
        iat[addr]=dll.split('.')[0]+'!'+str(cache[dll].get(o,o))
        j+=1
    i+=1
code=secs[0]
md=Cs(CS_ARCH_ARM, CS_MODE_ARM); md.detail=False
out=[]
def strat(v):
    s,o=m.find(v)
    if s is None: return None
    b=s['blob']
    if o+4<len(b) and 32<=b[o]<127 and b[o+1]==0 and 32<=b[o+2]<127 and b[o+3]==0:
        w=m.cstr(v,True)
        if len(w)>=3: return 'L"%s"'%w
    if o+4<len(b) and all(32<=c<127 for c in b[o:o+4]):
        a=m.cstr(v)
        if a and len(a)>=4: return '"%s"'%a
    return None
blob=code['blob']; base=code['realaddr']
for off in range(0,len(blob),4):
    va=base+off
    w=struct.unpack_from('<I',blob,off)[0]
    ins=list(md.disasm(blob[off:off+4],va))
    if not ins:
        out.append('%08x: %08x  .word'%(va,w)); continue
    I=ins[0]; s='%08x: %08x  %-8s %s'%(va,w,I.mnemonic,I.op_str)
    mm=re.search(r'\[pc, #(-?0x[0-9a-f]+|-?\d+)\]',I.op_str)
    if I.mnemonic.startswith('ldr') and mm:
        lit=va+8+int(mm.group(1),0)
        v=m.u32(lit)
        if v is not None:
            note='=0x%08x'%v
            if v in iat: note+=' &'+iat[v]
            st=strat(v)
            if st: note+=' '+st
            s+='   ; '+note
    out.append(s)
open(sys.argv[2],'w').write('\n'.join(out)+'\n')
open(sys.argv[2]+'.iat','w').write('\n'.join('%08x %s'%(k,v) for k,v in sorted(iat.items())))
