import sys
from mod import module, Mem
from exps import exports
img,e32,secs=module(sys.argv[1]); vb=e32[3]; m=Mem(secs)
imp=vb+e32[13]; i=0
while True:
    ilt,ts,fw,nm,iat=[m.u32(imp+20*i+4*k) for k in range(5)]
    if not nm: break
    dll=m.cstr(vb+nm); ex=exports(dll); j=0
    while True:
        v=m.u32(vb+ilt+4*j)
        if not v: break
        o=v&0xffff
        print('%s\t%d\t%s'%(dll,o,ex.get(o,'?')) ); j+=1
    i+=1
