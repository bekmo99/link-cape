"""Measures the Hylian-armor Link model (a Collada .dae) and writes src/body_data.hpp.

Usage: python3 tools/gen_body_data.py path/to/al.dae src/body_data.hpp

Outputs ~180 collision spheres on the model's back, each stored relative to a joint frame, and the
cape's rigid rows stored relative to the chest and waist. Needs numpy. See src/cape_actor.cpp.
"""
import sys
import re, numpy as np
import xml.etree.ElementTree as ET
s=open(sys.argv[1]).read()
s=re.sub(r'\sxmlns="[^"]+"','',s,count=1)
r=ET.fromstring(s)
# joints
joints=[]; 
def walk(n,parent,M):
    for c in n.findall('node'):
        isj=c.get('type')=='JOINT'
        m=c.find('matrix')
        L=np.array([float(x) for x in m.text.split()]).reshape(4,4) if m is not None else np.eye(4)
        W=M@L
        if isj:
            joints.append(dict(name=c.get('name') or c.get('id'),sid=c.get('sid'),id=c.get('id'),parent=parent,L=L,W=W))
            walk(c,len(joints)-1,W)
        else:
            walk(c,parent,W)
walk(r.find('.//visual_scene'),-1,np.eye(4))
meshes={}
for g in r.findall('.//geometry'):
    gid=g.get('id'); mesh=g.find('mesh')
    srcs={}
    for src in mesh.findall('source'):
        fa=src.find('float_array')
        if fa is not None: srcs[src.get('id')]=np.array(fa.text.split(),float)
    verts=mesh.find('vertices'); pos_src=None
    for inp in verts.findall('input'):
        if inp.get('semantic')=='POSITION': pos_src=inp.get('source')[1:]
    P=srcs[pos_src].reshape(-1,3)
    tris=[]
    for t in mesh.findall('triangles'):
        ni=len(t.findall('input')); idx=np.array(t.find('p').text.split(),int).reshape(-1,ni)
        off=[int(i.get('offset')) for i in t.findall('input') if i.get('semantic')=='VERTEX'][0]
        tris.append(idx[:,off].reshape(-1,3))
    meshes[gid]=dict(P=P,T=np.vstack(tris) if tris else np.zeros((0,3),int),mat=[t.get('material') for t in mesh.findall('triangles')])
skins={}
for c in r.findall('.//controller'):
    sk=c.find('skin'); gid=sk.get('source')[1:]
    bsm=np.array(sk.find('bind_shape_matrix').text.split(),float).reshape(4,4)
    srcs={}
    for src in sk.findall('source'):
        na=src.find('Name_array') ; fa=src.find('float_array')
        if na is not None: srcs[src.get('id')]=na.text.split()
        elif fa is not None: srcs[src.get('id')]=np.array(fa.text.split(),float)
    ji=[i for i in sk.find('joints').findall('input')]
    jn=srcs[[i.get('source')[1:] for i in ji if i.get('semantic')=='JOINT'][0]]
    ibm=srcs[[i.get('source')[1:] for i in ji if i.get('semantic')=='INV_BIND_MATRIX'][0]].reshape(-1,4,4)
    vw=sk.find('vertex_weights')
    inputs={i.get('semantic'):(int(i.get('offset')),srcs[i.get('source')[1:]]) for i in vw.findall('input')}
    vc=np.array(vw.find('vcount').text.split(),int); v=np.array(vw.find('v').text.split(),int)
    nin=len(inputs); pos=0; W=[]
    for n in vc:
        ws=[]
        for k in range(n):
            j=v[pos+k*nin+inputs['JOINT'][0]]; w=inputs['WEIGHT'][1][v[pos+k*nin+inputs['WEIGHT'][0]]]
            ws.append((jn[j],w))
        pos+=n*nin; W.append(ws)
    skins[gid]=dict(bsm=bsm,jn=jn,ibm=ibm,W=W)

import sys
names=[j['name'] for j in joints]; jid={n:i for i,n in enumerate(names)}
Pb=np.array([j['W'][:3,3] for j in joints])

def norm(v): 
    n=np.linalg.norm(v); return v/n if n>1e-9 else v
def ortho(a,y):
    a=a-y*np.dot(a,y); return norm(a)
def frames(P):
    hip=norm(P[17]-P[22]); chest=norm(P[6]-P[11]); F={}
    def mk(Y,side):
        Y=norm(Y); X=ortho(side,Y); Z=np.cross(X,Y); return (X,Y,Z)
    F['chest']=mk(P[3]-P[2],chest)
    F['spine']=mk(P[2]-P[1],chest)
    F['waist']=mk(P[2]-P[16],hip)
    F['thighL']=mk(P[18]-P[19],hip); F['thighR']=mk(P[23]-P[24],hip)
    F['shinL']=mk(P[19]-P[20],hip);  F['shinR']=mk(P[24]-P[25],hip)
    F['skirtL']=mk(P[31]-P[32],hip); F['skirtR']=mk(P[33]-P[34],hip)
    return F
GROUP={}
for n in range(35): GROUP[n]='waist'
for n in (1,): GROUP[n]='spine'
for n in (2,3,4,5,6,7,8,9,10,11,12,13,14,15): GROUP[n]='chest'
for n in (18,): GROUP[n]='thighL'
for n in (23,): GROUP[n]='thighR'
for n in (19,20,21): GROUP[n]='shinL'
for n in (24,25,26): GROUP[n]='shinR'
for n in (31,32): GROUP[n]='skirtL'
for n in (33,34): GROUP[n]='skirtR'
GROUPS=['chest','spine','waist','thighL','thighR','shinL','shinR','skirtL','skirtR']
Fb=frames(Pb)
def to_local(j,p,F=Fb,P=Pb):
    X,Y,Z=F[GROUP[j]]; d=p-P[j]; return np.array([d@X,d@Y,d@Z])
def to_world(j,l,F,P):
    X,Y,Z=F[GROUP[j]]; return P[j]+X*l[0]+Y*l[1]+Z*l[2]

# --- vertices with dominant joint
inc=[2,3,5,6,7,9,10,11,15]   # body (no eri/collar, arms, ears)
V=[];DJ=[]
for i in inc:
    gid='meshId%d'%i; P=meshes[gid]['P']; sk=skins[gid]; bsm=sk['bsm']
    Pw=(np.c_[P,np.ones(len(P))]@bsm.T)[:,:3]
    for k,p in enumerate(Pw):
        ws=sorted(sk['W'][k],key=lambda t:-t[1]); V.append(p); DJ.append(jid[ws[0][0]])
V=np.array(V);DJ=np.array(DJ)
CHEST_J=set(range(2,16))
def grp_w(ws):
    g=np.zeros(3)  # chest, spine, waist
    for n,w in ws:
        j=jid[n]
        g[0 if j in CHEST_J else (1 if j==1 else 2)]+=w
    return g/max(g.sum(),1e-9)
VW=[]
for i in inc:
    gid='meshId%d'%i; sk=skins[gid]
    for k in range(len(meshes[gid]['P'])): VW.append(grp_w(sk['W'][k]))
VW=np.array(VW)
print('verts',len(V), 'bsm0',np.round(skins['meshId3']['bsm'],2).tolist()[0])
raw=np.vstack([meshes['meshId3']['P']]); print('raw vs world range', raw.min(0), V.min(0))

def zsurf(x,y,rad=3.5,need_back=True):
    m=(np.abs(V[:,0]-x)<rad)&(np.abs(V[:,1]-y)<rad)
    if not m.any(): return None,None
    idx=np.where(m)[0]; k=idx[np.argmin(V[idx,2])]
    return V[k,2],DJ[k]

R_SPH=6.0
sph=[]   # (joint, local xyz, radius)
for y in np.arange(4,111,5.0):
    for x in np.arange(-24,24.1,5.0):
        z,j=zsurf(x,y)
        if z is None or z>-4: continue
        c=np.array([x,y,z+R_SPH])
        sph.append((int(j),to_local(int(j),c),R_SPH))
print('spheres',len(sph))

# --- rigid cape rows (bind space)
kCols=13; kRigid=4
Y_TOP=131.5; Y_HIP=103.0
HALFW=18.0
def hw(y): return HALFW
rigid=[]  # per row, per col: (lChest,lWaist)
pts=[]
for r in range(kRigid+1):
    y=Y_TOP+(Y_HIP-Y_TOP)*r/kRigid
    for c in range(kCols):
        u=1-2*c/(kCols-1); x=u*hw(y)
        # surface from upbody only above waist belt; smooth: min over a few x
        zs=[]
        for sx in (x,-x):
            for dx in (-3,0,3):
                z,_=zsurf(sx+dx,y,3.0)
                if z is not None: zs.append(z)
        z=min(zs) if zs else -10.0
        z=max(-13.0,min(-3.0,z))
        # the top rows sink into the body so a little drift in motion stays hidden under the mesh
        off=[+3.0,+1.5,0.0,-0.8,-0.8][r]
        p=np.array([x,y,z+off])
        pts.append(p)
        # skin weights: average of the nearest body vertices' weights (chest / spine / waist)
        d=np.linalg.norm(V-p,axis=1); nn=np.argsort(d)[:8]
        wgt=VW[nn].mean(axis=0); wgt=wgt/wgt.sum()
        rigid.append((to_local(2,p),to_local(1,p),to_local(16,p),wgt))
pts=np.array(pts)

out=[]
out.append("// Generated by tools/gen_body_data.py from the Hylian armor model (al.dae). Do not edit by hand.\n#pragma once\n\nnamespace lnkcape {\n")
out.append("// Frame groups: 0 chest, 1 spine, 2 waist, 3 thighL, 4 thighR, 5 shinL, 6 shinR, 7 skirtL, 8 skirtR\n")
out.append("struct BodySphere { int joint; int group; float l[3]; float r; };\n")
out.append("constexpr int kBodySphereCount = %d;\nconstexpr BodySphere kBodySpheres[kBodySphereCount] = {\n"%len(sph))
for j,l,r in sph:
    out.append("    {%d, %d, {%.3ff, %.3ff, %.3ff}, %.2ff},\n"%(j,GROUPS.index(GROUP[j]),l[0],l[1],l[2],r))
out.append("};\n\n")
out.append("// Rigid cape rows (top = row 0, hip = last): each point stored relative to the chest and the waist.\n")
out.append("struct RigidPoint { float chest[3]; float spine[3]; float waist[3]; float w[3]; };\n")
out.append("constexpr int kRigidPointCount = %d;\nconstexpr RigidPoint kRigidPoints[kRigidPointCount] = {\n"%len(rigid))
for lc,ls,lw,wg in rigid:
    out.append("    {{%.3ff, %.3ff, %.3ff}, {%.3ff, %.3ff, %.3ff}, {%.3ff, %.3ff, %.3ff}, {%.3ff, %.3ff, %.3ff}},\n"%(*lc,*ls,*lw,*wg))
out.append("};\n")
out.append("\n}  // namespace lnkcape\n")
open(sys.argv[2],'w').write("".join(out))
print(len(sph),len(rigid),ws)
