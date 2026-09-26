; SPDX-FileCopyrightText: 2026 wanghaomiao.cn
; SPDX-License-Identifier: Apache-2.0
;
; seimi-render NSIS installer (per-user, no UAC).
; Invoked by scripts/package-windows.bat after windeployqt staging:
;   makensis -DAPP_VERSION=<ver> -DSRC_DIR=<staged dist> -DOUT_FILE=<setup.exe> installer-windows.nsi
;
; Notes (deliberate design, keep in sync with src/UpdateManager.cpp):
;  - Per-user install into %LOCALAPPDATA%\seimi-render: no admin, no UAC.
;  - Writes ".seimi-install-manifest" next to the exe. The GUI update manager
;    detects this file to pick "installer mode" (silent reinstall via /S)
;    instead of the portable directory-swap path.
;  - This file is ASCII-only on purpose: makensis treats non-BOM files with
;    the legacy ANSI codepage and localized comments would mojibake on other
;    machines.

!ifndef APP_VERSION
!error "APP_VERSION must be passed via -DAPP_VERSION=..."
!endif
!ifndef SRC_DIR
!error "SRC_DIR must be passed via -DSRC_DIR=..."
!endif
!ifndef OUT_FILE
!error "OUT_FILE must be passed via -DOUT_FILE=..."
!endif

!define APP_NAME "seimi-render"
!define APP_PUBLISHER "wanghaomiao.cn"
!define APP_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\seimi-render"
!define APP_REG_KEY "Software\seimi-render"

Name "${APP_NAME} ${APP_VERSION}"
OutFile "${OUT_FILE}"
; Install under the user profile: silent self-update re-runs this installer
; without elevation (see UpdateManager::spawnApplyChild installer branch).
InstallDir "$LOCALAPPDATA\${APP_NAME}"
InstallDirRegKey HKCU "${APP_REG_KEY}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
Unicode true

Icon "${SRC_DIR}\admin-ui\favicon.ico"
UninstallIcon "${SRC_DIR}\admin-ui\favicon.ico"

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "install"
    SetOutPath "$INSTDIR"
    ; Staged windeployqt bundle (exe + Qt DLLs + qml/ + plugins/ + resources/).
    File /r /x data "${SRC_DIR}\*.*"

    ; Installer-mode marker for the GUI update manager.
    FileOpen $0 "$INSTDIR\.seimi-install-manifest" w
    FileWrite $0 "name=seimi-render$\r$\nversion=${APP_VERSION}$\r$\nmode=nsis-per-user$\r$\n"
    FileClose $0

    WriteUninstaller "$INSTDIR\uninstall.exe"

    WriteRegStr HKCU "${APP_REG_KEY}" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${APP_REG_KEY}" "Version" "${APP_VERSION}"
    WriteRegStr HKCU "${APP_UNINST_KEY}" "DisplayName" "${APP_NAME} ${APP_VERSION}"
    WriteRegStr HKCU "${APP_UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKCU "${APP_UNINST_KEY}" "Publisher" "${APP_PUBLISHER}"
    WriteRegStr HKCU "${APP_UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKCU "${APP_UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegDWORD HKCU "${APP_UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${APP_UNINST_KEY}" "NoRepair" 1

    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_NAME}.exe" "--gui"
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

Section "un.install" UnInstall
    ; Stop a running instance first (otherwise the exe is locked and files linger).
    nsExec::ExecToLog 'taskkill /IM ${APP_NAME}.exe /F'
    Sleep 500

    Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
    RMDir "$SMPROGRAMS\${APP_NAME}"

    ; data/ holds the machine-bound cookie vault; ask before wiping it.
    IfFileExists "$INSTDIR\data\*.*" 0 skip_data_prompt
    MessageBox MB_YESNO|MB_ICONQUESTION "Delete user data (login cookies, config) too?$\n$\nChoose No to keep $INSTDIR\data for a future reinstall." IDNO skip_data_prompt
    RMDir /REBOOTOK "$INSTDIR\data"
skip_data_prompt:

    RMDir /REBOOTOK "$INSTDIR"
    DeleteRegKey HKCU "${APP_UNINST_KEY}"
    DeleteRegKey HKCU "${APP_REG_KEY}"
SectionEnd
