import struct
class R:
    def __init__(s,b,off=0): s.b=b; s.o=off
    def al(s): s.o=(s.o+3)&~3
    def i32(s): v=struct.unpack_from('<i',s.b,s.o)[0]; s.o+=4; return v
    def f32(s): v=struct.unpack_from('<f',s.b,s.o)[0]; s.o+=4; return v
    def bool(s): v=s.b[s.o]; s.o+=1; s.al(); return bool(v)
    def u8(s): v=s.b[s.o]; s.o+=1; return v
    def str(s):
        n=s.i32(); v=s.b[s.o:s.o+n].decode('utf8','replace'); s.o+=n; s.al(); return v
    def pptr(s): fid,pid=struct.unpack_from('<iq',s.b,s.o); s.o+=12; return (fid,pid)
    def arr(s,fn):
        n=s.i32(); assert 0<=n<100000,(n,s.o)
        return [fn() for _ in range(n)]
    def vec3(s): return (s.f32(),s.f32(),s.f32())
    def color(s): return (s.f32(),s.f32(),s.f32(),s.f32())
    def mbhead(s):
        go=s.pptr(); en=s.bool(); sc=s.pptr(); nm=s.str(); return dict(go=go,enabled=en,script=sc,name=nm)
def qframe(r): return dict(mesh=r.str(), material=r.pptr())
def qanim(r):
    return dict(name=r.str(), next=r.str(), speed=r.f32(), frames=r.arr(lambda: qframe(r)))
def character(raw, sea):
    r=R(raw); h=r.mbhead(); d={}
    d['id']=r.str(); d['anim']=r.str(); d['selectScreenScale']=r.f32(); d['selectedZoomPercentage']=r.f32()
    d['specialBannerSprite']=r.pptr()
    A=lambda: r.arr(lambda: qanim(r))
    d['selectAnimations']=A(); d['idleAnimationBehaviour']=r.i32(); d['idleAnimations']=A(); d['readyAnimations']=A(); d['jumpAnimations']=A()
    d['altJumpChance']=r.f32(); d['altJumpDoublePlay']=r.bool(); d['altJumpAnimations']=A()
    d['jumpParticleSystem']=r.pptr(); d['jumpParticleOffset']=r.vec3()
    d['deadAnimations']=A(); d['blockedAnimations']=A(); d['additionalAnimations']=A()
    d['switchParticleSystem']=r.pptr(); d['manualIdleAdditionalAnimation']=r.bool()
    d['sort']=r.i32(); d['universe']=r.pptr(); d['prefab']=r.pptr(); d['characterSelectPrefab']=r.pptr(); d['promotionalDisplayPrefab']=r.pptr()
    d['preventUIRotation']=r.bool()
    d['hopAudioKey']=r.str(); d['genericHopsKey']=r.str(); d['audioScreamKey']=r.str(); d['firstHopKey']=r.str()
    d['baseHopPitch']=r.f32(); d['randomPitchHop']=r.bool(); d['overrideMusic']=r.str()
    if sea: d['callDoUpdatePlayerPosition']=r.bool()
    d['allowMeshScaling']=r.bool(); d['eagleKeyOverride']=r.str(); d['worldPieceSwap']=r.arr(r.str)
    d['delayGameOver']=r.f32(); d['hasSpecialCredits']=r.bool(); d['specialCreditsText']=r.str()
    d['particleDeathPrefabs']=r.arr(r.pptr); d['deathParticleColors']=r.arr(r.color); d['useMeshColours']=r.bool()
    d['deathParticleSize']=r.f32(); d['keepCharacterModel']=r.bool(); d['immuneFromDeathTypes']=r.arr(r.i32)
    d['isUsingSingleChunkFolderIndex']=r.bool(); d['overrideSingleChunkFolderIndex']=r.i32()
    d['_end']=(r.o,len(raw)); d['_name']=h['name']
    return d
def universe(raw):
    r=R(raw); h=r.mbhead(); d={'_name':h['name']}
    d['Id']=r.i32(); d['NameLocalisationId']=r.str(); d['isHidden']=r.bool(); d['audioKey']=r.i32(); d['SortOrder']=r.i32()
    d['CardLogoSprite']=r.pptr(); d['CardLogoX']=r.f32(); d['CardLogoY']=r.f32(); d['CardBackgroundSprite']=r.pptr(); d['CardBackgroundSpriteColor']=r.color()
    d['ShowCardLabel']=r.bool(); d['BackgroundSprite']=r.pptr(); d['BackgroundColor']=r.color(); d['EventLiveBackgroundColor']=r.color()
    d['GoodScoreThreshold']=r.i32(); d['IsTrainSingleCarriage']=r.bool(); d['IsRiverAPit']=r.bool(); d['EaglePoolKey']=r.str()
    d['DoRoadsHavePlacedObjects']=r.bool(); d['PooledObjectToPlace']=r.str(); d['IsLeftForestAlwaysAlt']=r.bool(); d['stopWorldPieceSwitchAnimations']=r.bool()
    d['AreRoadsAlternatingStrips']=r.bool(); d['WorldMusic']=r.str(); d['TopScoreLabelColor']=r.color(); d['BoulderPositions']=r.arr(r.i32)
    d['BoulderPrefab']=r.str(); d['BoulderMoveSpeed']=r.f32(); d['BoulderFrequencyEasy']=r.f32(); d['BoulderFrequencyHard']=r.f32(); d['BoulderScoreHard']=r.f32()
    return d, r
def universe_full(raw):
    d,r=universe(raw)
    def lv(): return dict(lightType=r.i32(),color=r.color(),intensity=r.f32(),shadowStrength=r.f32(),doOverrideShadowBias=r.bool(),shadowBias=r.f32(),doOverrideRotation=r.bool(),euler=r.vec3())
    d['LightValueOverrides']=r.arr(lv)
    d['CharacterDeathMaterialSwaps']=r.arr(lambda: dict(deathTypes=r.arr(r.i32),mat=r.pptr()))
    d['canSpawnTrapsAsLogs']=r.bool(); d['TrapLogPrefab']=r.str(); d['TrapLogChance']=r.f32()
    d['BirdMinimumHeightOverride']=r.f32(); d['BirdMaximumHeightOverride']=r.f32(); d['DisallowedDeaths']=r.arr(r.i32)
    d['LilypadHeight']=r.f32(); d['LilypadCoinOffset']=r.f32(); d['TraintrackHeight']=r.f32(); d['RoadHeight']=r.f32()
    d['HasHighWalledEdges']=r.bool(); d['HasLightningGameplay']=r.bool(); d['ChanceToSpawnBlockingObject']=r.f32(); d['HasSearchlightGameplay']=r.bool()
    d['SpotlightDisallowedCharacterIDs']=r.arr(r.str); d['firstHopKey']=r.str(); d['SoundOfRiversEnabled']=r.bool()
    d['ExcludeGenericChunkFolder']=r.bool(); d['StartWithRandomChunkFolder']=r.bool()
    d['ChunkFoldersAndHops']=r.arr(lambda: dict(ChunkFolder=r.str(),min=r.i32(),max=r.i32(),HasBoulders=r.bool()))
    d['CameraClearColor']=r.color(); d['CameraNearClippingDistance']=r.f32(); d['DefaultWorldPieceSwap']=r.arr(r.str); d['DefaultPlayerController']=r.pptr()
    d['_end']=(r.o,len(raw))
    return d
def chunk(raw):
    r=R(raw); h=r.mbhead(); d={'_name':h['name']}
    d['Name']=r.str(); d['Folder']=r.str(); d['Type']=r.i32(); d['Rarity']=r.i32(); d['RarityToUse']=r.i32()
    d['Lanes']=r.arr(r.str); d['Events']=r.arr(lambda:(r.i32(),r.i32())); d['_end']=(r.o,len(raw)); return d
def worldpiece(raw):
    r=R(raw); h=r.mbhead(); d={'_name':h['name']}
    d['PieceType']=r.i32(); d['w']=r.f32()
    def var():
        v={}
        v['Name']=r.str(); v['w']=r.f32(); v['score']=r.i32(); v['restricted']=r.arr(r.str); v['disallowed']=r.arr(r.str); v['hitbox']=r.vec3()
        v['LogWidth']=r.i32(); v['LogCoinHeight']=r.f32(); v['carEngine']=r.str(); v['carHorn']=r.str(); v['charger']=r.str(); v['train']=r.str()
        v['UseParticlesForTrainWarning']=r.bool(); v['SignalParticlePrefab']=r.str(); v['useAltEagleSound']=r.bool(); v['eaglePitch']=r.f32(); v['logSplash']=r.str()
        v['ExtraSetupFunctions']=r.arr(r.str); v['MonoBehavioursToAdd']=r.arr(r.pptr); v['IsUniqueVehicle']=r.bool(); v['VehicleForceSameRotation']=r.bool(); v['AllowCarSideDeath']=r.bool(); v['OverrideAnimationNext']=r.bool()
        v['Animation']=qanim(r); v['AnimationSwitch']=qanim(r)
        v['SpawnRapids']=r.bool(); v['SpawnBirds']=r.bool(); v['RandomRotation']=r.bool(); v['MinSpeed']=r.f32(); v['MaxSpeed']=r.f32(); v['OnKillPrefab']=r.str()
        v['TrackWithHeightOffset']=r.bool(); v['useSwitchAnimationAtSameTime']=r.bool(); v['ForestXPositionOffset']=r.f32(); v['IsTallForest']=r.bool()
        v['UseMainOverrideColour']=r.bool(); v['MainOverrideColour']=r.color(); v['UseSwitchOverrideColour']=r.bool(); v['SwitchOverrideColour']=r.color()
        v['vehicleMovementStyle']=r.i32(); v['vehicleHopTime']=r.f32(); v['lean']=r.f32(); v['bounce']=r.f32()
        return v
    d['Variations']=r.arr(var); d['_end']=(r.o,len(raw)); return d
