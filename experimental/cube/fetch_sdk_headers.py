"""Read selected official ZIP entries over HTTP ranges; no full SDK checkout."""
import io,json,urllib.request,zipfile,hashlib,ssl
from pathlib import Path
URL='https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v2.0.0/FidelityFX-SDK-v2.0.0.zip'
SIZE=130172282
class Remote(io.RawIOBase):
    def __init__(self): self.pos=0; self.received=0
    def seekable(self): return True
    def readable(self): return True
    def tell(self): return self.pos
    def seek(self,offset,whence=0):
        self.pos=offset if whence==0 else self.pos+offset if whence==1 else SIZE+offset
        return self.pos
    def read(self,n=-1):
        n=SIZE-self.pos if n<0 else min(n,SIZE-self.pos)
        if n<=0:return b''
        if n>24*1024*1024:raise RuntimeError('single range budget')
        request=urllib.request.Request(URL,headers={'Range':f'bytes={self.pos}-{self.pos+n-1}'})
        with urllib.request.urlopen(request,timeout=30,context=ssl.create_default_context(cafile="/etc/ssl/cert.pem")) as response:
            if response.status!=206:raise RuntimeError('server ignored range')
            expected=f'bytes {self.pos}-{self.pos+n-1}/{SIZE}'
            if response.headers.get('Content-Range')!=expected:raise RuntimeError('unexpected range')
            data=response.read(n+1)
        if len(data)!=n:raise RuntimeError('range size')
        self.received+=n
        if self.received>40*1024*1024:raise RuntimeError('total transfer budget')
        self.pos+=n;return data
out=Path(__file__).resolve().parent/'sdk-headers';out.mkdir(exist_ok=False)
remote=Remote();records={}
with zipfile.ZipFile(remote) as archive:
    names=archive.namelist()
    selected=[n for n in names if n.endswith('.h') and (n.startswith('Kits/FidelityFX/framegeneration/include/') or n.startswith('Kits/FidelityFX/api/include/')) or n in ('LICENSE.md','LICENSE','Kits/FidelityFX/LICENSE.md')]
    (out/'available.json').write_text(json.dumps(selected,indent=2))
    for name in selected:
        info=archive.getinfo(name)
        if info.file_size>40*1024*1024:raise RuntimeError('entry size budget')
        path=out/name
        if not path.resolve().is_relative_to(out.resolve()):raise RuntimeError("unsafe archive path")
        path.parent.mkdir(parents=True,exist_ok=True)
        data=archive.read(name);path.write_bytes(data)
        records[name]={'size':len(data),'sha256':hashlib.sha256(data).hexdigest()}
(out/'provenance.json').write_text(json.dumps({'url':URL,'zip_size':SIZE,'transferred':remote.received,'files':records},indent=2))
print(json.dumps({'transferred':remote.received,'files':records},indent=2))
