@echo off
rem diag: bypass sccache so cl.exe errors reach the MSBuild log
"%ZLINK_SCCACHE_CL%" %*
