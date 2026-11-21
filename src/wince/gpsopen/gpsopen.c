/*
 * gpsopen — diagnóstico do GPS no Windows CE do FOSTON: abre COM1: (o driver
 * gps.dll liga o MT3328 e carrega o DSP), grava o NMEA recebido em
 * \SDMMC\BigFoston\gps-ce.log por N segundos (argumento; padrão 120) e fecha.
 * Serve de referência para o bfgps (Linux) e mantém o GPS ativo enquanto o PC
 * lê registradores pelo bfagent. Só lê a porta; não grava nada no aparelho
 * além do log no cartão.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    (void)inst; (void)prev; (void)show;
    int secs = cmd && *cmd ? (int)wcstol(cmd, NULL, 10) : 120;
    if(secs <= 0 || secs > 3600) secs = 120;
    FILE *log = _wfopen(L"\\SDMMC\\BigFoston\\gps-ce.log", L"wb");
    if(!log) return 1;
    DWORD t0 = GetTickCount();
    fprintf(log, "gpsopen: abrindo COM1: por %d s (tick %lu)\r\n", secs, (unsigned long)t0);
    fflush(log);
    HANDLE h = CreateFileW(L"COM1:", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if(h == INVALID_HANDLE_VALUE) {
        fprintf(log, "CreateFileW(COM1:) falhou: erro %lu\r\n", (unsigned long)GetLastError());
        fclose(log);
        return 2;
    }
    COMMTIMEOUTS to = { MAXDWORD, 0, 500, 0, 1000 };
    SetCommTimeouts(h, &to);
    unsigned long total = 0;
    char buf[512];
    while(GetTickCount() - t0 < (DWORD)secs * 1000u) {
        DWORD n = 0;
        if(!ReadFile(h, buf, sizeof buf, &n, NULL)) { Sleep(100); continue; }
        if(n) {
            fwrite(buf, 1, n, log);
            total += n;
            fflush(log);
        }
    }
    fprintf(log, "\r\ngpsopen: %lu bytes em %lu ms\r\n", total, (unsigned long)(GetTickCount() - t0));
    CloseHandle(h);
    fclose(log);
    return 0;
}
