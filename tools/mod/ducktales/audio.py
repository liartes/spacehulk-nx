# Paths come from the environment (analysis of your own APKs, unpacked):
#   DT_DATA   folder with dcr/ and sea/ (each: the APK's assets/bin/Data files)
#   DT_MAP_DCR, DT_MAP_SEA   each APK's assets/AssetBundles/guids-to-asset-mapping.txt
import UnityPy, pickle, sys, json, os
sys.path.insert(0,'.')
from ser import R
res={}
for g in ['sea','dcr']:
    D=os.path.join(os.environ['DT_DATA'],g)+'/'
    rows=pickle.load(open(f'mb_{g}.pkl','rb'))
    r=[r for r in rows if r[2]=='AudioController'][0]
    env=UnityPy.load(D+r[0]); f=list(env.files.values())[0]; objs=f.objects
    raw=objs[r[1]].get_raw_data(); rr=R(raw); h=rr.mbhead()
    dbg=rr.bool()
    def ov(): return dict(name=rr.str(), key=rr.i32(), vol=rr.f32(), pitch=rr.f32(), sounds=rr.arr(rr.str))
    def mp(): return dict(name=rr.str(), sounds=rr.arr(rr.str), mixer=rr.pptr(), sorting=rr.i32(), overrides=rr.arr(ov))
    maps=rr.arr(mp); pre=rr.pptr()
    print(g, len(maps), rr.o, len(raw))
    res[g]=maps
json.dump(res,open('audio_maps.json','w'))
M={}
for gg,mp in [('dcr',os.environ['DT_MAP_DCR']),('sea',os.environ['DT_MAP_SEA'])]:
    M[gg]={}
    for l in open(mp):
        p=l.rstrip('\n').split('\t')
        if len(p)==3: M[gg][p[0]]=(p[1],p[2])
sea={m['name']:m for m in res['sea']}
for m in res['dcr']:
    dt=[o for o in m['overrides'] if o['key']==115]
    if 'ucktales' in m['name'] or m['sorting']==115 or dt:
        print('DCR', m['name'], 'sorting',m['sorting'], [M['dcr'].get(s,('?',s)) for s in m['sounds']], 'inSEA=', m['name'] in sea)
        for o in dt: print('     override115', o['name'], o['vol'],o['pitch'], [M['dcr'].get(s,('?',s)) for s in o['sounds']], 'SEA mapping has sounds:', [s in M['sea'] for s in o['sounds']])
print('SEA mappings w/ 115:', [m['name'] for m in res['sea'] if m['sorting']==115 or any(o['key']==115 for o in m['overrides'])])
