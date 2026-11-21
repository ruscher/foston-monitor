import sys, struct
from mod import module, Mem
img,e32,secs=module(sys.argv[1])
vb=e32[3]; m=Mem(secs)
imp=vb+e32[13]
i=0
while True:
    ilt,ts,fw,name,iat=[m.u32(imp+20*i+4*k) for k in range(5)]
    if not name: break
    print('DLL', m.cstr(vb+name), 'ilt %x iat %x'%(vb+ilt if ilt else 0, iat))
    # iat may be rva into data section
    for base in (ilt,):
        j=0
        while True:
            v=m.u32(vb+base+4*j)
            if not v: break
            ent = 'ord %d'%(v&0xffff) if v&0x80000000 else m.cstr(vb+v+2)
            print('   IAT %08x -> %s'%(vb+iat+4*j, ent)); j+=1
    i+=1
