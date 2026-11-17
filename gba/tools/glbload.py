import json,struct,numpy as np
d=open(__import__('os').path.join(__import__('os').path.dirname(__file__),'..','..','models','pieces.glb'),'rb').read()
l=struct.unpack('<I',d[12:16])[0]
j=json.loads(d[20:20+l]); off=20+l; bl=struct.unpack('<I',d[off:off+4])[0]; bin_=d[off+8:off+8+bl]
CT={5126:('f',4),5123:('H',2),5125:('I',4),5121:('B',1)}
NC={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}
def acc(i):
    a=j['accessors'][i]; bv=j['bufferViews'][a['bufferView']]
    t,s=CT[a['componentType']]; n=NC[a['type']]
    o=bv.get('byteOffset',0)+a.get('byteOffset',0)
    arr=np.frombuffer(bin_,dtype='<'+t,count=a['count']*n,offset=o)
    return arr.reshape(a['count'],n) if n>1 else arr
def mesh(mi):
    p=j['meshes'][mi]['primitives'][0]
    return acc(p['attributes']['POSITION']).astype(float), acc(p['indices']).reshape(-1,3)
pieces={}
for n in j['nodes']:
    if 'children' in n and n.get('mesh') is None:
        ch=[j['nodes'][c] for c in n['children']]
        pieces[n['name']]={'model':[c['mesh'] for c in ch if c['name']=='Model'],'col':[c['mesh'] for c in ch if c['name']!='Model'],'node':n,'chn':ch}
