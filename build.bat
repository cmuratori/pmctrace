@echo off
mkdir build
pushd build

call rc -nologo ..\pmctrace_server.rc
move ..\pmctrace_server.res .
call cl -FC -nologo -Zi -Od -Wall ..\pmctrace_server.cpp pmctrace_server.res -Fepmctrace_server_dm.exe -link /SUBSYSTEM:windows
call cl -FC -nologo -Zi -O2 -Wall ..\pmctrace_server.cpp pmctrace_server.res -Fepmctrace_server_rm.exe -link /SUBSYSTEM:windows

call cl -FC -nologo -Zi -Od ..\pmctrace_simple_test.c -Fepmctrace_simple_test_dm.exe
call cl -FC -nologo -Zi -O2 ..\pmctrace_simple_test.c -Fepmctrace_simple_test_rm.exe

where /q nasm || (echo WARNING: nasm not found -- threaded test will not be built)
call nasm -f win64 ..\pmctrace_test_asm.asm -o pmctrace_test_asm.obj
call lib -nologo pmctrace_test_asm.obj
call cl -FC -nologo -Zi -Od ..\pmctrace_threaded_test.cpp -Fepmctrace_threaded_test_dm.exe
call cl -FC -nologo -Zi -O2 ..\pmctrace_threaded_test.cpp -Fepmctrace_threaded_test_rm.exe

popd build
