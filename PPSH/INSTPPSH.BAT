@echo off
echo ÿ
echo This batch file is an installation script for installing the Parallel
echo Port Shell (PPSH) on a Windows NT Workstation or Server.  It uses the
echo settings in ppsh.ini to configure NT's system registry.  The two
echo registry values in ppsh.ini most likely to require changes are
echo InterruptVector, InterruptLevel, and PortAddress.  If you do not want
echo PPSH to start automatically after the system reboots itself, set the
echo registry value Start in ppshell.ini to 0x00000003. If you are using
echo NKPC or Puzzle, set TargetType=1 in ppsh.ini.
echo ÿ
echo This script installs the Parallel Port Shell (PPSH) service on your computer.  
echo Before running this script, verify that the settings in ppsh.ini are correct.
echo ÿ
echo Press Ctrl-C to abort installation. To continue with installation, press Enter.
pause 
echo ÿ
net stop ppshell
xcopy ppshell.sys %SystemRoot%\System32\Drivers /v
regini ppsh.ini
regini ppshell.ini
echo ÿ
echo The PPSH service has been installed on your workstation. You must now
echo reboot.  If the PPSH service fails to start or does not function properly,
echo make sure there are no other devices using the parrallel port specified in
echo ppsh.ini.
