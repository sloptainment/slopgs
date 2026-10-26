/* oracle.c -- renders a MIDI file through the real swmidi.sys (XP SP3,
 * 5.1.2600.5512). The driver image is mapped into this 32-bit process, its
 * ntoskrnl/ks imports are stubbed, and its synth core is called directly:
 * ControlLogic::RecordMIDI/RecordSysEx to queue, ControlLogic::Mix to render.
 * The kernel-streaming plumbing (IRPs, pins, clocks) is never entered.
 *
 * Same command line as msgs-render, so score.py can A/B it via MSGS_RENDER:
 *   oracle [--jitter=MS] <dls> <smf> <loops> <max_frames> <out_wav>
 * The driver path is compiled in (SWMIDI_SYS, see Makefile).
 *
 * Freestanding i386: no 32-bit libc is assumed, syscalls go through int 0x80.
 * Entry point VMAs come from the driver's public symbols (swmidi.pdb). */
#include <stdint.h>

#include "engine/smf.h"

#ifndef SWMIDI_SYS
#error "build with -DSWMIDI_SYS=\"/path/to/swmidi.sys\""
#endif
/* Frames per ControlLogic::Mix call: FillBuffer [A:0x181da] mixes one
 * 0x1000-byte stereo buffer [A:0x184c0] = 1024 frames. */
#ifndef ORACLE_MIX_FRAMES
#define ORACLE_MIX_FRAMES 1024u
#endif

#define STDCALL __attribute__((stdcall))
#define FASTCALL __attribute__((fastcall))
#define THISCALL __attribute__((thiscall))

/* ---- syscalls ---------------------------------------------------------- */

static int32_t sys3(int32_t n, int32_t a, int32_t b, int32_t c)
{
    int32_t r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return r;
}

static __attribute__((noreturn)) void sys_exit(int code)
{
    sys3(1, code, 0, 0);
    __builtin_unreachable();
}
#define sys_read(fd, p, n) sys3(3, fd, (int32_t) (p), n)
#define sys_write(fd, p, n) sys3(4, fd, (int32_t) (p), n)
#define sys_open(path, fl) sys3(5, (int32_t) (path), fl, 0644)
#define sys_close(fd) sys3(6, fd, 0, 0)
#define sys_lseek(fd, off) sys3(19, fd, off, 0)

/* old_mmap(90) takes its six arguments as a block */
static void *sys_mmap_rwx(void *hint, uint32_t len)
{
    int32_t a[6] = {(int32_t) hint, (int32_t) len, 7, 0x22, -1, 0};
    int32_t r = sys3(90, (int32_t) a, 0, 0);
    return (r < 0 && r > -4096) ? 0 : (void *) r;
}

static uint32_t slen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void say(const char *s) { sys_write(2, s, slen(s)); }

static __attribute__((noreturn)) void die(const char *what, const char *arg)
{
    say("oracle: ");
    say(what);
    if (arg)
    {
        say(": ");
        say(arg);
    }
    say("\n");
    sys_exit(2);
}

void *memcpy(void *d, const void *s, uint32_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, uint32_t n)
{
    uint8_t *dp = d;
    while (n--)
        *dp++ = (uint8_t) c;
    return d;
}

/* ---- heap: bump allocator, zeroed, never freed ------------------------- */

static uint8_t g_heap[256u << 20] __attribute__((aligned(16)));
static uint32_t g_heap_used;

void *rt_alloc(uint32_t n)
{
    n = (n + 15u) & ~15u;
    if (n > sizeof g_heap - g_heap_used)
        die("out of memory", 0);
    void *p = g_heap + g_heap_used;
    g_heap_used += n;
    return p;
}

static uint8_t *read_file(const char *path, uint32_t *len)
{
    int fd = sys_open(path, 0);
    if (fd < 0)
        return 0;
    int32_t n = sys3(19, fd, 0, 2); /* SEEK_END */
    sys_lseek(fd, 0);
    uint8_t *p = rt_alloc((uint32_t) n + 1);
    int32_t got = 0;
    while (got < n)
    {
        int32_t r = sys_read(fd, p + got, n - got);
        if (r <= 0)
            break;
        got += r;
    }
    sys_close(fd);
    if (got != n)
        return 0;
    *len = (uint32_t) n;
    return p;
}

/* ---- ntoskrnl stubs ---------------------------------------------------- */

typedef uint16_t wchar;

static uint8_t *g_dls;
static uint32_t g_dls_len, g_dls_pos;

typedef struct
{
    int32_t status;
    uint32_t information;
} IoStatus;

static STDCALL void *ExAllocatePoolWithTag(int type, uint32_t n, uint32_t tag)
{
    (void) type, (void) tag;
    return rt_alloc(n);
}
static STDCALL void ExFreePool(void *p) { (void) p; }

/* single-threaded: mutexes and waits are no-ops */
static STDCALL void KeInitializeMutex(void *m, uint32_t l) { (void) m, (void) l; }
static STDCALL int32_t KeWaitForSingleObject(void *o, int r, int m, int a,
                                             void *t)
{
    (void) o, (void) r, (void) m, (void) a, (void) t;
    return 0;
}
static STDCALL int32_t KeReleaseMutex(void *m, uint8_t w)
{
    (void) m, (void) w;
    return 0;
}

static STDCALL int32_t KeSaveFloatingPointState(void *s)
{
    (void) s;
    return 0;
}
static STDCALL int32_t KeRestoreFloatingPointState(void *s)
{
    (void) s;
    return 0;
}

static FASTCALL int32_t InterlockedIncrement(int32_t *p) { return ++*p; }
static FASTCALL int32_t InterlockedDecrement(int32_t *p) { return --*p; }

static uint32_t wcslen(const wchar *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}
static wchar *wcscpy(wchar *d, const wchar *s)
{
    wchar *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}
static wchar *wcscat(wchar *d, const wchar *s)
{
    wcscpy(d + wcslen(d), s);
    return d;
}
static int wcscmp(const wchar *a, const wchar *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (int) *a - (int) *b;
}

typedef struct
{
    uint16_t len, max;
    wchar *buf;
} UnicodeString;

static STDCALL void RtlInitUnicodeString(UnicodeString *u, wchar *s)
{
    u->buf = s;
    u->len = s ? (uint16_t) (wcslen(s) * 2) : 0;
    u->max = s ? (uint16_t) (u->len + 2) : 0;
}

/* No registry: GetRegDlsFileName fails and the driver keeps its built-in
 * \SystemRoot\System32\Drivers\gm.dls, which ZwCreateFile maps to <dls>. */
static STDCALL int32_t ZwOpenKey(void *h, uint32_t a, void *o)
{
    (void) h, (void) a, (void) o;
    return (int32_t) 0xC0000034; /* STATUS_OBJECT_NAME_NOT_FOUND */
}
static STDCALL int32_t ZwClose(void *h)
{
    (void) h;
    return 0;
}

/* Collection::Open [A:0x15efa]: create, query FileStandardInformation, one
 * read of the whole file. */
static STDCALL int32_t ZwCreateFile(void **h, uint32_t acc, void *oa,
                                    IoStatus *io, void *alloc, uint32_t attr,
                                    uint32_t share, uint32_t disp,
                                    uint32_t opts, void *ea, uint32_t ealen)
{
    (void) acc, (void) oa, (void) alloc, (void) attr, (void) share;
    (void) disp, (void) opts, (void) ea, (void) ealen;
    *h = (void *) 1;
    g_dls_pos = 0;
    io->status = 0;
    io->information = 1; /* FILE_OPENED */
    return 0;
}
static STDCALL int32_t ZwQueryInformationFile(void *h, IoStatus *io,
                                              uint8_t *info, uint32_t len,
                                              uint32_t cls)
{
    (void) h;
    if (cls != 5 || len < 24) /* FileStandardInformation */
        return (int32_t) 0xC0000003;
    memset(info, 0, len);
    *(uint32_t *) (info + 0) = g_dls_len; /* AllocationSize */
    *(uint32_t *) (info + 8) = g_dls_len; /* EndOfFile */
    io->status = 0;
    io->information = 24;
    return 0;
}
static STDCALL int32_t ZwReadFile(void *h, void *ev, void *apc, void *ctx,
                                  IoStatus *io, uint8_t *buf, uint32_t len,
                                  int64_t *off, uint32_t *key)
{
    (void) h, (void) ev, (void) apc, (void) ctx, (void) key;
    if (off)
        g_dls_pos = (uint32_t) *off;
    uint32_t n = g_dls_len - g_dls_pos;
    if (n > len)
        n = len;
    memcpy(buf, g_dls + g_dls_pos, n);
    g_dls_pos += n;
    io->status = 0;
    io->information = n;
    return 0;
}

/* MSVC 64-bit helpers: args on the stack, result in edx:eax, callee pops */
static STDCALL int64_t allmul(int64_t a, int64_t b) { return a * b; }

static uint64_t udiv64(uint64_t n, uint64_t d)
{
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--)
    {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d)
            r -= d, q |= (uint64_t) 1 << i;
    }
    return q;
}
static STDCALL int64_t alldiv(int64_t a, int64_t b)
{
    int neg = (a < 0) != (b < 0);
    uint64_t q = udiv64(a < 0 ? -(uint64_t) a : (uint64_t) a,
                        b < 0 ? -(uint64_t) b : (uint64_t) b);
    return neg ? -(int64_t) q : (int64_t) q;
}

/* _allshl/_allshr: value in edx:eax, count in cl */
void allshl(void);
void allshr(void);
__asm__(".text\n.globl allshl\n.globl allshr\n"
        "allshl:\n"
        "  cmpb $64, %cl\n  jae 2f\n"
        "  cmpb $32, %cl\n  jae 1f\n"
        "  shldl %cl, %eax, %edx\n  shll %cl, %eax\n  ret\n"
        "1: movl %eax, %edx\n  xorl %eax, %eax\n"
        "  andb $31, %cl\n  shll %cl, %edx\n  ret\n"
        "2: xorl %eax, %eax\n  xorl %edx, %edx\n  ret\n"
        "allshr:\n"
        "  cmpb $64, %cl\n  jae 2f\n"
        "  cmpb $32, %cl\n  jae 1f\n"
        "  shrdl %cl, %edx, %eax\n  sarl %cl, %edx\n  ret\n"
        "1: movl %edx, %eax\n  sarl $31, %edx\n"
        "  andb $31, %cl\n  sarl %cl, %eax\n  ret\n"
        "2: sarl $31, %edx\n  movl %edx, %eax\n  ret\n");

/* KSYSTEM_TIME; a data import, never advanced */
static uint32_t g_tick_count[3];

static const struct
{
    const char *name;
    void *fn;
} k_imports[] = {
    {"ExAllocatePoolWithTag", ExAllocatePoolWithTag},
    {"ExFreePool", ExFreePool},
    {"KeInitializeMutex", KeInitializeMutex},
    {"KeWaitForSingleObject", KeWaitForSingleObject},
    {"KeReleaseMutex", KeReleaseMutex},
    {"KeSaveFloatingPointState", KeSaveFloatingPointState},
    {"KeRestoreFloatingPointState", KeRestoreFloatingPointState},
    {"InterlockedIncrement", InterlockedIncrement},
    {"InterlockedDecrement", InterlockedDecrement},
    {"wcslen", wcslen},
    {"wcscpy", wcscpy},
    {"wcscat", wcscat},
    {"wcscmp", wcscmp},
    {"RtlInitUnicodeString", RtlInitUnicodeString},
    {"ZwOpenKey", ZwOpenKey},
    {"ZwClose", ZwClose},
    {"ZwCreateFile", ZwCreateFile},
    {"ZwQueryInformationFile", ZwQueryInformationFile},
    {"ZwReadFile", ZwReadFile},
    {"_allmul", allmul},
    {"_alldiv", alldiv},
    {"_allshl", allshl},
    {"_allshr", allshr},
    {"KeTickCount", g_tick_count},
};

/* Every other import resolves to a thunk `push idx; call trap`: reaching one
 * means the render path left what this harness models. */
static const char *g_trap_names[128];

static __attribute__((noreturn, used)) void trap(uint32_t idx)
{
    die("driver called unstubbed import", g_trap_names[idx]);
}

/* ---- PE loader --------------------------------------------------------- */

#define U16(p) (*(const uint16_t *) (p))
#define U32(p) (*(const uint32_t *) (p))

/* XP SP3 swmidi.sys 5.1.2600.5512; the VMAs below are only valid for it */
#define IMAGE_BASE 0x10000u
#define IMAGE_STAMP 0x480254b3u

static uint8_t *g_image;

static void *at(uint32_t vma) { return g_image + (vma - IMAGE_BASE); }

static void load_driver(const char *path)
{
    uint32_t flen;
    const uint8_t *f = read_file(path, &flen);
    if (!f || flen < 0x40)
        die("could not read", path);
    const uint8_t *pe = f + U32(f + 0x3c);
    const uint8_t *opt = pe + 24;
    if (U32(pe) != 0x4550 || U32(pe + 8) != IMAGE_STAMP ||
        U32(opt + 28) != IMAGE_BASE)
        die("not the XP SP3 swmidi.sys (5.1.2600.5512)", path);
    uint32_t size = U32(opt + 56), page = (size + 4095u) & ~4095u;

    /* image + one page of import trap thunks */
    g_image = sys_mmap_rwx((void *) IMAGE_BASE, page + 4096);
    if (!g_image)
        die("mmap failed", 0);
    memcpy(g_image, f, U32(opt + 60));
    const uint8_t *sec = opt + U16(pe + 20);
    for (uint32_t i = 0; i < U16(pe + 6); i++, sec += 40)
    {
        uint32_t raw = U32(sec + 16), vsz = U32(sec + 8);
        memcpy(g_image + U32(sec + 12), f + U32(sec + 20),
               raw < vsz || !vsz ? raw : vsz);
    }

    /* base relocations (IMAGE_REL_BASED_HIGHLOW only, all this image has) */
    uint32_t delta = (uint32_t) g_image - IMAGE_BASE;
    const uint8_t *rel = g_image + U32(opt + 96 + 5 * 8);
    const uint8_t *rel_end = rel + U32(opt + 96 + 5 * 8 + 4);
    while (delta && rel < rel_end && U32(rel + 4))
    {
        for (uint32_t k = 8; k < U32(rel + 4); k += 2)
        {
            uint16_t e = U16(rel + k);
            if (e >> 12 == 3)
                *(uint32_t *) (g_image + U32(rel) + (e & 0xfff)) += delta;
        }
        rel += U32(rel + 4);
    }

    uint8_t *thunk = g_image + page;
    uint32_t ntrap = 0;
    for (const uint8_t *d = g_image + U32(opt + 96 + 8); U32(d + 12); d += 20)
    {
        uint32_t *iat = (uint32_t *) (g_image + U32(d + 16));
        const uint32_t *names = (const uint32_t *) (g_image + U32(d));
        for (; *names; names++, iat++)
        {
            const char *name = (const char *) g_image + *names + 2;
            void *fn = 0;
            for (uint32_t k = 0; k < sizeof k_imports / sizeof *k_imports; k++)
            {
                const char *a = k_imports[k].name, *b = name;
                while (*a && *a == *b)
                    a++, b++;
                if (*a == *b)
                    fn = k_imports[k].fn;
            }
            if (!fn)
            {
                g_trap_names[ntrap] = name;
                thunk[0] = 0x68; /* push imm32 */
                *(uint32_t *) (thunk + 1) = ntrap++;
                thunk[5] = 0xe8; /* call rel32 */
                *(int32_t *) (thunk + 6) =
                    (int32_t) ((uint8_t *) trap - (thunk + 10));
                fn = thunk;
                thunk += 10;
            }
            *iat = (uint32_t) fn;
        }
    }
}

/* ---- driver entry points (swmidi.pdb publics) -------------------------- */

typedef void(STDCALL *StaticInit)(void);
typedef int(STDCALL *StaticInitRet)(void);
typedef void(THISCALL *Ctor)(void *);
typedef int(THISCALL *RecordMidiFn)(void *, int64_t, uint8_t, uint8_t, uint8_t);
typedef int(THISCALL *RecordSysExFn)(void *, int64_t, uint32_t, uint8_t *);
typedef void(THISCALL *MixFn)(void *, int16_t *, uint32_t);

#define CONTROL_LOGIC_SIZE 0x1360u /* PinDispatchCreate [A:0x183f1] */
#define CL_VOICES_ACTIVE 0x1c0u    /* Mix walks this list [A:0x130b4] */

static uint8_t *g_cl;
static uint64_t g_pos;  /* ControlLogic's sample clock, [A:0x13197] */
static uint32_t g_off;  /* offset of the event being dispatched */

static void set_fpu_cw(uint16_t cw) { __asm__ volatile("fldcw %0" ::"m"(cw)); }

static void driver_start(void)
{
    /* DriverEntry [A:0x13a8e] */
    ((StaticInit) at(0x16804))(); /* MIDIRecorder::InitTables */
    ((StaticInit) at(0x16a3e))(); /* Voice::Init */
    ((StaticInit) at(0x168be))(); /* Wave::Init */
    /* FilterDispatchCreate [A:0x13b21] */
    if (!((StaticInitRet) at(0x16962))()) /* MIDIRecorder::InitEventList */
        die("InitEventList failed", 0);
    /* PinDispatchCreate [A:0x18402]: 22050 Hz stereo, loads gm.dls */
    g_cl = rt_alloc(CONTROL_LOGIC_SIZE);
    ((Ctor) at(0x12984))(g_cl); /* ControlLogic::ControlLogic */
}

/* ---- the hooks smf.c drives -------------------------------------------- */

void voice_set_event_offset(uint32_t frames) { g_off = frames; }

void synth_midi(uint32_t status, uint32_t d1, uint32_t d2)
{
    ((RecordMidiFn) at(0x131c0))(g_cl, (int64_t) (g_pos + g_off),
                                 (uint8_t) status, (uint8_t) d1, (uint8_t) d2);
}

/* smf.c strips the F0; the driver wants the whole message [A:0x13699] */
void synth_sysex(const uint8_t *buf, uint32_t len)
{
    uint8_t *m = rt_alloc(len + 1);
    m[0] = 0xF0;
    memcpy(m + 1, buf, len);
    ((RecordSysExFn) at(0x1367a))(g_cl, (int64_t) (g_pos + g_off), len + 1, m);
}

void render_frames(int16_t *out, uint32_t frames)
{
    ((MixFn) at(0x13054))(g_cl, out, frames); /* ControlLogic::Mix */
    g_pos += frames;
}

int voice_any_active(void) { return *(void **) (g_cl + CL_VOICES_ACTIVE) != 0; }

/* ---- main -------------------------------------------------------------- */

static uint32_t to_u32(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint32_t) (*s++ - '0');
    return v;
}

static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void wav_header(int fd, uint32_t frames)
{
    uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0"
                    "\0\0\0\0\0\0\0\0\x04\0\x10\0data";
    put_u32(h + 4, 36 + frames * 4);
    put_u32(h + 24, 22050);
    put_u32(h + 28, 22050 * 4);
    put_u32(h + 40, frames * 4);
    sys_lseek(fd, 0);
    sys_write(fd, h, 44);
}

static int oracle_main(int argc, char **argv)
{
    /* --jitter=MS: capture realism (smf_set_jitter_ms), as msgs-render */
    const char *flag = "--jitter=";
    uint32_t k = 0;
    while (argc > 1 && flag[k] && argv[1][k] == flag[k])
        k++;
    if (!flag[k])
    {
        smf_set_jitter_ms(to_u32(argv[1] + k));
        argv++;
        argc--;
    }
    if (argc != 6)
    {
        say("usage: oracle [--jitter=MS] <dls> <smf> <loops> <max_frames> "
            "<out_wav>\n");
        return 1;
    }
    if (!(g_dls = read_file(argv[1], &g_dls_len)))
        die("could not read", argv[1]);
    uint32_t smf_len;
    uint8_t *smf = read_file(argv[2], &smf_len);
    if (!smf)
        die("could not read", argv[2]);
    uint32_t max_frames = to_u32(argv[4]);

    /* x87 rounds to double, as msgs-render's SSE math does, so both see the
       same event sample times. The driver runs under it too: 0x27F and 0x37F
       render all 58 probes and tests/ byte-identically. */
    set_fpu_cw(0x27F);
    if (smf_load(smf, smf_len) != 0)
        die("bad smf", argv[2]);
    load_driver(SWMIDI_SYS);
    driver_start();
    smf_set_loop((int32_t) to_u32(argv[3]));

    int fd = sys_open(argv[5], 01 | 0100 | 01000); /* WRONLY|CREAT|TRUNC */
    if (fd < 0)
        die("could not open", argv[5]);
    wav_header(fd, 0);
    static int16_t buf[ORACLE_MIX_FRAMES * 2];
    uint32_t total = 0;
    for (;;)
    {
        uint32_t n = smf_render(buf, ORACLE_MIX_FRAMES);
        if (sys_write(fd, buf, (int32_t) (n * 4)) != (int32_t) (n * 4))
            die("short write", argv[5]);
        total += n;
        if (n == 0 || smf_is_finished() || total >= max_frames)
            break;
    }
    wav_header(fd, total);
    sys_close(fd);
    return 0;
}

/* Linux i386 entry: argc at (%esp), argv after it */
__attribute__((noreturn, used)) void start_c(int32_t *sp)
{
    sys_exit(oracle_main(sp[0], (char **) (sp + 1)));
}
__asm__(".text\n.globl _start\n_start:\n"
        "  movl %esp, %eax\n  andl $-16, %esp\n  subl $12, %esp\n  pushl %eax\n"
        "  call start_c\n");
