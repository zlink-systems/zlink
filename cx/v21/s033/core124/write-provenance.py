from pathlib import Path
import hashlib, json
J=Path('/root/jobs/fx/cx/v21/s033/core124')
for version, revision in (('1.23','79567d379162e67476ba13e18cfe0f69925e63c9'),('1.24','0005a4d6c07160bbf661a6f7aea3d8093880514e')):
    prefix=J/f'prefix-{version}'
    runtime=prefix/'lib/libzlink.so.1.23.0'
    digest=hashlib.sha256(runtime.read_bytes()).hexdigest()
    manifest={'schema':1,'package':'zlink-core','version':'1.23.0','abiMajor':0,
      'runtime':{'path':'lib/libzlink.so.1.23.0','sha256':digest,'soname':'libzlink.so.0'},
      'source':{'revision':revision,'dirty':False},
      'release':{'tag':f'local/core-{version}','checksumsSha256':None},'files':[]}
    dest=prefix/'share/zlink/core-package-provenance.json';dest.parent.mkdir(parents=True,exist_ok=True)
    dest.write_text(json.dumps(manifest,indent=2)+'\n')
    print(f'{dest}\t{digest}')
