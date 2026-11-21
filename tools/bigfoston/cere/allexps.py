import sys, struct
from mod import module, Mem
img,e32,secs=module(sys.argv[1]); vb=e32[3]; m=Mem(secs)
ed=vb+e32[11]
f=[m.u32(ed+4*k) for k in range(10)]
base,nfun,nnames,afun,anames,aords=f[4],f[5],f[6],f[7],f[8],f[9]
print('name', m.cstr(vb+f[3]), 'nfun',nfun,'nnames',nnames)
for k in range(nnames):
    nm=m.cstr(vb+m.u32(vb+anames+4*k))
    s,off=m.find(vb+aords+2*k); o=struct.unpack_from('<H',s['blob'],off)[0]
    print(o+base, nm, hex(vb+m.u32(vb+afun+4*o)))
