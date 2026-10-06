// Shared constants.  Kept as `define macros (no SystemVerilog packages) so
// the same sources work with Verilator 5.020+, Icarus Verilog 12+ and Yosys.
`ifndef RV_DEFS_SVH
`define RV_DEFS_SVH

// ALU operations
`define ALU_ADD   4'd0
`define ALU_SUB   4'd1
`define ALU_SLL   4'd2
`define ALU_SLT   4'd3
`define ALU_SLTU  4'd4
`define ALU_XOR   4'd5
`define ALU_SRL   4'd6
`define ALU_SRA   4'd7
`define ALU_OR    4'd8
`define ALU_AND   4'd9

// ALU operand A
`define A_RS1     2'd0
`define A_PC      2'd1
`define A_ZERO    2'd2

// value written to rd
`define WB_ALU    3'd0
`define WB_MEM    3'd1
`define WB_PC4    3'd2
`define WB_CSR    3'd3
`define WB_MDU    3'd4

// A extension: funct5 of the AMO opcode
`define AMO_ADD   5'b00000
`define AMO_SWAP  5'b00001
`define AMO_LR    5'b00010
`define AMO_SC    5'b00011
`define AMO_XOR   5'b00100
`define AMO_OR    5'b01000
`define AMO_AND   5'b01100
`define AMO_MIN   5'b10000
`define AMO_MAX   5'b10100
`define AMO_MINU  5'b11000
`define AMO_MAXU  5'b11100

// Snooping bus commands (rtl/bus.sv).  See docs/DESIGN.md, section 3.
`define CMD_RD    3'd0   // read a line for a load (others keep S copies; requester gets E or S)
`define CMD_RDX   3'd1   // read a line for ownership (others invalidate; requester gets M)
`define CMD_UPGR  3'd2   // S -> M without data (others invalidate)
`define CMD_WB    3'd3   // write back a dirty line from the write-back buffer
`define CMD_IRD   3'd4   // instruction fetch: read a line, an M copy supplies it, no state changes
`define CMD_UNC   3'd5   // uncached I/O access (strobed write or word read), not snooped

// Deliberately broken protocol variants (parameter BUG of l1d), the "bug
// museum": each one is a classic coherence or atomics mistake.  0 = correct.
`define BUG_NONE              0
`define BUG_LOST_UPGRADE      1   // a pending BusUpgr is not turned into BusRdX after a remote invalidation
`define BUG_WB_NO_SNOOP       2   // the write-back buffer is not snooped
`define BUG_LRSC_NO_CLEAR     3   // a remote invalidation does not clear the LR reservation
`define BUG_E_IGNORES_SHARED  4   // a read miss installs E even when another cache has the line
`define BUG_LOCKOUT_FOREVER   5   // the LR lock-out window has no timeout

// MESI line states as stored and as reported to the harness
`define ST_I 2'd0
`define ST_S 2'd1
`define ST_E 2'd2
`define ST_M 2'd3

// exception causes (mcause)
`define CAUSE_MISALIGNED_FETCH 32'd0
`define CAUSE_ILLEGAL          32'd2
`define CAUSE_BREAKPOINT       32'd3
`define CAUSE_MISALIGNED_LOAD  32'd4
`define CAUSE_LOAD_FAULT       32'd5
`define CAUSE_MISALIGNED_STORE 32'd6
`define CAUSE_STORE_FAULT      32'd7
`define CAUSE_ECALL_M          32'd11

// platform (same numbers as model/rv_platform.h)
`define RESET_PC     32'h8000_0000
`define RAM_BASE     32'h8000_0000
`define RAM_BYTES    32'h0010_0000
`define MMIO_CONSOLE 32'h1000_0000
`define MMIO_EXIT    32'h1000_0004
`define MMIO_NHARTS  32'h1000_0008   // read: number of harts

// performance events (mhpmcounter3 + index), same order as RV_HPM_NAMES
`define NUM_EVENTS        16
`define EV_ICACHE_MISS    0
`define EV_DCACHE_ACCESS  1
`define EV_DCACHE_MISS    2
`define EV_DCACHE_WB      3
`define EV_BRANCH         4
`define EV_BRANCH_MISS    5
`define EV_JUMP           6
`define EV_JUMP_MISS      7
`define EV_LOAD_USE       8
`define EV_ICACHE_ACCESS  9
`define EV_BUS_RD         10   // read misses (BusRd issued)
`define EV_BUS_RDX        11   // write/atomic misses (BusRdX issued)
`define EV_BUS_UPGR       12   // upgrade misses (BusUpgr issued)
`define EV_SNOOP_INV      13   // a valid line here was invalidated by another hart's request
`define EV_SNOOP_SUPPLY   14   // this cache supplied a dirty line (from the array or the WB buffer)
`define EV_SC_FAIL        15   // SC.W that failed

`endif
