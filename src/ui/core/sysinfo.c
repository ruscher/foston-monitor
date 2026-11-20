#define _DEFAULT_SOURCE
#include "sysinfo.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/if_packet.h>
#endif

bool sysinfo_cpu_sample(bf_cpu_sample_t *s)
{
    FILE *f = fopen("/proc/stat", "r");
    if(!f) return false;
    unsigned long long v[8] = { 0 };
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if(n < 4) return false;
    s->total = 0;
    for(int i = 0; i < 8; i++) s->total += v[i];
    s->idle = v[3] + v[4]; /* idle + iowait */
    return true;
}

int sysinfo_cpu_permille(const bf_cpu_sample_t *a, const bf_cpu_sample_t *b)
{
    unsigned long long dt = b->total - a->total, di = b->idle - a->idle;
    if(b->total <= a->total || di > dt) return 0;
    return (int)(((dt - di) * 1000ULL) / dt);
}

bool sysinfo_mem(unsigned long *total_kb, unsigned long *avail_kb)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if(!f) return false;
    char line[128];
    unsigned long tot = 0, avail = 0, fr = 0, buf = 0, cach = 0;
    bool have_avail = false;
    while(fgets(line, sizeof line, f)) {
        if(sscanf(line, "MemTotal: %lu", &tot) == 1) continue;
        if(sscanf(line, "MemAvailable: %lu", &avail) == 1) { have_avail = true; continue; }
        if(sscanf(line, "MemFree: %lu", &fr) == 1) continue;
        if(sscanf(line, "Buffers: %lu", &buf) == 1) continue;
        if(sscanf(line, "Cached: %lu", &cach) == 1) continue;
    }
    fclose(f);
    if(!have_avail) avail = fr + buf + cach; /* kernels antigos */
    *total_kb = tot;
    *avail_kb = avail;
    return tot > 0;
}

bool sysinfo_loadavg(char *b, size_t n)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if(!f) return false;
    char l1[16], l5[16], l15[16];
    bool ok = fscanf(f, "%15s %15s %15s", l1, l5, l15) == 3;
    fclose(f);
    if(ok) snprintf(b, n, "%s  %s  %s", l1, l5, l15);
    return ok;
}

int sysinfo_proc_count(void)
{
    DIR *d = opendir("/proc");
    if(!d) return -1;
    int n = 0;
    struct dirent *e;
    while((e = readdir(d))) if(isdigit((unsigned char)e->d_name[0])) n++;
    closedir(d);
    return n;
}

long sysinfo_uptime(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    if(!f) return -1;
    long s = -1;
    if(fscanf(f, "%ld", &s) != 1) s = -1;
    fclose(f);
    return s;
}

void sysinfo_os_pretty(char *buf, size_t n)
{
    snprintf(buf, n, "Linux");
    FILE *f = fopen("/etc/os-release", "r");
    if(!f) return;
    char line[256];
    while(fgets(line, sizeof line, f)) {
        if(strncmp(line, "PRETTY_NAME=", 12)) continue;
        char *v = line + 12;
        v[strcspn(v, "\r\n")] = 0;
        if(*v == '"') { v++; char *q = strrchr(v, '"'); if(q) *q = 0; }
        snprintf(buf, n, "%s", v);
        break;
    }
    fclose(f);
}

static char *after_colon(char *l)
{
    char *c = strchr(l, ':');
    if(!c) return NULL;
    c++;
    while(*c == ' ' || *c == '\t') c++;
    c[strcspn(c, "\r\n")] = 0;
    return c;
}

void sysinfo_cpu_model(char *buf, size_t n, int *cores)
{
    char model[128] = "", hw[96] = "", line[256];
    int nc = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if(f) {
        while(fgets(line, sizeof line, f)) {
            char *v;
            if(!strncmp(line, "processor", 9)) nc++;
            if((!strncmp(line, "model name", 10) || !strncmp(line, "Processor", 9)) && !model[0]
               && (v = after_colon(line))) snprintf(model, sizeof model, "%s", v);
            if(!strncmp(line, "Hardware", 8) && (v = after_colon(line))) snprintf(hw, sizeof hw, "%s", v);
        }
        fclose(f);
    }
    /* colapsa espaços repetidos (Intel/AMD gostam de enchê-los) */
    char *w = model;
    for(char *r = model; *r; r++) if(!(*r == ' ' && (w == model || w[-1] == ' '))) *w++ = *r;
    *w = 0;
    if(hw[0]) snprintf(buf, n, "%s (%s)", model[0] ? model : "?", hw);
    else snprintf(buf, n, "%s", model[0] ? model : "?");
    if(cores) *cores = nc;
}

bool sysinfo_disk(const char *path, unsigned long long *total, unsigned long long *avail)
{
    struct statvfs s;
    if(statvfs(path, &s)) return false;
    *total = (unsigned long long)s.f_blocks * s.f_frsize;
    *avail = (unsigned long long)s.f_bavail * s.f_frsize;
    return true;
}

void sysinfo_net(char *ifname, size_t nif, char *ip, size_t nip, char *mac, size_t nmac)
{
    ifname[0] = ip[0] = mac[0] = 0;
    struct ifaddrs *ifa, *i;
    if(getifaddrs(&ifa)) return;
    for(i = ifa; i; i = i->ifa_next) {
        if(!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
        snprintf(ifname, nif, "%s", i->ifa_name);
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, nip);
        break;
    }
#ifdef __linux__
    for(i = ifa; i && ifname[0]; i = i->ifa_next) {
        if(!i->ifa_addr || i->ifa_addr->sa_family != AF_PACKET) continue;
        if(strcmp(i->ifa_name, ifname)) continue;
        struct sockaddr_ll *ll = (struct sockaddr_ll *)i->ifa_addr;
        if(ll->sll_halen == 6)
            snprintf(mac, nmac, "%02x:%02x:%02x:%02x:%02x:%02x", ll->sll_addr[0], ll->sll_addr[1],
                     ll->sll_addr[2], ll->sll_addr[3], ll->sll_addr[4], ll->sll_addr[5]);
    }
#endif
    freeifaddrs(ifa);
}

static int port22_in(const char *path)
{
    FILE *f = fopen(path, "r");
    if(!f) return -1;
    char line[256];
    int found = 0;
    if(!fgets(line, sizeof line, f)) { fclose(f); return 0; } /* cabeçalho */
    while(fgets(line, sizeof line, f)) {
        char local[80];
        unsigned st;
        if(sscanf(line, "%*s %79s %*s %x", local, &st) != 2) continue;
        char *c = strrchr(local, ':');
        if(c && strtoul(c + 1, NULL, 16) == 22 && st == 0x0A) { found = 1; break; }
    }
    fclose(f);
    return found;
}

bf_tri_t sysinfo_ssh_listening(void)
{
    int a = port22_in("/proc/net/tcp"), b = port22_in("/proc/net/tcp6");
    if(a < 0 && b < 0) return BF_UNKNOWN;
    return (a == 1 || b == 1) ? BF_YES : BF_NO;
}

void sysinfo_hostname(char *buf, size_t n)
{
    if(gethostname(buf, n)) snprintf(buf, n, "bigfoston");
    buf[n - 1] = 0;
}

void sysinfo_user(char *buf, size_t n)
{
    struct passwd *pw = getpwuid(geteuid());
    snprintf(buf, n, "%s", pw ? pw->pw_name : "user");
}

void fmt_bytes(unsigned long long b, char *buf, size_t n)
{
    static const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    int i = 0;
    unsigned long long whole = b, frac = 0;
    while(whole >= 1024 && i < 4) {
        frac = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        i++;
    }
    if(i == 0 || whole >= 100) snprintf(buf, n, "%llu %s", whole, u[i]);
    else snprintf(buf, n, "%llu,%llu %s", whole, frac, u[i]);
}

void fmt_duration(long s, char *buf, size_t n)
{
    if(s < 0) { snprintf(buf, n, "—"); return; }
    long d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
    if(d) snprintf(buf, n, "%ldd %ldh %ldmin", d, h, m);
    else if(h) snprintf(buf, n, "%ldh %ldmin", h, m);
    else snprintf(buf, n, "%ldmin %lds", m, s % 60);
}
