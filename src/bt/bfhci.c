/* bfhci — estado do adaptador Bluetooth do kernel (hci0), sem o BlueZ.
 *
 *   bfhci            mostra estado, endereço, versões e contadores
 *   bfhci up|down    liga/desliga o adaptador (HCIDEVUP/HCIDEVDOWN)
 *   bfhci scan on|off  visível e conectável / invisível (HCISETSCAN)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define AF_BT 31
#define BTPROTO_HCI 1
#define HCIDEVUP _IOW('H', 201, int)
#define HCIDEVDOWN _IOW('H', 202, int)
#define HCIGETDEVINFO _IOR('H', 211, int)
#define HCISETSCAN _IOW('H', 221, int)

struct hci_dev_stats { uint32_t err_rx, err_tx, cmd_tx, evt_rx, acl_tx, acl_rx, sco_tx, sco_rx, byte_rx, byte_tx; };
struct hci_dev_info {
    uint16_t dev_id;
    char name[8];
    uint8_t bdaddr[6];
    uint32_t flags;
    uint8_t type;
    uint8_t features[8];
    uint32_t pkt_type, link_policy, link_mode;
    uint16_t acl_mtu, acl_pkts, sco_mtu, sco_pkts;
    struct hci_dev_stats stat;
};
struct hci_dev_req { uint16_t dev_id; uint32_t dev_opt; };

int main(int argc, char **argv)
{
    int s = socket(AF_BT, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if(s < 0) { perror("socket(AF_BLUETOOTH)"); return 1; }
    if(argc > 1 && (!strcmp(argv[1], "up") || !strcmp(argv[1], "down"))) {
        int up = !strcmp(argv[1], "up");
        if(ioctl(s, up ? HCIDEVUP : HCIDEVDOWN, 0) < 0 && !(up && errno == EALREADY)) { perror(argv[1]); return 1; }
    } else if(argc > 2 && !strcmp(argv[1], "scan")) {
        struct hci_dev_req r = { 0, !strcmp(argv[2], "on") ? 3u : 0u };   /* SCAN_INQUIRY | SCAN_PAGE */
        if(ioctl(s, HCISETSCAN, &r) < 0) { perror("HCISETSCAN"); return 1; }
    }
    struct hci_dev_info di;
    memset(&di, 0, sizeof di);
    di.dev_id = 0;
    if(ioctl(s, HCIGETDEVINFO, &di) < 0) { perror("HCIGETDEVINFO hci0"); return 1; }
    const uint8_t *a = di.bdaddr;
    printf("%s: %s%s%s  endereço %02X:%02X:%02X:%02X:%02X:%02X\n", di.name, di.flags & 1 ? "LIGADO" : "desligado",
           di.flags & (1 << 4) ? " visível" : "", di.flags & (1 << 3) ? " conectável" : "", a[5], a[4], a[3], a[2], a[1], a[0]);
    printf("  ACL %u x %u, SCO %u x %u; comandos %u, eventos %u, ACL tx/rx %u/%u, erros rx/tx %u/%u\n", di.acl_mtu, di.acl_pkts,
           di.sco_mtu, di.sco_pkts, di.stat.cmd_tx, di.stat.evt_rx, di.stat.acl_tx, di.stat.acl_rx, di.stat.err_rx, di.stat.err_tx);
    close(s);
    return 0;
}
