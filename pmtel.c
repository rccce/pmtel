/*
 * pmtel.c - read-only dump of the "pmtelemetry" IOReport counters on Intel Macs and Hackintoshes.
 *
 * Apple's com.apple.driver.pmtelemetry publishes, through IOReport, the counters the CPU and the
 * chipset (PCH) keep about package C-states:
 *   - how often the package entered each C-state (PKGC_Cx_ENTRANCE_COUNTER),
 *   - why the package was refused a deeper state (PKGC_BREAK_CAUSE_*, PKGC_BCLK_*),
 *   - the latency tolerance (LTR) every chipset device reports right now (pmtelemetry_ltr),
 *   - chipset activity (PLLs, PHY lanes, USB, SATA, audio ...) (pmtelemetry_pch).
 * The program samples them over an idle window and prints the differences.
 * It only reads. It loads nothing into the kernel and changes no setting.
 *
 * Build:  clang -O2 -Wall -arch x86_64 -mmacosx-version-min=10.15 -framework CoreFoundation -o pmtel pmtel.c
 * Usage:  sudo ./pmtel [seconds]      default 20 s; leave the Mac untouched while it runs
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

typedef const void *IOReportSubscriptionRef;

/* libIOReport is private; its functions are looked up at run time so that the build does not
   depend on a stub library in the SDK. Prototypes as used by macmon / socpowerbud. */
static CFDictionaryRef (*pCopyChannelsInGroup)(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
static void (*pMergeChannels)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
static IOReportSubscriptionRef (*pCreateSubscription)(void *, CFMutableDictionaryRef, CFMutableDictionaryRef *, uint64_t, CFTypeRef);
static CFDictionaryRef (*pCreateSamples)(IOReportSubscriptionRef, CFMutableDictionaryRef, CFTypeRef);
static CFDictionaryRef (*pCreateSamplesDelta)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
static CFStringRef (*pGetGroup)(CFDictionaryRef);
static CFStringRef (*pGetSubGroup)(CFDictionaryRef);
static CFStringRef (*pGetName)(CFDictionaryRef);
static int64_t (*pSimpleGet)(CFDictionaryRef, int32_t);

static int load_ioreport(void)
{
    void *h = dlopen("/usr/lib/libIOReport.dylib", RTLD_NOW);
    if (!h) {
        fprintf(stderr, "pmtel: cannot load libIOReport: %s\n", dlerror());
        return -1;
    }
#define SYM(var, name)                                               \
    do {                                                             \
        *(void **)(&(var)) = dlsym(h, name);                         \
        if (!(var)) {                                                \
            fprintf(stderr, "pmtel: libIOReport lacks %s\n", name);  \
            return -1;                                               \
        }                                                            \
    } while (0)
    SYM(pCopyChannelsInGroup, "IOReportCopyChannelsInGroup");
    SYM(pMergeChannels, "IOReportMergeChannels");
    SYM(pCreateSubscription, "IOReportCreateSubscription");
    SYM(pCreateSamples, "IOReportCreateSamples");
    SYM(pCreateSamplesDelta, "IOReportCreateSamplesDelta");
    SYM(pGetGroup, "IOReportChannelGetGroup");
    SYM(pGetSubGroup, "IOReportChannelGetSubGroup");
    SYM(pGetName, "IOReportChannelGetChannelName");
    SYM(pSimpleGet, "IOReportSimpleGetIntegerValue");
#undef SYM
    return 0;
}

static CFArrayRef channels_of(CFDictionaryRef d)
{
    if (!d)
        return NULL;
    CFTypeRef v = CFDictionaryGetValue(d, CFSTR("IOReportChannels"));
    if (!v || CFGetTypeID(v) != CFArrayGetTypeID())
        return NULL;
    return (CFArrayRef)v;
}

static void cstr(CFStringRef s, char *buf, size_t n)
{
    buf[0] = 0;
    if (s && CFGetTypeID(s) == CFStringGetTypeID())
        CFStringGetCString(s, buf, (CFIndex)n, kCFStringEncodingUTF8);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* One LTR half (16 bits): value 9:0, scale 12:10, requirement bit 15. Returns ns, or -1 = none. */
static double ltr_ns(uint32_t half)
{
    static const double mult[8] = {1, 32, 1024, 32768, 1048576, 33554432, 0, 0};
    if (!(half & 0x8000))
        return -1;
    return (double)(half & 0x3FF) * mult[(half >> 10) & 7];
}

static void print_ltr(const char *tag, double ns)
{
    if (ns < 0)
        printf("  %s none", tag);
    else if (ns >= 1000)
        printf("  %s %.1f us", tag, ns / 1000.0);
    else
        printf("  %s %.0f ns", tag, ns);
}

#define MAXCH 2048
struct chan {
    char group[64], sub[64], name[96];
    int64_t now, delta;
    int dup;
};
static struct chan ch[MAXCH];
static int nch;

static const char *kGroups[] = {"pmtelemetry_cpu", "pmtelemetry_ltr", "pmtelemetry_pch",
                                "pmtelemetry_uncore", "pmtelemetry_flex"};

static int has_prefix(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

int main(int argc, char **argv)
{
    int secs = 20;
    if (argc > 1) {
        secs = atoi(argv[1]);
        if (secs < 2 || secs > 600)
            secs = 20;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (load_ioreport())
        return 1;

    /* collect the channels of the pmtelemetry groups */
    CFDictionaryRef first = NULL;
    for (size_t i = 0; i < sizeof kGroups / sizeof kGroups[0]; i++) {
        CFStringRef g = CFStringCreateWithCString(NULL, kGroups[i], kCFStringEncodingUTF8);
        CFDictionaryRef c = pCopyChannelsInGroup(g, NULL, 0, 0, 0);
        CFRelease(g);
        CFArrayRef a = channels_of(c);
        if (!a || CFArrayGetCount(a) == 0) {
            printf("# group %s: not present\n", kGroups[i]);
            if (c)
                CFRelease(c);
            continue;
        }
        if (!first) {
            first = c;
        } else {
            pMergeChannels(first, c, NULL);
            CFRelease(c);
        }
    }
    if (!first) {
        fprintf(stderr, "pmtel: no pmtelemetry channels on this Mac\n");
        return 1;
    }
    CFMutableDictionaryRef want = CFDictionaryCreateMutableCopy(NULL, CFDictionaryGetCount(first), first);
    CFRelease(first);

    CFMutableDictionaryRef subbed = NULL;
    IOReportSubscriptionRef sub = pCreateSubscription(NULL, want, &subbed, 0, NULL);
    if (!sub) {
        fprintf(stderr, "pmtel: IOReportCreateSubscription failed (run it with sudo)\n");
        return 1;
    }

    char tbuf[64];
    time_t t0 = time(NULL);
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", localtime(&t0));
    struct utsname u;
    uname(&u);
    uint64_t tsc = 0;
    size_t sz = sizeof tsc;
    if (sysctlbyname("machdep.tsc.frequency", &tsc, &sz, NULL, 0) != 0)
        tsc = 0;
    printf("pmtel 1: %s, Darwin %s, TSC %.0f MHz, window %d s (sampling, do not touch the Mac)\n",
           tbuf, u.release, tsc / 1e6, secs);

    double a0 = now_s();
    CFDictionaryRef s1 = pCreateSamples(sub, want, NULL);
    sleep((unsigned)secs);
    CFDictionaryRef s2 = pCreateSamples(sub, want, NULL);
    double el = now_s() - a0;
    if (!s1 || !s2) {
        fprintf(stderr, "pmtel: IOReportCreateSamples failed\n");
        return 1;
    }
    CFDictionaryRef d = pCreateSamplesDelta(s1, s2, NULL);
    CFArrayRef a2 = channels_of(s2), ad = channels_of(d);
    if (!a2 || !ad) {
        fprintf(stderr, "pmtel: samples have no channels\n");
        return 1;
    }
    printf("measured window: %.2f s\n", el);

    /* read current values from the second sample */
    CFIndex n2 = CFArrayGetCount(a2);
    for (CFIndex i = 0; i < n2 && nch < MAXCH; i++) {
        CFDictionaryRef c = (CFDictionaryRef)CFArrayGetValueAtIndex(a2, i);
        struct chan *p = &ch[nch++];
        cstr(pGetGroup(c), p->group, sizeof p->group);
        cstr(pGetSubGroup(c), p->sub, sizeof p->sub);
        cstr(pGetName(c), p->name, sizeof p->name);
        p->now = pSimpleGet(c, 0);
        p->delta = 0;
        p->dup = 1;
        for (int k = 0; k < nch - 1; k++)
            if (!strcmp(ch[k].group, p->group) && !strcmp(ch[k].sub, p->sub) && !strcmp(ch[k].name, p->name))
                p->dup++;
    }
    /* match the differences by position, falling back to the name */
    CFIndex nd = CFArrayGetCount(ad);
    for (CFIndex i = 0; i < nd; i++) {
        CFDictionaryRef c = (CFDictionaryRef)CFArrayGetValueAtIndex(ad, i);
        char g[64], s[64], nm[96];
        cstr(pGetGroup(c), g, sizeof g);
        cstr(pGetSubGroup(c), s, sizeof s);
        cstr(pGetName(c), nm, sizeof nm);
        int64_t v = pSimpleGet(c, 0);
        if (i < nch && !strcmp(ch[i].group, g) && !strcmp(ch[i].sub, s) && !strcmp(ch[i].name, nm)) {
            ch[i].delta = v;
            continue;
        }
        for (int k = 0; k < nch; k++)
            if (!strcmp(ch[k].group, g) && !strcmp(ch[k].sub, s) && !strcmp(ch[k].name, nm) && ch[k].delta == 0) {
                ch[k].delta = v;
                break;
            }
    }

    /* print, group by group */
    const char *cur = "";
    const char *cursub = "";
    for (int i = 0; i < nch; i++) {
        struct chan *p = &ch[i];
        if (strcmp(cur, p->group) || strcmp(cursub, p->sub)) {
            cur = p->group;
            cursub = p->sub;
            printf("\n== %s%s%s\n", p->group, p->sub[0] ? " / " : "", p->sub);
        }
        char label[112];
        if (p->dup > 1)
            snprintf(label, sizeof label, "%s #%d", p->name, p->dup);
        else
            snprintf(label, sizeof label, "%s", p->name);
        printf("%-48s delta %15lld  now %20lld", label, (long long)p->delta, (long long)p->now);
        if (!strcmp(p->group, "pmtelemetry_ltr") && strstr(p->name, " Raw") && !has_prefix(p->name, "IGN")) {
            uint32_t raw = (uint32_t)p->now;
            printf("  = 0x%08X", raw);
            print_ltr("snoop", ltr_ns(raw & 0xFFFF));
            print_ltr("no-snoop", ltr_ns(raw >> 16));
        }
        if (tsc && el > 0 && (has_prefix(p->name, "MSR_IA32_PKG_C") || has_prefix(p->name, "MSR_IA32_CORE_C") ||
                              has_prefix(p->name, "MSR_IA32_LLC_FLUSHED")) && strstr(p->name, "RESIDENCY"))
            printf("  (%.1f%% of the window)", 100.0 * (double)p->delta / ((double)tsc * el));
        printf("\n");
    }
    printf("\ndone\n");
    return 0;
}
