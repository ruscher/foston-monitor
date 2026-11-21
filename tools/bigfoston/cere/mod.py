import sys, struct
import os
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
import cerom
IMG = os.environ.get('BF_NAND_IMG', os.path.join(HERE, '../../../backup/2026-11-19-nand/DSK9-msflash.img'))
def load(offset=0x20000, base=0x8c003000, romhdr=0x8c357738):
    with open(IMG,'rb') as f:
        f.seek(offset); data=f.read(0x1800000)
    img=cerom.Image(data, base)
    return img, romhdr
def module(name, **kw):
  for kw in ([dict(offset=0x20000, base=0x8c003000, romhdr=0x8c357738), dict(offset=0x722000, base=0x8c705000, romhdr=0x8dcb89c0)] if not kw else [kw]):
   img, romhdr = load(**kw)
   if 1:
    h = cerom.ROMHDR.unpack(img.at(romhdr, cerom.ROMHDR.size))
    for i in range(h[4]):
        attrs,_,size,name_va,e32_va,o32_va,load_va = cerom.TOC.unpack(img.at(romhdr+cerom.ROMHDR.size+i*cerom.TOC.size, cerom.TOC.size))
        if img.cstr(name_va).lower()!=name.lower(): continue
        e32 = cerom.E32.unpack(img.at(e32_va, cerom.E32.size))
        secs=[]
        for k in range(e32[0]):
            vsz,rva,psize,dataptr,realaddr,flags = cerom.O32.unpack(img.at(o32_va+k*24,24))
            blob = img.at(dataptr, min(psize, vsz) if vsz else psize) if psize else b''
            blob = blob + b'\0'*(max(vsz,psize)-len(blob))
            secs.append(dict(rva=rva,vsize=vsz,psize=psize,dataptr=dataptr,realaddr=realaddr,flags=flags,blob=blob))
        return img, e32, secs
  raise KeyError(name)
class Mem:
    def __init__(self, secs): self.secs=secs
    def find(self, va):
        for s in self.secs:
            if s['realaddr'] <= va < s['realaddr']+len(s['blob']): return s, va-s['realaddr']
        return None, None
    def u32(self, va):
        s,o=self.find(va)
        if s is None or o+4>len(s['blob']): return None
        return struct.unpack_from('<I', s['blob'], o)[0]
    def cstr(self, va, wide=False):
        s,o=self.find(va)
        if s is None: return None
        b=s['blob']
        if wide:
            out=''
            while o+1<len(b) and (b[o] or b[o+1]): out+=chr(b[o]|b[o+1]<<8); o+=2
            return out
        e=b.index(b'\0',o); return b[o:e].decode('latin1')
if __name__=='__main__':
    img,e32,secs=module(sys.argv[1])
    print('entry rva %x vbase %x'%(e32[2],e32[3]))
    names='EXP IMP RES EXC SEC FIX DEB IMD MSP'.split()
    for j,n in enumerate(names): print(n, hex(e32[11+2*j]), hex(e32[12+2*j]))
    for s in secs: print({k:(hex(v) if isinstance(v,int) else len(v)) for k,v in s.items()})
