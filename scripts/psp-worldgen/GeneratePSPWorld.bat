@echo off
rem OptiCraft PSP world generator: builds a complete fixed-size world with vanilla
rem Minecraft 1.2.5 terrain on this PC. Copy the result to the PSP afterwards.
setlocal
cd /d "%~dp0"

echo OptiCraft PSP world generator (vanilla 1.2.5 terrain)
echo.
set /p NAME=World folder name (no spaces, e.g. MyWorld):
if "%NAME%"=="" set NAME=PSPWorld
set /p SEED=Seed (number or text, empty = random):
if "%SEED%"=="" set SEED=random
set /p SIZE=Size in blocks (e.g. 3000 or 5000):
if "%SIZE%"=="" set SIZE=3000

echo.
OptiCraft.exe --pregen out "%NAME%" "%SEED%" %SIZE%
echo.
echo When it says Done: copy the folder out\%NAME% to
echo   PSP\SAVEDATA\OPTICRAFT\saves\%NAME%
echo on the PSP (Memory Stick or PSP Go internal storage).
pause
