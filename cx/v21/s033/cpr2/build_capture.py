"""Capture with unmodified main 213e579f52 sources and its rebuilt library, before applying the product patch."""
import json, pathlib, shlex, subprocess
root=pathlib.Path('/root/worktree/zlink-3867e/framework/languages/cpp')
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/cpr2')
build=root/'build/linux-ninja-release'
commands=subprocess.check_output(['ninja','-t','commands','test_cpp_framework_service_wire_codec'],cwd=build,text=True).splitlines()
compile=next(shlex.split(line) for line in commands if 'test_cpp_framework_service_wire_codec.cpp.o -c ' in line)
original_object=compile[compile.index('-o')+1]
compile[compile.index('-o')+1]=str(job/'capture_main_wire.o')
compile[compile.index('-c')+1]=str(job/'capture_main_wire.cpp')
if '-MF' in compile: compile[compile.index('-MF')+1]=str(job/'capture_main_wire.d')
subprocess.run(compile,cwd=build,check=True)
link=shlex.split(commands[-1].removeprefix(': && ').removesuffix(' && :'))
link=[str(job/'capture_main_wire.o') if arg==original_object else arg for arg in link]
link[link.index('-o')+1]=str(job/'capture-main-wire')
subprocess.run(link,cwd=build,check=True)
fixture=root/'tests/fixtures/request-wire-main-213e579f52.json'
fixture.parent.mkdir(parents=True,exist_ok=True)
subprocess.run([str(job/'capture-main-wire'),str(fixture)],cwd=build,check=True)
