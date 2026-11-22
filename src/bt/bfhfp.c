/* bfhfp — canal de comandos AT do viva-voz (HFP Hands-Free) do BlueALSA, para testes.
 *
 *   bfhfp AA:BB:CC:DD:EE:FF [SEGUNDOS] [COMANDO_AT...]
 *
 * Abre o RFCOMM do HFP pelo D-Bus do BlueALSA (org.bluealsa.RFCOMM1.Open), manda cada comando
 * (com \r) e imprime tudo o que o celular responder durante SEGUNDOS (padrão 5): RING, +CLIP,
 * +CIEV (indicadores de chamada), OK/ERROR. O BlueALSA cuida da conexão de serviço (SLC).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "uso: bfhfp ENDERECO [SEGUNDOS] [AT...]\n"); return 2; }
    char dev[32], path[96];
    snprintf(dev, sizeof dev, "%s", argv[1]);
    for(char *c = dev; *c; c++) if(*c == ':') *c = '_';
    snprintf(path, sizeof path, "/org/bluealsa/hci0/dev_%s/rfcomm", dev);
    int secs = argc > 2 ? atoi(argv[2]) : 5;

    DBusError e;
    dbus_error_init(&e);
    DBusConnection *bus = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
    if(!bus) { fprintf(stderr, "D-Bus: %s\n", e.message); return 1; }
    DBusMessage *m = dbus_message_new_method_call("org.bluealsa", path, "org.bluealsa.RFCOMM1", "Open");
    DBusMessage *r = dbus_connection_send_with_reply_and_block(bus, m, 5000, &e);
    dbus_message_unref(m);
    if(!r) { fprintf(stderr, "RFCOMM1.Open: %s\n", e.message); return 1; }
    int fd = -1;
    if(!dbus_message_get_args(r, &e, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID)) {
        fprintf(stderr, "resposta: %s\n", e.message);
        return 1;
    }
    dbus_message_unref(r);

    for(int i = 3; i < argc; i++) {
        char cmd[256];
        int n = snprintf(cmd, sizeof cmd, "%s\r", argv[i]);
        if(write(fd, cmd, (size_t)n) != n) { perror("write"); return 1; }
        printf("> %s\n", argv[i]);
        fflush(stdout);
        usleep(300000);
    }
    time_t end = time(NULL) + secs;
    char buf[512];
    while(time(NULL) < end) {
        struct pollfd p = { fd, POLLIN, 0 };
        if(poll(&p, 1, 500) <= 0) continue;
        ssize_t k = read(fd, buf, sizeof buf - 1);
        if(k <= 0) { printf("(canal fechado)\n"); break; }
        buf[k] = 0;
        for(char *s = strtok(buf, "\r\n"); s; s = strtok(NULL, "\r\n")) printf("< %s\n", s);
        fflush(stdout);
    }
    close(fd);
    return 0;
}
