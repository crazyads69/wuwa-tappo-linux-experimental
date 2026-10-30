import sys,struct
d=open(sys.argv[1],'rb').read()
pe=struct.unpack_from('<I',d,0x3c)[0]
ns=struct.unpack_from('<H',d,pe+6)[0]; opt=struct.unpack_from('<H',d,pe+20)[0]
base=struct.unpack_from('<Q',d,pe+24+24)[0]
st=pe+24+opt
secs=[]
for i in range(ns):
    o=st+i*40
    n=d[o:o+8].rstrip(b'\0'); vs,va,rs,ro,ch=struct.unpack_from('<IIII',d,o+8)[0:4],0,0,0,0
    vs,va,rs,ro=struct.unpack_from('<IIII',d,o+8); ch=struct.unpack_from('<I',d,o+36)[0]
    secs.append((n,vs,va,rs,ro,ch))
targets=[int(x,16) for x in sys.argv[2:]]
for n,vs,va,rs,ro,ch in secs:
    if not ch&0x20000000: continue
    sec=d[ro:ro+rs]
    for t in targets:
        for k in range(len(sec)-4):
            disp=struct.unpack_from('<i',sec,k)[0]
            a=base+va+k+4+disp
            if a==t:
                print(hex(t),'refd at',hex(base+va+k-3),'(disp at',hex(base+va+k),')')
