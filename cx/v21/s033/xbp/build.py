import os, pathlib, subprocess, time, json
R=pathlib.Path('/root/worktree/zlink-v21-s033/xbp')
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/xbp')
A=R/'.artifacts/wsl'
core=str(A/'install/zlink-core/1.23.0')
env=os.environ.copy(); env.update(ZLINK_CORE_SOURCE='local', ZLINK_CORE_INCLUDE_DIR=core+'/include', ZLINK_CORE_LIB_DIR=core+'/lib', ZLINK_LOCAL_PACKAGE_ROOT=str(A),ZLINK_CORE_PACKAGE_PREFIX=core, ZLINK_BINDING_BUILD_JOBS='3', CMAKE_BUILD_PARALLEL_LEVEL='3',MSBUILDDISABLENODEREUSE='1',DOTNET_CLI_USE_MSBUILD_SERVER='0',UseSharedCompilation='false')
def run(name, args, cpp=False, cwd=R):
    wrapper='/root/jobs/fx/cx/cpp-build-slot.sh' if cpp else '/root/jobs/fx/cx/measure-shared.sh'
    if cpp: args=['bash','-c','ulimit -v 26214400\nexec "$@"','xbp-cap',*args]
    print(time.strftime('%H:%M:%S'),name,flush=True)
    with open(J/'raw'/f'main2-{name}.log','w') as log:
        log.write(json.dumps([wrapper,*args])+'\n');log.flush()
        p=subprocess.run([wrapper,*args],cwd=cwd,env=env,stdout=log,stderr=subprocess.STDOUT)
    print(name,p.returncode,flush=True)
    if p.returncode: raise SystemExit(p.returncode)
corelink=pathlib.Path(core)
if corelink.is_symlink(): corelink.unlink()
run('configure-core',['cmake','-S','core','-B',str(A/'build/core-main'),'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_INSTALL_PREFIX='+core,'-DCMAKE_INSTALL_LIBDIR=lib','-DBUILD_SHARED=ON','-DBUILD_STATIC=OFF','-DBUILD_TESTS=OFF','-DZLINK_BUILD_TESTS=OFF','-DBUILD_BENCHMARKS=OFF'],True)
run('build-core',['cmake','--build',str(A/'build/core-main'),'--target','libzlink','--parallel','3'],True)
run('install-core',['cmake','--install',str(A/'build/core-main')],True)
import hashlib
native=corelink/'lib/libzlink.so.1.23.0'
manifest=corelink/'share/zlink/core-package-provenance.json';manifest.parent.mkdir(parents=True,exist_ok=True)
manifest.write_text(json.dumps({'version':'1.23.0','abiMajor':0,'source':'local','head':'c9408ba50e2cf17bdd555b755ba112304572c22f','runtime':{'sha256':hashlib.sha256(native.read_bytes()).hexdigest(),'soname':'libzlink.so.0'}},indent=2))
run('binding-cpp', ['bash','scripts/local-package/cpp/build-wsl.sh','--core-prefix',core],True)
run('configure-cpp',['cmake','-S','framework/bench/grpc/cpp','-B','framework/bench/grpc/cpp/build','-G','Ninja','-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_TOOLCHAIN_FILE=/root/.cache/zlink/vcpkg/scripts/buildsystems/vcpkg.cmake', '-DVCPKG_MANIFEST_MODE=OFF','-DVCPKG_INSTALLED_DIR=/root/worktree/zlink-ratio040/framework/bench/grpc/cpp/build/vcpkg_installed', '-DZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT='+str(A), '-DZLINK_FRAMEWORK_CPP_TEST_PROBES=OFF'],True)
run('build-cpp',['cmake','--build','framework/bench/grpc/cpp/build','--target','bench_cpp_client','bench_cpp_zlink_server','bench_cpp_framework_server','--parallel','3'],True)
run('build-counter-fixture',['c++','-std=c++20','-O2','-Ibindings/cpp/tests/contract','-Ibindings/cpp/include','-I'+core+'/include',str(J/'counter_fixture.cpp'),str(A/'install/zlink-cpp/1.23.0/lib/libzlink_cpp.a'),'-L'+core+'/lib','-Wl,-rpath,'+core+'/lib','-lzlink','-lpthread','-o',str(J/'counter-fixture')],True)
run('binding-dotnet',['bash','scripts/local-package/dotnet/build-wsl.sh','--core-prefix',core])
run('binding-java',['bash','scripts/local-package/java/build-wsl.sh','--core-prefix',core])
# Use a job-local NuGet cache so the exact freshly built binding is restored.
env['NUGET_PACKAGES']=str(A/'nuget-cache')
run('build-dotnet',['bash','framework/bench/grpc/dotnet/build.sh'])
run('build-java',['bash','framework/bench/grpc/java/build.sh'])
(J/'raw/builds-done.json').write_text(json.dumps({'end':time.time(),'packageRoot':str(A),'core':core}))
