#ifndef GTE_H_
#define GTE_H_
#include <stdint.h>

// Geometry Transformation Engine (GTE), the PlayStation's coprocessor 2. It is
// a fixed-point vector and matrix unit the CPU drives over COP2 instructions:
// the move ops (MFC2/CFC2/MTC2/CTC2) and load/store ops (LWC2/SWC2) shuttle its
// registers, and the command ops run perspective transforms, lighting, and
// depth averaging. This header exposes the register file and the accessors the
// CPU dispatch uses; the arithmetic ops live in gte.c.
//
// Registers come in two banks of 32, addressed by the move ops. The data bank
// (cop2d, MFC2/MTC2) holds the working vectors, colour and screen FIFOs, and
// the MAC/IR accumulators. The control bank (cop2c, CFC2/CTC2) holds the
// rotation and light matrices, the translation and colour vectors, the screen
// projection parameters, and the FLAG error register. The register layout and
// the read-back quirks follow nocash psx-spx.

typedef struct {
    // ---- data registers (cop2d 0..31) ----
    int16_t  v[3][3];     // V0..V2, each as x,y,z (VXY0/VZ0, VXY1/VZ1, VXY2/VZ2)
    uint8_t  rgbc[4];     // RGBC: r, g, b, code
    uint16_t otz;         // OTZ: average Z, unsigned
    int16_t  ir[4];       // IR0..IR3: the interpolation and vector accumulators
    int16_t  sxy[3][2];   // SXY0..2 screen-coordinate FIFO, each as x,y
    uint16_t sz[4];       // SZ0..3 screen-Z FIFO, unsigned
    uint8_t  rgb[3][4];   // RGB0..2 colour FIFO, each r, g, b, code
    uint32_t res1;        // RES1: prohibited register
    int32_t  mac[4];      // MAC0..3: the wide multiply accumulators
    uint32_t lzcs;        // LZCS: leading-zero-count source (as written)
    int32_t  lzcr;        // LZCR: leading-zero/one count of LZCS

    // ---- control registers (cop2c 0..31) ----
    int16_t  rt[3][3];    // RT: rotation matrix, row-major
    int32_t  tr[3];       // TR: translation vector
    int16_t  llm[3][3];   // LLM: light source direction matrix
    int32_t  bk[3];       // BK: background colour
    int16_t  lcm[3][3];   // LCM: light colour matrix
    int32_t  fc[3];       // FC: far colour
    int32_t  ofx, ofy;    // screen offset X, Y (1/16 fixed point)
    uint16_t h;           // H: projection plane distance
    int16_t  dqa;         // DQA: depth-cue scale
    int32_t  dqb;         // DQB: depth-cue offset
    int16_t  zsf3, zsf4;  // ZSF3/ZSF4: Z-average scale factors
    uint32_t flag;        // FLAG: overflow and saturation error register
} GTE;

extern GTE gte;

void gte_reset(void);

// Move-op accessors. read_data/read_control return the 32-bit value a CFC2/MFC2
// would load (with the hardware read-back quirks); write_data/write_control
// apply a CTC2/MTC2 store (with the FIFO pushes and derived-register quirks).
uint32_t gte_read_data(uint32_t reg);
void     gte_write_data(uint32_t reg, uint32_t value);
uint32_t gte_read_control(uint32_t reg);
void     gte_write_control(uint32_t reg, uint32_t value);

// Run a GTE command (the low 25 bits of a COP2 command instruction).
void gte_execute(uint32_t instruction);

#endif
