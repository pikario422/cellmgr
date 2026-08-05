#include "sysinfo.h"
#include "json_util.h"

#include <sys/statvfs.h>
#include <unistd.h>

typedef struct cpu_sample {
    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
    unsigned long long steal;
} cpu_sample;

static int read_cpu(cpu_sample *s)
{
    FILE *fp = fopen("/proc/stat", "rb");
    if (!fp) {
        return -1;
    }
    char tag[8];
    int n = fscanf(fp, "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
                   tag, &s->user, &s->nice, &s->system, &s->idle, &s->iowait,
                   &s->irq, &s->softirq, &s->steal);
    fclose(fp);
    return n >= 8 ? 0 : -1;
}

static double cpu_usage_percent(void)
{
    static cpu_sample prev;
    static int has_prev = 0;
    cpu_sample cur;
    if (read_cpu(&cur) != 0) {
        return -1.0;
    }
    if (!has_prev) {
        prev = cur;
        has_prev = 1;
        return 0.0;
    }
    unsigned long long prev_idle = prev.idle + prev.iowait;
    unsigned long long cur_idle = cur.idle + cur.iowait;
    unsigned long long prev_total = prev.user + prev.nice + prev.system + prev.idle +
                                    prev.iowait + prev.irq + prev.softirq + prev.steal;
    unsigned long long cur_total = cur.user + cur.nice + cur.system + cur.idle +
                                   cur.iowait + cur.irq + cur.softirq + cur.steal;
    unsigned long long totald = cur_total - prev_total;
    unsigned long long idled = cur_idle - prev_idle;
    prev = cur;
    if (totald == 0) {
        return 0.0;
    }
    return (double)(totald - idled) * 100.0 / (double)totald;
}

static void meminfo(long *total_kb, long *avail_kb)
{
    *total_kb = 0;
    *avail_kb = 0;
    FILE *fp = fopen("/proc/meminfo", "rb");
    if (!fp) {
        return;
    }
    char key[64];
    long value;
    char unit[32];
    while (fscanf(fp, "%63s %ld %31s", key, &value, unit) == 3) {
        if (strcmp(key, "MemTotal:") == 0) {
            *total_kb = value;
        } else if (strcmp(key, "MemAvailable:") == 0) {
            *avail_kb = value;
        }
    }
    fclose(fp);
}

static double uptime_sec(void)
{
    FILE *fp = fopen("/proc/uptime", "rb");
    if (!fp) {
        return 0.0;
    }
    double up = 0.0;
    fscanf(fp, "%lf", &up);
    fclose(fp);
    return up;
}

static void loadavg(double *l1, double *l5, double *l15)
{
    *l1 = *l5 = *l15 = 0.0;
    FILE *fp = fopen("/proc/loadavg", "rb");
    if (!fp) {
        return;
    }
    fscanf(fp, "%lf %lf %lf", l1, l5, l15);
    fclose(fp);
}

static long read_first_temp_millic(void)
{
    for (int i = 0; i < 16; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            continue;
        }
        long t = 0;
        if (fscanf(fp, "%ld", &t) == 1) {
            fclose(fp);
            return t;
        }
        fclose(fp);
    }
    return -1;
}

static void net_bytes(const char *iface, unsigned long long *rx, unsigned long long *tx)
{
    *rx = 0;
    *tx = 0;
    if (!iface || iface[0] == '\0') {
        return;
    }
    FILE *fp = fopen("/proc/net/dev", "rb");
    if (!fp) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *colon = strchr(line, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        trim_in_place(line);
        if (strcmp(line, iface) != 0) {
            continue;
        }
        unsigned long long vals[16] = {0};
        sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
               &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5],
               &vals[6], &vals[7], &vals[8]);
        *rx = vals[0];
        *tx = vals[8];
        break;
    }
    fclose(fp);
}

int sysinfo_json(cellmgr_buf *out, const char *wan_iface)
{
    double cpu = cpu_usage_percent();
    long mem_total = 0;
    long mem_avail = 0;
    meminfo(&mem_total, &mem_avail);
    struct statvfs vfs;
    unsigned long long disk_total = 0;
    unsigned long long disk_free = 0;
    if (statvfs("/", &vfs) == 0) {
        disk_total = (unsigned long long)vfs.f_blocks * (unsigned long long)vfs.f_frsize;
        disk_free = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
    }
    double l1, l5, l15;
    loadavg(&l1, &l5, &l15);
    long temp_millic = read_first_temp_millic();
    unsigned long long rx, tx;
    net_bytes(wan_iface, &rx, &tx);

    if (buf_append(out, "{") != 0) return -1;
    if (buf_appendf(out, "\"cpu_percent\":%.2f", cpu) != 0) return -1;
    if (buf_appendf(out, ",\"mem_total_kb\":%ld,\"mem_available_kb\":%ld", mem_total, mem_avail) != 0) return -1;
    if (buf_appendf(out, ",\"disk_total_bytes\":%llu,\"disk_free_bytes\":%llu", disk_total, disk_free) != 0) return -1;
    if (buf_appendf(out, ",\"loadavg\":[%.2f,%.2f,%.2f]", l1, l5, l15) != 0) return -1;
    if (buf_appendf(out, ",\"uptime_sec\":%.0f", uptime_sec()) != 0) return -1;
    if (temp_millic >= 0) {
        if (buf_appendf(out, ",\"temperature_c\":%.1f", (double)temp_millic / 1000.0) != 0) return -1;
    } else {
        if (buf_append(out, ",\"temperature_c\":null") != 0) return -1;
    }
    if (json_prop_string(out, "wan_iface", wan_iface ? wan_iface : "", 1) != 0) return -1;
    if (buf_appendf(out, ",\"rx_bytes\":%llu,\"tx_bytes\":%llu", rx, tx) != 0) return -1;
    return buf_append(out, "}");
}

int sysinfo_drop_caches(void)
{
    sync();
    FILE *fp = fopen("/proc/sys/vm/drop_caches", "wb");
    if (!fp) {
        return -1;
    }
    int ok = fputs("3\n", fp) >= 0 ? 0 : -1;
    fclose(fp);
    return ok;
}
