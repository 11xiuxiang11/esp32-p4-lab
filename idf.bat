@echo off
rem  p4-lab: forward everything to idf.ps1 (PowerShell execution policy is
rem  bypassed per-process, no system setting is changed)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0idf.ps1" %*
