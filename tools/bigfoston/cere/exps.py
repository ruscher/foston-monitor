import sys, struct
from mod import module, Mem
def exports(name):
    img,e32,secs=module(name); vb=e32[3]; m=Mem(secs)
    # code section realaddr = vb+rva for read-only; export dir in rdata
    ed=vb+e32[11]
    f=[m.u32(ed+4*k) for k in range(10)]
    base,nfun,nnames,afun,anames,aords=f[4],f[5],f[6],f[7],f[8],f[9]
    out={}
    for k in range(nnames):
        nm=m.cstr(vb+m.u32(vb+anames+4*k))
        o=struct.unpack('<H', bytes([0,0]))[0]
        s,off=m.find(vb+aords+2*k)
        o=struct.unpack_from('<H',s['blob'],off)[0]
        out[o+base]=nm
    return out
if __name__=='__main__':
    e=exports(sys.argv[1])
    for o in map(int, sys.argv[2:]): print(o, e.get(o))
