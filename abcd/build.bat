@echo off
echo Building Banking Management System...
gcc -std=gnu99 -Wall -Wextra -O2 -D_GNU_SOURCE -D_WIN32_WINNT=0x0600 -D__USE_MINGW_ANSI_STDIO=1 -o server.exe server.c storage.c approval.c models.c -lws2_32
if %ERRORLEVEL% NEQ 0 (
    echo Server build failed!
    exit /b %ERRORLEVEL%
)
gcc -std=gnu99 -Wall -Wextra -O2 -D_GNU_SOURCE -D_WIN32_WINNT=0x0600 -D__USE_MINGW_ANSI_STDIO=1 -o client.exe client.c models.c -lws2_32
if %ERRORLEVEL% NEQ 0 (
    echo Client build failed!
    exit /b %ERRORLEVEL%
)
echo Build successful: server.exe and client.exe generated!
