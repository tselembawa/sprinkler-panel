@echo off
rem Sprinkler panel - test menu for Windows. Double-click it (it lives in the sprinkler-panel folder).
chcp 65001 >nul
setlocal
title Sprinkler panel - menu
cd /d "%~dp0"
set "ROOT=%~dp0"

where python >nul 2>nul
if errorlevel 1 goto nopython

:menu
set "MISSING="
python -c "import serial, uvicorn, starlette" >nul 2>nul
if errorlevel 1 set "MISSING=1"
cls
echo.
echo   ====================================================
echo      SPRINKLER PANEL  -  Μενού δοκιμών (PC)
echo   ====================================================
echo.
echo     1   Προσομοιωτής module  (βρίσκει μόνος τη USB-RS485)
echo     2   Προσομοιωτής: διαλέγω θύρα και modules (π.χ. 2 3 5)
echo.
echo     3   HMI στο PC με τον πραγματικό controller (Nucleo στο USB)
echo     4   HMI στο PC με τον εικονικό controller (χωρίς hardware)
echo     5   Άνοιγμα του HMI στον browser
echo.
echo     6   Λίστα θυρών COM
echo     7   Εγκατάσταση / ενημέρωση πακέτων Python
echo     8   Άνοιγμα του φακέλου STM32 (Blink)
echo     9   Σταμάτημα όλων (προσομοιωτής και HMI)
echo.
echo     0   Έξοδος
echo.
if defined MISSING echo   ^>^> Λείπουν πακέτα Python - διάλεξε πρώτα το 7.
if defined MISSING echo.
set "CH="
set /p "CH=   Επιλογή: "
if "%CH%"=="1" goto sim_auto
if "%CH%"=="2" goto sim_port
if "%CH%"=="3" goto hmi_serial
if "%CH%"=="4" goto hmi_sim
if "%CH%"=="5" goto open_browser
if "%CH%"=="6" goto list_ports
if "%CH%"=="7" goto install
if "%CH%"=="8" goto stm32
if "%CH%"=="9" goto stop_all
if "%CH%"=="0" goto end
goto menu

:sim_auto
start "Module simulator" /d "%ROOT%." cmd /k python tools\module_sim\module_sim.py
goto menu

:sim_port
call :ports
set "PORT="
set /p "PORT=   Θύρα της USB-RS485 (π.χ. COM7), Enter = αυτόματα: "
if not defined PORT set "PORT=auto"
set "MODS="
set /p "MODS=   Διευθύνσεις modules με κενό (π.χ. 2 3 5), Enter = 2: "
if not defined MODS set "MODS=2"
set "ARGS="
for %%m in (%MODS%) do call set "ARGS=%%ARGS%% -a %%m"
start "Module simulator %MODS%" /d "%ROOT%." cmd /k python tools\module_sim\module_sim.py %PORT% %ARGS%
goto menu

:hmi_serial
echo.
echo   Κλείσε το PuTTY αν είναι ανοιχτό - η θύρα του Nucleo πρέπει να είναι ελεύθερη.
set "SPRINKLER_CONTROLLER=serial"
start "HMI - controller" /d "%ROOT%hmi" cmd /k python -m uvicorn app.main:app --port 8000
call :browser
goto menu

:hmi_sim
set "SPRINKLER_CONTROLLER=sim"
start "HMI - virtual controller" /d "%ROOT%hmi" cmd /k python -m uvicorn app.main:app --port 8000
call :browser
goto menu

:open_browser
start "" http://localhost:8000
goto menu

:list_ports
call :ports
pause
goto menu

:install
echo.
python -m pip install -r "%ROOT%hmi\requirements.txt"
echo.
pause
goto menu

:stm32
if exist "%ROOT%..\stm32\Blink" (
  explorer "%ROOT%..\stm32\Blink"
) else (
  echo   Δεν βρήκα τον φάκελο Documents\stm32\Blink
  pause
)
goto menu

:stop_all
powershell -NoProfile -Command "Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'python*' -and $_.CommandLine -match 'module_sim|uvicorn' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force; Write-Host ('   σταμάτησε: ' + $_.ProcessId) }"
echo.
echo   Έτοιμο. Τα παράθυρα μένουν ανοιχτά - μπορείς να τα κλείσεις.
timeout /t 3 >nul
goto menu

rem ---- helpers ------------------------------------------------------------------
:browser
echo.
echo   Περιμένω λίγα δευτερόλεπτα να ξεκινήσει το HMI ...
timeout /t 5 /nobreak >nul
start "" http://localhost:8000
exit /b

:ports
echo.
echo   Θύρες COM σε αυτό το PC:
python -m serial.tools.list_ports -v
echo.
exit /b

:nopython
echo.
echo   Δεν βρέθηκε η Python. Εγκατάστησέ την από το Microsoft Store ή το python.org
echo   και ξανάνοιξε αυτό το μενού.
echo.
pause
goto end

:end
endlocal
