import csv, hashlib, pathlib, subprocess
job0=pathlib.Path('/root/jobs/fx/cx/v21/s033/core124')
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/core124b')
rows=list(csv.DictReader((job0/'sha-ldd.tsv').open(),delimiter='\t'))
out=['version\texecutable\tsha_match\tactual_sha256\trunpath_match\tldd_match\tstatus']
for row in rows:
    v=row['version']; exe=row['executable']
    p=job/f'src-{v}/bindings/c/build-release-1.23.0/perf/{exe}'
    actual=hashlib.sha256(p.read_bytes()).hexdigest()
    rp=subprocess.run(['readelf','-d',str(p)],capture_output=True,text=True,check=True).stdout
    ldd=subprocess.run(['ldd',str(p)],capture_output=True,text=True,check=True).stdout
    if exe=='perf_stream_client':
        rpm='no Core dependency' in row['core_ldd']; lm='no Core dependency' in row['core_ldd']
    else:
        expected='/root/jobs/fx/cx/v21/s033/core124/prefix-'+v+'/lib'
        rpm=expected in rp
        lm=expected+'/libzlink.so.0' in ldd
    sm=actual==row['sha256']
    out.append(f"{v}\t{exe}\t{sm}\t{actual}\t{rpm}\t{lm}\t{'PASS' if sm and rpm and lm else 'FAIL'}")
(job/'harness-verification.tsv').write_text('\n'.join(out)+'\n')
print('\n'.join(out[:1]+out[1:5]))
print(f'rows={len(rows)} failures={sum(x.endswith("FAIL") for x in out[1:])}; full details: harness-verification.tsv')
