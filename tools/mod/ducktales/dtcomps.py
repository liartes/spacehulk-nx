# Paths come from the environment (analysis of your own APKs, unpacked):
#   DT_DATA   folder with dcr/ and sea/ (each: the APK's assets/bin/Data files)
#   DT_MAP_DCR, DT_MAP_SEA   each APK's assets/AssetBundles/guids-to-asset-mapping.txt
import UnityPy, pickle, sys, json, os
sys.path.insert(0,'.')
from ser import *
env=UnityPy.load(os.path.join(os.environ['DT_DATA'],'dcr','sharedassets0.assets'))
objs=list(env.files.values())[0].objects
rows=pickle.load(open('mb_dcr.pkl','rb'))
mb={r[1]:r for r in rows if r[0]=='sharedassets0.assets'}
def gon(pid):
    try: return objs[pid].read().m_Name
    except: return '?'
def pp(r):
    fid,pid=r.pptr()
    if pid==0: return None
    if fid!=0: return f'ext{fid}:{pid}'
    o=objs.get(pid)
    if o is None: return f'?{pid}'
    if o.type.name=='MonoBehaviour': x=mb.get(pid); return f'{x[2]}@{gon(x[5])}'
    try: return f'{o.type.name}:{o.read().m_Name}'
    except: return f'{o.type.name}#{pid}'
P={
 'FloatMovement': lambda r: dict(floatSpeed=r.f32(),floatHeightPercentage=r.f32(),groundOffset=r.f32(),keepHop=r.bool(),additional=r.arr(lambda: pp(r)),turnOffFloat=r.bool()),
 'EffectsWhenBlocked': lambda r: dict(blocksUntilActivate=r.i32(),resetOnActivate=r.bool(),effectParticles=pp(r),animator=pp(r),soundWhenBlocked=r.str(),character=pp(r)),
 'BlockingObjectPrefabSwap': lambda r: dict(newBlockingPrefabs=r.arr(r.str),randomRotation=r.bool(),chanceToSwap=r.f32()),
 'CouncilWomanJumbaSpawner': lambda r: dict(prisonCellChunkName=r.str(),min=r.i32(),max=r.i32()),
 'CollectCharactersUnlock': lambda r: dict(unlockLocKey=r.str(),characterToUnlockId=r.str(),characterIdsToCollect=r.arr(r.str),characterToUnlock=pp(r)),
 'MiscObjectSpawner': lambda r: dict(objs=r.arr(lambda: dict(pool=r.str(),chance=r.f32(),positions=r.arr(r.i32),isBlocking=r.bool(),afterMsg=r.bool(),msg=r.str(),canSpawn=r.bool()))),
 'MeshDetach': lambda r: dict(qanimator=pp(r),detachables=r.arr(lambda: pp(r)),detachAnimation=r.str()),
 'DetachAndBounce': lambda r: dict(audioKey=r.str(),useOverrideRotation=r.bool(),overrideRotation=r.vec3()),
 'OverrideDefaultLighting': lambda r: dict(lightValues=r.arr(lambda: dict(t=r.i32(),c=r.color(),i=r.f32(),ss=r.f32(),ob=r.bool(),sb=r.f32(),orot=r.bool(),e=r.vec3())),overrideAmbient=r.bool(),ambient=r.color()),
 'AlwaysFaceCamera': lambda r: dict(playerMeshTransform=pp(r)),
 'SpawnChunkForCharacter': lambda r: dict(chunks=r.arr(lambda: dict(chunkName=r.str(),rarity=r.i32(),score=r.i32()))),
 'PlayRandomSoundAfterHops': lambda r: dict(min=r.i32(),max=r.i32(),soundKey=r.str()),
 'ParticleEmitOnHop': lambda r: dict(particles=pp(r),lo=r.i32(),hi=r.i32(),dlo=r.i32(),dhi=r.i32(),emitSoundKey=r.str(),player=pp(r)),
 'DoKillInstantiate': lambda r: dict(prefabObject=pp(r),parent=r.bool()),
 'DarkwingDuckDisappear': lambda r: dict(qAnimator=pp(r),animationName=r.str(),soundEffect=pp(r),soundKey=r.str(),particles=pp(r),minHops=r.i32(),maxHops=r.i32(),actionDelay=r.f32(),myMeshRenderer=pp(r),materialForHiding=pp(r),timeToHide=r.f32(),pauseBeforeAnimation=r.f32()),
 'AttackSecurityGuards': lambda r: dict(qAnimator=pp(r),attackPoint=pp(r),anim=r.str(),damagedMeshPoolObject=r.str(),effectParticles=pp(r),soundClipKey=r.str(),objectsToNotAttack=r.arr(r.str)),
 'GizmoDuckFloat': lambda r: dict(floatSpeed=r.f32(),floatHeightPercentage=r.f32(),groundOffset=r.f32(),keepHop=r.bool(),blade=pp(r),floatAnimation=r.str(),turnOffFloat=r.bool()),
 'DucktalesWebbyAcrobat': lambda r: dict(spinTarget=pp(r),flipTarget=pp(r)),
 'SinkingPlayer': lambda r: dict(effect=pp(r)),
 'MagickaShadow': lambda r: dict(),
 'DucktalesLaunchpadPlaneCrash': lambda r: dict(),
}
seen=set()
for pid,r in mb.items():
    go=gon(r[5])
    # climb to root name
    if r[2] in P:
        # find root: check transform father chain
        g=objs[r[5]].read()
        t=None
        for c in g.m_Components:
            o=objs.get(c.path_id)
            if o and o.type.name=='Transform': t=o.read()
        root=go
        while t and t.m_Father.path_id:
            t=objs[t.m_Father.path_id].read(); root=gon(t.m_GameObject.path_id)
        if not root.startswith('Ducktales') and r[2] not in ('SinkingPlayer',): continue
        if r[2]=='SinkingPlayer' and root in seen: continue
        seen.add(root) if r[2]=='SinkingPlayer' else None
        rr=R(objs[pid].get_raw_data()); rr.mbhead()
        try: d=P[r[2]](rr); end=(rr.o, r[6])
        except Exception as e: d=str(e); end=None
        print(f'{root:34s} {r[2]:26s} {d} end={end}')
