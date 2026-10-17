@echo off
rem Collects hardware/system diagnostics for a crash report (read-only, nothing is sent). See tools\diag\README.md.
rem -ExecutionPolicy Bypass applies to this PowerShell process only; no system setting is changed.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0diag\collect_diagnostics.ps1" %*
