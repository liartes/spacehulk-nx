# minimal Unity SerializedFile (format 17, no type trees) reader -- prototype of what the port would do at run time
import struct,sys,glob,os
def load_split(dirp,name):
    parts=sorted(glob.glob(os.path.join(dirp,name+'.split*')),key=lambda p:int(p.rsplit('split',1)[1]))
    return b''.join(open(p,'rb').read() for p in parts) if parts else open(os.path.join(dirp,name),'rb').read()
def parse(b):
    msize,fsize,ver,doff=struct.unpack_from('>IIII',b,0)
    endian=b[16]; o=20
    E='<' if endian==0 else '>'
    e=b.index(b'\0',o); uver=b[o:e].decode(); o=e+1
    plat,=struct.unpack_from(E+'i',b,o); o+=4
    tt=b[o]; o+=1
    ntypes,=struct.unpack_from(E+'i',b,o); o+=4
    types=[]
    for i in range(ntypes):
        cid,=struct.unpack_from(E+'i',b,o); o+=4
        stripped=b[o]; o+=1
        sidx,=struct.unpack_from(E+'h',b,o); o+=2
        if cid==114: o+=16
        o+=16
        assert not tt
        types.append((cid,sidx))
    nobj,=struct.unpack_from(E+'i',b,o); o+=4
    objs=[]
    for i in range(nobj):
        o=(o+3)&~3
        pid,bs,bz,tid=struct.unpack_from(E+'qIIi',b,o); o+=20
        objs.append((pid,doff+bs,bz,tid))
    nscr,=struct.unpack_from(E+'i',b,o); o+=4
    scr=[]
    for i in range(nscr):
        fi,=struct.unpack_from(E+'i',b,o); o+=4
        o=(o+3)&~3
        lid,=struct.unpack_from(E+'q',b,o); o+=8
        scr.append((fi,lid))
    next_,=struct.unpack_from(E+'i',b,o); o+=4
    ext=[]
    for i in range(next_):
        e=b.index(b'\0',o); o=e+1   # asset path (empty)
        o+=16; o+=4                  # guid, type
        e=b.index(b'\0',o); ext.append(b[o:e].decode()); o=e+1
    return dict(uver=uver,types=types,objs=objs,scripts=scr,externals=ext)
if __name__=='__main__':
    d=sys.argv[1]  # the world-wide APK's assets/bin/Data, unpacked
    b=load_split(d,'sharedassets0.assets')
    sf=parse(b)
    print(sf['uver'], len(sf['types']), len(sf['objs']), len(sf['scripts']), sf['externals'])
    # type entries whose script is (externals[0]=globalgamemanagers.assets -> fileIndex 1, pathID 1812 Character)
    want={1812:'Character',1090:'Universe',1403:'Chunk'}
    for ti,(cid,sidx) in enumerate(sf['types']):
        if cid==114 and sidx>=0:
            fi,lid=sf['scripts'][sidx]
            if lid in want and fi==1:
                objs=[x for x in sf['objs'] if x[3]==ti]
                print(want[lid],'typeIndex',ti,'objects',len(objs))
