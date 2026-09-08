#ifndef GDB_H_
#define GDB_H_

// GDB Remote Serial Protocol server. It listens on a loopback TCP port, waits
// for gdb (mipsel-unknown-elf-gdb) to connect, and drives the CPU under gdb
// control: registers, memory, breakpoints, single step, and continue. It runs
// headless (no SDL) and reads/writes the machine through the same cpu/mem state
// the interpreter uses.
//
// step_hook, if non-NULL, runs before each executed instruction during continue
// and single step, so the loader and sideload boot services stay live under gdb.

// Listen on 127.0.0.1:port and block until gdb connects. Returns 0 on success.
int gdb_start(int port);

// Serve gdb commands until it detaches or disconnects.
void gdb_run(void (*step_hook)(void));

// Close the gdb connection and listening socket.
void gdb_stop(void);

#endif
