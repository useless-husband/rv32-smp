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
`define WB_FPU    3'd5   // integer result of an F/D instruction (compare, FCVT.W, FMV.X.W, FCLASS)

// FPU operations (fpu.sv).  14 and above are combinational (one cycle in EX).
`define FOP_ADD    5'd0
`define FOP_SUB    5'd1
`define FOP_MUL    5'd2
`define FOP_MADD   5'd3
`define FOP_MSUB   5'd4
`define FOP_NMSUB  5'd5
`define FOP_NMADD  5'd6
`define FOP_DIV    5'd7
`define FOP_SQRT   5'd8
`define FOP_F2F    5'd9    // FCVT.S.D / FCVT.D.S
`define FOP_I2F    5'd10   // FCVT.fmt.W
`define FOP_IU2F   5'd11   // FCVT.fmt.WU
`define FOP_F2I    5'd12   // FCVT.W.fmt
`define FOP_F2IU   5'd13   // FCVT.WU.fmt
`define FOP_SGNJ   5'd14
`define FOP_SGNJN  5'd15
`define FOP_SGNJX  5'd16
`define FOP_MIN    5'd17
`define FOP_MAX    5'd18
`define FOP_EQ     5'd19
`define FOP_LT     5'd20
`define FOP_LE     5'd21
`define FOP_CLASS  5'd22
`define FOP_MVXW   5'd23   // FMV.X.W
`define FOP_MVWX   5'd24   // FMV.W.X

// rounding modes (frm) and exception flag bits (fflags)
`define RM_RNE 3'd0
`define RM_RTZ 3'd1
`define RM_RDN 3'd2
`define RM_RUP 3'd3
`define RM_RMM 3'd4
`define FF_NX 0
`define FF_UF 1
`define FF_OF 2
`define FF_DZ 3
`define FF_NV 4

// exception causes (mcause)
`define CAUSE_MISALIGNED_FETCH 32'd0
`define CAUSE_ILLEGAL          32'd2
`define CAUSE_BREAKPOINT       32'd3
`define CAUSE_MISALIGNED_LOAD  32'd4
`define CAUSE_MISALIGNED_STORE 32'd6
`define CAUSE_ECALL_M          32'd11

// platform (same numbers as model/rv_platform.h)
`define RESET_PC     32'h8000_0000
`define RAM_BASE     32'h8000_0000
`define RAM_BYTES    32'h0010_0000
`define MMIO_CONSOLE 32'h1000_0000
`define MMIO_EXIT    32'h1000_0004

// performance events (mhpmcounter3 + index), same order as RV_HPM_NAMES
`define NUM_EVENTS        10
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

`endif
