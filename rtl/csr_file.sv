// Machine-mode CSRs and performance counters.
//
// Implemented: mstatus (MIE, MPIE; MPP reads as M), misa (read-only value:
// RV32IMA), mie/mip (read zero: no interrupts), mtvec (direct mode),
// mscratch, mepc, mcause, mtval, mhartid (the HARTID parameter),
// mvendorid/marchid/mimpid/mconfigptr (zero), mcycle[h], minstret[h],
// mhpmcounter3..18[h] and their read-only user aliases cycle[h], instret[h],
// hpmcounter3..18[h].  Anything else is an illegal instruction, as is a
// write to a read-only CSR.  The list matches the golden model
// (model/rv_iss.c) exactly.
`include "rv_defs.svh"

module csr_file #(
    parameter int HARTID = 0
) (
    input  logic        clk,
    input  logic        rst,
    // CSR instruction
    input  logic [11:0] addr,
    input  logic [1:0]  op,          // funct3[1:0]: 01 write, 10 set, 11 clear
    input  logic [31:0] src,         // rs1 value or zero-extended uimm
    input  logic        writes,      // the instruction writes the CSR
    input  logic        we,          // ... and executes this cycle
    output logic [31:0] rdata,
    output logic        illegal,
    output logic        writes_instret, // a write to minstret[h] (replaces the increment)
    // traps and MRET
    input  logic        trap,
    input  logic [31:0] trap_pc,
    input  logic [31:0] trap_cause,
    input  logic [31:0] trap_tval,
    input  logic        mret,
    output logic [31:0] mtvec,
    output logic [31:0] mepc,
    // counting
    input  logic        instret_inc,
    input  logic [`NUM_EVENTS-1:0] events
);
    logic mie_bit, mpie_bit;
    logic [31:0] mscratch, mcause, mtval;
    logic [63:0] mcycle, minstret;
    logic [63:0] hpm [0:`NUM_EVENTS-1];

    logic [6:0]  lo;
    logic        hi, is_ctr, exists;
    logic [63:0] ctr;
    logic [31:0] wval;
    logic [4:0]  hpm_idx;

    localparam logic [6:0] HPM_END = 7'd3 + `NUM_EVENTS;  // one past the last mhpmcounter

    assign lo = addr[6:0];
    assign hi = addr[7];
    assign hpm_idx = lo[4:0] - 5'd3;   // < NUM_EVENTS = 16 whenever it is used
    logic unused_idx;
    assign unused_idx = hpm_idx[4];
    // counter CSRs: 0xB00-0xB1F/0xB80-0xB9F (machine) and 0xC00../0xC80.. (user, read-only)
    assign is_ctr = (addr[11:8] == 4'hB || addr[11:8] == 4'hC) &&
                    (lo == 7'd0 || lo == 7'd2 || (lo >= 7'd3 && lo < HPM_END));

    always_comb begin
        ctr = 64'd0;
        if (lo == 7'd0) ctr = mcycle;
        else if (lo == 7'd2) ctr = minstret;
        else if (lo >= 7'd3 && lo < HPM_END) ctr = hpm[hpm_idx[3:0]];

        exists = 1'b1;
        rdata = 32'd0;
        case (addr)
            12'h300: rdata = {19'd0, 2'b11, 3'd0, mpie_bit, 3'd0, mie_bit, 3'd0};
            12'h301: rdata = 32'h4000_1101;               // MXL=32, I, M, A
            12'h304, 12'h344: rdata = 32'd0;              // mie, mip
            12'h305: rdata = mtvec;
            12'h340: rdata = mscratch;
            12'h341: rdata = mepc;
            12'h342: rdata = mcause;
            12'h343: rdata = mtval;
            12'hF14: rdata = HARTID;
            12'hF11, 12'hF12, 12'hF13, 12'hF15: rdata = 32'd0;
            default: begin
                exists = is_ctr;
                rdata = hi ? ctr[63:32] : ctr[31:0];
            end
        endcase
        illegal = !exists || (writes && addr[11:10] == 2'b11);

        case (op)
            2'b01: wval = src;
            2'b10: wval = rdata | src;
            default: wval = rdata & ~src;
        endcase
    end

    logic do_write;
    assign do_write = we && writes && !illegal;
    assign writes_instret = do_write && addr[11:8] == 4'hB && lo == 7'd2;

    always_ff @(posedge clk) begin
        if (rst) begin
            mie_bit <= 1'b0;
            mpie_bit <= 1'b0;
            mtvec <= 32'd0;
            mscratch <= 32'd0;
            mepc <= 32'd0;
            mcause <= 32'd0;
            mtval <= 32'd0;
        end else if (trap) begin
            mepc <= trap_pc;
            mcause <= trap_cause;
            mtval <= trap_tval;
            mpie_bit <= mie_bit;
            mie_bit <= 1'b0;
        end else if (mret) begin
            mie_bit <= mpie_bit;
            mpie_bit <= 1'b1;
        end else if (do_write) begin
            case (addr)
                12'h300: begin mie_bit <= wval[3]; mpie_bit <= wval[7]; end
                12'h305: mtvec <= {wval[31:2], 2'b00};
                12'h340: mscratch <= wval;
                12'h341: mepc <= {wval[31:2], 2'b00};
                12'h342: mcause <= wval;
                12'h343: mtval <= wval;
                default: ;
            endcase
        end
    end

    // Counters.  A CSR write replaces that cycle's increment.
    logic wr_ctr;
    assign wr_ctr = do_write && addr[11:8] == 4'hB;

    always_ff @(posedge clk) begin
        if (rst) begin
            mcycle <= 64'd0;
            minstret <= 64'd0;
        end else begin
            if (wr_ctr && lo == 7'd0)
                mcycle <= hi ? {wval, mcycle[31:0]} : {mcycle[63:32], wval};
            else
                mcycle <= mcycle + 64'd1;
            if (wr_ctr && lo == 7'd2)
                minstret <= hi ? {wval, minstret[31:0]} : {minstret[63:32], wval};
            else if (instret_inc)
                minstret <= minstret + 64'd1;
        end
    end

    genvar g;
    generate
        for (g = 0; g < `NUM_EVENTS; g = g + 1) begin : g_hpm
            localparam logic [6:0] LO = g + 3;
            always_ff @(posedge clk) begin
                if (rst)
                    hpm[g] <= 64'd0;
                else if (wr_ctr && lo == LO)
                    hpm[g] <= hi ? {wval, hpm[g][31:0]} : {hpm[g][63:32], wval};
                else if (events[g])
                    hpm[g] <= hpm[g] + 64'd1;
            end
        end
    endgenerate
endmodule
