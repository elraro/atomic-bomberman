import re,collections,sys
drops=[];expl=[];first=None
for l in open(sys.argv[1],errors='replace'):
    m=re.match(r'\s*([0-9.]+):[0-9a-f]+:trace:file:CreateFile[AW] L"([^"]*)"',l)
    if not m: continue
    t=float(m.group(1)); f=re.split(r'[\\/]+',m.group(2).lower())[-1]
    if f.startswith('field'): first=t; drops=[]; expl=[]   # keep the last round only
    if f.startswith('bmdrop'): drops.append(t)
    elif re.match(r'bomb_\d+b?\.rss',f): expl.append(t)
print('round start %.3f  drops %d  explosions %d  span %.1f s'%(first,len(drops),len(expl),max(expl+drops)-first))
print('first drop %.3f s after field load; first explosion %.3f s after first drop'%(drops[0]-first, expl[0]-drops[0]))
h=collections.Counter()
for d in drops:
    for e in expl:
        if 1.0<=e-d<=3.0: h[round((e-d)*20)/20]+=1
print('pair deltas (50 ms bins):',' '.join('%.2f:%d'%(k,h[k]) for k in sorted(h)))
errs=[min(expl,key=lambda e:abs(e-d-2.0))-d-2.0 for d in drops]
good=sorted(e for e in errs if abs(e)<0.12)
print('drops with an explosion within 120 ms of +2.000 s: %d of %d'%(len(good),len(drops)))
if good: print('offset from 2000 ms: min %+.0f  median %+.0f  max %+.0f'%(good[0]*1000,good[len(good)//2]*1000,good[-1]*1000))
