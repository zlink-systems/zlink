import pathlib,hashlib,json,subprocess
R=pathlib.Path('/root/worktree/zlink-v21-s033/xbp');J=pathlib.Path('/root/jobs/fx/cx/v21/s033/xbp');G=R/'framework/bench/grpc'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
result={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'core':{},'artifacts':{}}
core=pathlib.Path('/root/worktree/zlink-v21-s033/xbp/.artifacts/wsl/install/zlink-core/1.23.0/lib/libzlink.so.1.23.0');result['core']={str(core):sha(core)}
for p in (G/'cpp/build').glob('bench_cpp_*'):
    if p.is_file() and p.stat().st_mode&0o111:result['artifacts'][str(p)]=sha(p)
for p in (G/'dotnet').glob('*/bin/Release/net8.0/Systems.Zlink.dll'):
    source=R/'bindings/dotnet/src/Zlink/bin/Release/net8.0/Systems.Zlink.dll'
    assert sha(p)==sha(source),(p,source)
    result['artifacts'][str(p)]=sha(p)
for p in (G/'java').glob('*/build/install/*/lib/zlink-1.23.0.jar'):
    source=R/'.artifacts/wsl/maven/systems/zlink/zlink/1.23.0/zlink-1.23.0.jar'
    assert sha(p)==sha(source),(p,source)
    result['artifacts'][str(p)]=sha(p)
cppcache=(G/'cpp/build/CMakeCache.txt').read_text().splitlines();result['cppConfiguration']=[x for x in cppcache if x.startswith(('CMAKE_BUILD_TYPE:','ZLINK_FRAMEWORK_CPP_TEST_PROBES:','ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX:','ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX:'))]
corecache=(R/'.artifacts/wsl/build/core-main/CMakeCache.txt').read_text().splitlines()
result['coreConfiguration']=[x for x in corecache if x.startswith(('CMAKE_BUILD_TYPE:','CMAKE_CXX_FLAGS_RELEASE:','ENABLE_LTO:','WITH_TLS:','BUILD_SHARED:','BUILD_STATIC:'))]
for lang in ('cpp','dotnet','java'):
    p=G/lang
    result['versions_'+lang]={'binding':(R/f'bindings/{lang}/VERSION').read_text().strip(),'framework':(R/f'framework/languages/{lang}/VERSION').read_text().strip()}
(J/'raw/build-validation.json').write_text(json.dumps(result,indent=2)+'\n')
print('build artifact/source checks passed',len(result['artifacts']))
