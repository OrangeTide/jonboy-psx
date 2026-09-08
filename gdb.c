#include "gdb.h"
#include "cpu.h"
#include "mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

// The GDB target we present: 72 registers in the standard MIPS o32 layout,
// matching the target description below. 0-31 are the GPRs, then status, lo, hi,
// badvaddr, cause, pc, then 32 FPU registers plus fcsr and fir (all reported as
// zero, since the R3000 in this emulator has no FPU).
#define GDB_NUM_REGS 72

#define GDB_MAX_BREAKPOINTS 64

static int gdb_listen_fd = -1;
static int gdb_conn_fd = -1;

static uint32_t gdb_breakpoints[GDB_MAX_BREAKPOINTS];
static int gdb_num_breakpoints;

#define GDB_MAX_WATCHPOINTS 32

// A watchpoint. type is the gdb Z kind: 2 = write, 3 = read, 4 = access.
static struct {
    uint32_t addr;
    uint32_t len;
    int type;
} gdb_watch[GDB_MAX_WATCHPOINTS];
static int gdb_num_watch;

// Set by the memory hook when an executing instruction touches a watched range.
// Read (and cleared) by the step loop, which runs on the same thread, so no
// synchronization is needed.
static int gdb_watch_hit;
static uint32_t gdb_watch_hit_addr;
static int gdb_watch_hit_type;

static const char gdb_hexchars[] = "0123456789abcdef";

static int
gdb_from_hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Parse a run of hex digits, advancing *s past them.
static uint32_t
gdb_parse_hex(const char **s)
{
    uint32_t v = 0;
    int d;
    while ((d = gdb_from_hex(**s)) >= 0) {
        v = (v << 4) | (uint32_t)d;
        (*s)++;
    }
    return v;
}

// Append a 32-bit register as 8 hex chars in target (little-endian) byte order.
static void
gdb_append_reg(char *out, uint32_t v)
{
    for (int b = 0; b < 4; b++) {
        uint8_t byte = (v >> (b * 8)) & 0xFF;
        *out++ = gdb_hexchars[byte >> 4];
        *out++ = gdb_hexchars[byte & 0xF];
    }
}

static uint32_t
gdb_get_reg(int n)
{
    if (n >= 0 && n < 32) {
        return cpu.regs[n];
    }
    switch (n) {
    case 32: return cpu.cp0_regs[CP0_REG_SR];
    case 33: return cpu.lo;
    case 34: return cpu.hi;
    case 35: return cpu.cp0_regs[CP0_REG_BADVADDR];
    case 36: return cpu.cp0_regs[CP0_REG_CAUSE];
    case 37: return cpu.PC;
    default: return 0; // FPU registers: none on this CPU
    }
}

static void
gdb_set_reg(int n, uint32_t v)
{
    if (n == 0) {
        return; // r0 is hardwired to zero
    }
    if (n < 32) {
        cpu.regs[n] = v;
        return;
    }
    switch (n) {
    case 32: cpu.cp0_regs[CP0_REG_SR] = v; break;
    case 33: cpu.lo = v; break;
    case 34: cpu.hi = v; break;
    case 35: cpu.cp0_regs[CP0_REG_BADVADDR] = v; break;
    case 36: cpu.cp0_regs[CP0_REG_CAUSE] = v; break;
    case 37: cpu.PC = v; cpu.next_pc = v + 4; break;
    default: break;
    }
}

// The target description gdb reads over qXfer:features:read. Declaring the
// registers explicitly frees us from gdb's built-in guesses about the layout.
static const char *
gdb_target_xml(void)
{
    static char xml[8192];
    if (xml[0]) {
        return xml;
    }

    int p = 0;
    p += snprintf(xml + p, sizeof xml - p,
                  "<?xml version=\"1.0\"?>\n"
                  "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
                  "<target version=\"1.0\">\n"
                  "<architecture>mips</architecture>\n"
                  "<feature name=\"org.gnu.gdb.mips.cpu\">\n");
    for (int i = 0; i < 32; i++) {
        p += snprintf(xml + p, sizeof xml - p,
                      "<reg name=\"r%d\" bitsize=\"32\" regnum=\"%d\"/>\n", i, i);
    }
    p += snprintf(xml + p, sizeof xml - p,
                  "<reg name=\"lo\" bitsize=\"32\" regnum=\"33\"/>\n"
                  "<reg name=\"hi\" bitsize=\"32\" regnum=\"34\"/>\n"
                  "<reg name=\"pc\" bitsize=\"32\" regnum=\"37\"/>\n"
                  "</feature>\n"
                  "<feature name=\"org.gnu.gdb.mips.cp0\">\n"
                  "<reg name=\"status\" bitsize=\"32\" regnum=\"32\"/>\n"
                  "<reg name=\"badvaddr\" bitsize=\"32\" regnum=\"35\"/>\n"
                  "<reg name=\"cause\" bitsize=\"32\" regnum=\"36\"/>\n"
                  "</feature>\n"
                  "<feature name=\"org.gnu.gdb.mips.fpu\">\n");
    for (int i = 0; i < 32; i++) {
        p += snprintf(xml + p, sizeof xml - p,
                      "<reg name=\"f%d\" bitsize=\"32\" regnum=\"%d\" group=\"float\"/>\n",
                      i, 38 + i);
    }
    p += snprintf(xml + p, sizeof xml - p,
                  "<reg name=\"fcsr\" bitsize=\"32\" regnum=\"70\" group=\"float\"/>\n"
                  "<reg name=\"fir\" bitsize=\"32\" regnum=\"71\" group=\"float\"/>\n"
                  "</feature>\n"
                  "</target>\n");
    return xml;
}

// Send one RSP packet ($<data>#<checksum>) and consume gdb's +/- acknowledge.
// Our replies only ever contain hex, plain ASCII, or XML, none of which needs
// the RSP escaping for } # $ *, so the data is sent verbatim.
static void
gdb_send(const char *data)
{
    size_t len = strlen(data);
    unsigned char sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += (unsigned char)data[i];
    }

    char *pkt = malloc(len + 5);
    size_t p = 0;
    pkt[p++] = '$';
    memcpy(pkt + p, data, len);
    p += len;
    pkt[p++] = '#';
    pkt[p++] = gdb_hexchars[sum >> 4];
    pkt[p++] = gdb_hexchars[sum & 0xF];

    (void)!write(gdb_conn_fd, pkt, p);
    free(pkt);

    char ack;
    (void)!read(gdb_conn_fd, &ack, 1); // consume '+' (retransmit not modeled)
}

// Read one RSP packet body into buf. Returns its length, 0 for a stray interrupt
// or empty read, or -1 if the connection closed.
static int
gdb_recv(char *buf, int max)
{
    char c;

    // Skip to the packet start, ignoring acks. A lone 0x03 here is a stray
    // interrupt (we handle interrupts inside continue), so drop it.
    for (;;) {
        if (read(gdb_conn_fd, &c, 1) != 1) {
            return -1;
        }
        if (c == '$') {
            break;
        }
        if (c == 0x03) {
            return 0;
        }
    }

    int len = 0;
    for (;;) {
        if (read(gdb_conn_fd, &c, 1) != 1) {
            return -1;
        }
        if (c == '#') {
            break;
        }
        if (len < max - 1) {
            buf[len++] = c;
        }
    }
    buf[len] = 0;

    char ck[2];
    if (read(gdb_conn_fd, &ck[0], 1) != 1 || read(gdb_conn_fd, &ck[1], 1) != 1) {
        return -1;
    }

    char ack = '+';
    (void)!write(gdb_conn_fd, &ack, 1);
    return len;
}

static int
gdb_is_breakpoint(uint32_t pc)
{
    for (int i = 0; i < gdb_num_breakpoints; i++) {
        if (gdb_breakpoints[i] == pc) {
            return 1;
        }
    }
    return 0;
}

static void
gdb_add_breakpoint(uint32_t addr)
{
    if (gdb_is_breakpoint(addr) || gdb_num_breakpoints >= GDB_MAX_BREAKPOINTS) {
        return;
    }
    gdb_breakpoints[gdb_num_breakpoints++] = addr;
}

static void
gdb_remove_breakpoint(uint32_t addr)
{
    for (int i = 0; i < gdb_num_breakpoints; i++) {
        if (gdb_breakpoints[i] == addr) {
            gdb_breakpoints[i] = gdb_breakpoints[--gdb_num_breakpoints];
            return;
        }
    }
}

// Memory hook (installed while any watchpoint exists): flag a hit when an access
// of the given kind falls in a watched range. type match: 2 = write only,
// 3 = read only, 4 = read or write.
static void
gdb_watch_check(uint32_t addr, int size, int is_write)
{
    for (int i = 0; i < gdb_num_watch; i++) {
        uint32_t a = gdb_watch[i].addr;
        uint32_t l = gdb_watch[i].len;
        if (addr < a + l && a < addr + (uint32_t)size) {
            int t = gdb_watch[i].type;
            if ((t == 2 && is_write) || (t == 3 && !is_write) || t == 4) {
                gdb_watch_hit = 1;
                gdb_watch_hit_addr = a;
                gdb_watch_hit_type = t;
                return;
            }
        }
    }
}

static void
gdb_add_watchpoint(uint32_t addr, uint32_t len, int type)
{
    if (gdb_num_watch >= GDB_MAX_WATCHPOINTS) {
        return;
    }
    gdb_watch[gdb_num_watch].addr = addr;
    gdb_watch[gdb_num_watch].len = len ? len : 1;
    gdb_watch[gdb_num_watch].type = type;
    gdb_num_watch++;
    mem_watch_hook = gdb_watch_check; // arm the per-access hook
}

static void
gdb_remove_watchpoint(uint32_t addr, uint32_t len, int type)
{
    for (int i = 0; i < gdb_num_watch; i++) {
        if (gdb_watch[i].addr == addr && gdb_watch[i].type == type &&
            gdb_watch[i].len == (len ? len : 1)) {
            gdb_watch[i] = gdb_watch[--gdb_num_watch];
            break;
        }
    }
    if (gdb_num_watch == 0) {
        mem_watch_hook = NULL; // no overhead once the last watchpoint is gone
    }
}

// Non-blocking check for gdb's Ctrl-C interrupt (a bare 0x03 byte).
static int
gdb_interrupted(void)
{
    char c;
    ssize_t r = recv(gdb_conn_fd, &c, 1, MSG_DONTWAIT);
    return r == 1 && c == 0x03;
}

// Signal numbers reported in stop replies.
#define GDB_SIGINT  2
#define GDB_SIGTRAP 5

// Run until a breakpoint, a halt, or a gdb interrupt. Returns the stop signal.
static int
gdb_continue(void (*step_hook)(void))
{
    unsigned poll = 0;
    for (;;) {
        if (cpu.halted) {
            return GDB_SIGTRAP;
        }
        if (step_hook) {
            step_hook();
        }
        gdb_watch_hit = 0; // count only this instruction's own accesses
        step_cpu();

        // A watched load or store during this instruction stops here.
        if (gdb_watch_hit) {
            return GDB_SIGTRAP;
        }
        // cpu.PC is now the next instruction to run; stop before executing a
        // breakpoint so gdb sees the machine halted at that address.
        if (gdb_is_breakpoint(cpu.PC)) {
            return GDB_SIGTRAP;
        }
        if ((++poll & 0xFFFF) == 0 && gdb_interrupted()) {
            return GDB_SIGINT;
        }
    }
}

static int
gdb_step(void (*step_hook)(void))
{
    if (!cpu.halted) {
        if (step_hook) {
            step_hook();
        }
        gdb_watch_hit = 0;
        step_cpu();
    }
    return GDB_SIGTRAP;
}

// Build the stop reply: a plain S packet, or a T packet naming the watchpoint
// when the stop was a watched access, so gdb reports the right watchpoint.
static void
gdb_send_stop(int sig)
{
    char r[64];
    if (gdb_watch_hit) {
        const char *kind = gdb_watch_hit_type == 2 ? "watch"
                           : gdb_watch_hit_type == 3 ? "rwatch"
                           : "awatch";
        snprintf(r, sizeof r, "T%02x%s:%x;", GDB_SIGTRAP, kind, gdb_watch_hit_addr);
    } else {
        snprintf(r, sizeof r, "S%02x", sig);
    }
    gdb_send(r);
}

// Answer a qXfer:features:read:target.xml:OFF,LEN request with the right slice.
static void
gdb_qxfer(const char *args)
{
    const char *s = args;
    uint32_t off = gdb_parse_hex(&s);
    if (*s == ',') {
        s++;
    }
    uint32_t len = gdb_parse_hex(&s);

    const char *xml = gdb_target_xml();
    uint32_t xmllen = (uint32_t)strlen(xml);
    if (off >= xmllen) {
        gdb_send("l");
        return;
    }

    uint32_t chunk = len;
    if (off + chunk > xmllen) {
        chunk = xmllen - off;
    }

    char *resp = malloc(chunk + 2);
    resp[0] = (off + chunk >= xmllen) ? 'l' : 'm';
    memcpy(resp + 1, xml + off, chunk);
    resp[chunk + 1] = 0;
    gdb_send(resp);
    free(resp);
}

// Handle one command. Returns 1 when gdb detaches or kills, else 0.
static int
gdb_handle(const char *buf, void (*step_hook)(void))
{
    switch (buf[0]) {
    case '?':
        gdb_send("S05");
        break;

    case 'g': {
        char r[GDB_NUM_REGS * 8 + 1];
        char *p = r;
        for (int i = 0; i < GDB_NUM_REGS; i++) {
            gdb_append_reg(p, gdb_get_reg(i));
            p += 8;
        }
        *p = 0;
        gdb_send(r);
        break;
    }

    case 'G': {
        const char *s = buf + 1;
        for (int i = 0; i < GDB_NUM_REGS && strlen(s) >= 8; i++) {
            uint32_t v = 0;
            for (int b = 0; b < 4; b++) {
                v |= (uint32_t)((gdb_from_hex(s[b * 2]) << 4) |
                                gdb_from_hex(s[b * 2 + 1])) << (b * 8);
            }
            gdb_set_reg(i, v);
            s += 8;
        }
        gdb_send("OK");
        break;
    }

    case 'p': {
        const char *s = buf + 1;
        uint32_t n = gdb_parse_hex(&s);
        char r[9];
        gdb_append_reg(r, n < GDB_NUM_REGS ? gdb_get_reg((int)n) : 0);
        r[8] = 0;
        gdb_send(r);
        break;
    }

    case 'P': {
        const char *s = buf + 1;
        uint32_t n = gdb_parse_hex(&s);
        if (*s == '=') {
            s++;
        }
        uint32_t v = 0;
        for (int b = 0; b < 4 && gdb_from_hex(s[0]) >= 0; b++) {
            v |= (uint32_t)((gdb_from_hex(s[0]) << 4) | gdb_from_hex(s[1])) << (b * 8);
            s += 2;
        }
        if (n < GDB_NUM_REGS) {
            gdb_set_reg((int)n, v);
        }
        gdb_send("OK");
        break;
    }

    case 'm': {
        const char *s = buf + 1;
        uint32_t addr = gdb_parse_hex(&s);
        if (*s == ',') {
            s++;
        }
        uint32_t len = gdb_parse_hex(&s);
        if (len > 4096) {
            len = 4096;
        }
        char *resp = malloc(len * 2 + 1);
        for (uint32_t i = 0; i < len; i++) {
            uint8_t b = (uint8_t)memory_read_byte(addr + i);
            resp[i * 2] = gdb_hexchars[b >> 4];
            resp[i * 2 + 1] = gdb_hexchars[b & 0xF];
        }
        resp[len * 2] = 0;
        gdb_send(resp);
        free(resp);
        break;
    }

    case 'M': {
        const char *s = buf + 1;
        uint32_t addr = gdb_parse_hex(&s);
        if (*s == ',') {
            s++;
        }
        uint32_t len = gdb_parse_hex(&s);
        if (*s == ':') {
            s++;
        }
        for (uint32_t i = 0; i < len; i++) {
            int hi = gdb_from_hex(s[0]);
            int lo = gdb_from_hex(s[1]);
            if (hi < 0 || lo < 0) {
                break;
            }
            memory_write_byte(addr + i, (uint32_t)((hi << 4) | lo));
            s += 2;
        }
        gdb_send("OK");
        break;
    }

    case 'c': {
        const char *s = buf + 1;
        if (gdb_from_hex(*s) >= 0) {
            uint32_t a = gdb_parse_hex(&s);
            cpu.PC = a;
            cpu.next_pc = a + 4;
        }
        gdb_send_stop(gdb_continue(step_hook));
        break;
    }

    case 's': {
        const char *s = buf + 1;
        if (gdb_from_hex(*s) >= 0) {
            uint32_t a = gdb_parse_hex(&s);
            cpu.PC = a;
            cpu.next_pc = a + 4;
        }
        gdb_send_stop(gdb_step(step_hook));
        break;
    }

    case 'Z':
    case 'z': {
        // Z0 (software) and Z1 (hardware) breakpoints are both tracked as an
        // address the step loop watches. Z2/Z3/Z4 are write/read/access
        // watchpoints, with the length in the kind field ("Z2,addr,len").
        char kind = buf[1];
        if (kind == '0' || kind == '1') {
            const char *s = buf + 2;
            if (*s == ',') {
                s++;
            }
            uint32_t addr = gdb_parse_hex(&s);
            if (buf[0] == 'Z') {
                gdb_add_breakpoint(addr);
            } else {
                gdb_remove_breakpoint(addr);
            }
            gdb_send("OK");
        } else if (kind == '2' || kind == '3' || kind == '4') {
            const char *s = buf + 2;
            if (*s == ',') {
                s++;
            }
            uint32_t addr = gdb_parse_hex(&s);
            uint32_t len = 1;
            if (*s == ',') {
                s++;
                len = gdb_parse_hex(&s);
            }
            if (buf[0] == 'Z') {
                gdb_add_watchpoint(addr, len, kind - '0');
            } else {
                gdb_remove_watchpoint(addr, len, kind - '0');
            }
            gdb_send("OK");
        } else {
            gdb_send("");
        }
        break;
    }

    case 'q':
        if (strncmp(buf, "qSupported", 10) == 0) {
            gdb_send("PacketSize=4096;qXfer:features:read+");
        } else if (strncmp(buf, "qXfer:features:read:target.xml:", 31) == 0) {
            gdb_qxfer(buf + 31);
        } else if (strncmp(buf, "qAttached", 9) == 0) {
            gdb_send("1");
        } else if (strncmp(buf, "qfThreadInfo", 12) == 0) {
            gdb_send("m1");
        } else if (strncmp(buf, "qsThreadInfo", 12) == 0) {
            gdb_send("l");
        } else {
            gdb_send("");
        }
        break;

    case 'H': // set thread for subsequent ops: single-threaded, always OK
        gdb_send("OK");
        break;

    case '!': // enable extended mode
        gdb_send("OK");
        break;

    case 'D': // detach
        gdb_send("OK");
        return 1;

    case 'k': // kill
        return 1;

    default: // unsupported: empty reply
        gdb_send("");
        break;
    }
    return 0;
}

int
gdb_start(int port)
{
    gdb_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (gdb_listen_fd < 0) {
        perror("gdb: socket");
        return -1;
    }

    int one = 1;
    setsockopt(gdb_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only, not the network
    addr.sin_port = htons((uint16_t)port);

    if (bind(gdb_listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("gdb: bind");
        return -1;
    }
    if (listen(gdb_listen_fd, 1) < 0) {
        perror("gdb: listen");
        return -1;
    }

    printf("gdb: waiting for a connection on 127.0.0.1:%d\n", port);
    gdb_conn_fd = accept(gdb_listen_fd, NULL, NULL);
    if (gdb_conn_fd < 0) {
        perror("gdb: accept");
        return -1;
    }

    int nodelay = 1;
    setsockopt(gdb_conn_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
    printf("gdb: connected\n");
    return 0;
}

void
gdb_run(void (*step_hook)(void))
{
    char buf[8192];
    for (;;) {
        int n = gdb_recv(buf, sizeof buf);
        if (n < 0) {
            break; // connection closed
        }
        if (n == 0) {
            continue; // stray interrupt or empty
        }
        if (gdb_handle(buf, step_hook)) {
            break; // detached or killed
        }
    }
}

void
gdb_stop(void)
{
    mem_watch_hook = NULL; // disarm the per-access hook
    if (gdb_conn_fd >= 0) {
        close(gdb_conn_fd);
        gdb_conn_fd = -1;
    }
    if (gdb_listen_fd >= 0) {
        close(gdb_listen_fd);
        gdb_listen_fd = -1;
    }
}
