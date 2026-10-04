# Paths come from the environment (analysis of your own APKs, unpacked):
#   DT_DATA   folder with dcr/ and sea/ (each: the APK's assets/bin/Data files)
#   DT_MAP_DCR, DT_MAP_SEA   each APK's assets/AssetBundles/guids-to-asset-mapping.txt
import UnityPy, pickle, sys, json, os
sys.path.insert(0,'.')
from ser import *
g=sys.argv[1]; pat=sys.argv[2]
D=os.path.join(os.environ['DT_DATA'],g)
env=UnityPy.load(D+'/sharedassets0.assets')
f=list(env.files.values())[0]; objs=f.objects
rows=pickle.load(open(f'mb_{g}.pkl','rb'))
byid={(r[0],r[1]):r for r in rows}
M={}
mp=os.environ['DT_MAP_SEA'] if g=='sea' else os.environ['DT_MAP_DCR']
for l in open(mp):
    p=l.rstrip('\n').split('\t')
    if len(p)==3: M[p[0]]=(p[1],p[2])
def nm(pp):
    fid,pid=pp
    if pid==0: return None
    if fid!=0: return f'ext{fid}:{pid}'
    o=objs.get(pid)
    if o is None: return f'?{pid}'
    if o.type.name=='MonoBehaviour':
        r=byid.get(('sharedassets0.assets',pid)); 
        # get GO name
        go=r[5] if r else None
        gon=objs[go].read().m_Name if go and go in objs else ''
        return f'{r[2]}:{r[3] or gon}#{pid}'
    try: return f'{o.type.name}:{o.read().m_Name}#{pid}'
    except Exception as e: return f'{o.type.name}#{pid}'
import re
out={}
for r in rows:
    if r[2]=='Character' and r[0]=='sharedassets0.assets' and re.search(pat,r[3]):
        d=character(objs[r[1]].get_raw_data(), g=='sea')
        for k in list(d):
            if k.endswith('Animations'):
                d[k]=[ (a['name'],a['next'],round(a['speed'],3),[ (M.get(fr['mesh'],('?',fr['mesh']))[1].replace('_optimised.asset',''), M.get(fr['mesh'],('?',))[0], nm(fr['material'])) for fr in a['frames']]) for a in d[k]]
            elif isinstance(d[k],tuple) and len(d[k])==2 and isinstance(d[k][0],int) and k!='_end':
                d[k]=nm(d[k])
        d['particleDeathPrefabs']=[nm(p) for p in d['particleDeathPrefabs']]
        out[r[3]]=d
json.dump(out,open(f'chars_{g}_{re.sub("[^a-z0-9]","",pat.lower())[:20]}.json','w'),indent=1)
for n,d in out.items():
    print('=====',n, d['_end'])
    for k,v in d.items():
        if v in ([],None,'',0,False) : continue
        print('  ',k,'=',v)
