import sys,struct
d=open(sys.argv[1],'rb').read()
pe=struct.unpack_from('<I',d,0x3c)[0]
ns=struct.unpack_from('<H',d,pe+6)[0]; opt=struct.unpack_from('<H',d,pe+20)[0]
base=struct.unpack_from('<Q',d,pe+24+24)[0]; st=pe+24+opt
secs=[]
for i in range(ns):
    o=st+i*40; vs,va,rs,ro=struct.unpack_from('<IIII',d,o+8); secs.append((va,vs,rs,ro))
def rd(a,n):
    for va,vs,rs,ro in secs:
        if base+va<=a<base+va+max(vs,rs): o=ro+a-base-va; return d[o:o+n]
a=int(sys.argv[2],16); n=int(sys.argv[3]); b=rd(a,n)
print(b.hex()); print(b)
