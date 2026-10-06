// Branch predictor for the pipelined core: a direct-mapped branch target
// buffer (BTB) plus a table of 2-bit saturating counters (BHT).
//
// Lookup (IF, combinational): if the BTB holds the fetch PC, an unconditional
// jump is predicted taken to the stored target and a conditional branch is
// predicted taken when its counter is 2 or 3.  Otherwise: not taken.
// Update (EX, when a branch or jump resolves): counters move toward the
// outcome; taken branches and all jumps (re)write their BTB entry.
// Returns (JALR with rs1 = ra/t0 and rd not a link register) are predicted
// from a return-address stack (RAS) of RAS_DEPTH entries: calls (JAL/JALR
// writing ra/t0) push pc+4 and returns pop, both when they resolve in EX, so
// the stack only ever holds non-speculative entries.  RAS_DEPTH=0 removes it
// (returns then use the BTB's last target).
// ENABLE=0 turns it into "always predict not taken", for comparison.
module bpred #(
    parameter int BTB_ENTRIES = 128,   // power of two
    parameter int BHT_ENTRIES = 256,  // power of two
    parameter int RAS_DEPTH = 8,      // 0 or a power of two
    parameter bit ENABLE = 1'b1
) (
    input  logic        clk,
    input  logic        rst,
    input  logic [31:0] pc,
    output logic        pred_taken,
    output logic [31:0] pred_target,
    input  logic        upd_valid,
    input  logic        upd_branch,    // conditional branch
    input  logic        upd_jump,      // JAL or JALR
    input  logic        upd_call,      // JAL/JALR that writes ra or t0
    input  logic        upd_ret,       // JALR from ra or t0 that is not a call
    input  logic [31:0] upd_pc,
    input  logic        upd_taken,
    input  logic [31:0] upd_target
);
    localparam int BI = $clog2(BTB_ENTRIES);
    localparam int HI = $clog2(BHT_ENTRIES);
    localparam int TW = 30 - BI;

    logic [BTB_ENTRIES-1:0]      btb_valid;   // packed so reset is one assignment
    logic          btb_jump  [0:BTB_ENTRIES-1];
    logic          btb_ret   [0:BTB_ENTRIES-1];
    logic [TW-1:0] btb_tag   [0:BTB_ENTRIES-1];
    logic [29:0]   btb_tgt   [0:BTB_ENTRIES-1];
    logic [2*BHT_ENTRIES-1:0]    bht;         // 2-bit counter i at bht[2*i +: 2]; kept flat
                                              // because the old tools in CI (Verilator 5.020,
                                              // Yosys 0.33) reject the array forms

    logic [BI-1:0] li, ui;
    logic [HI-1:0] lh, uh;
    logic          hit;

    assign li = pc[2 +: BI];
    assign lh = pc[2 +: HI];
    assign ui = upd_pc[2 +: BI];
    assign uh = upd_pc[2 +: HI];

    assign hit = btb_valid[li] && btb_tag[li] == pc[31:2+BI];
    logic [1:0] bht_l;                // read the counter first: Icarus cannot
    assign bht_l = bht[2*lh +: 2];    // bit-select a variable-indexed part-select
    assign pred_taken = ENABLE && hit && (btb_jump[li] || bht_l[1]);

    // return-address stack
    localparam int RW = (RAS_DEPTH > 1) ? $clog2(RAS_DEPTH) : 1;
    logic [29:0]   ras [0:(RAS_DEPTH > 0 ? RAS_DEPTH : 1)-1];
    logic [RW-1:0] ras_top;           // index of the newest entry
    logic [RW:0]   ras_count;
    logic          use_ras;
    localparam logic [RW:0] RAS_FULL = RAS_DEPTH[RW:0];

    assign use_ras = (RAS_DEPTH > 0) && btb_ret[li] && ras_count != '0;
    assign pred_target = use_ras ? {ras[ras_top], 2'b00} : {btb_tgt[li], 2'b00};

    always_ff @(posedge clk) begin
        if (rst) begin
            ras_top <= '0;
            ras_count <= '0;
        end else if (RAS_DEPTH > 0 && upd_valid) begin
            if (upd_call) begin
                ras[ras_top + 1'b1] <= upd_pc[31:2] + 30'd1;
                ras_top <= ras_top + 1'b1;
                if (ras_count != RAS_FULL) ras_count <= ras_count + 1'b1;
            end else if (upd_ret && ras_count != '0) begin
                ras_top <= ras_top - 1'b1;
                ras_count <= ras_count - 1'b1;
            end
        end
    end

    always_ff @(posedge clk) begin
        if (rst) begin
            btb_valid <= '0;
            bht <= {BHT_ENTRIES{2'b01}};  // weakly not taken
        end else if (upd_valid) begin
            if (upd_branch) begin
                if (upd_taken && bht[2*uh +: 2] != 2'b11) bht[2*uh +: 2] <= bht[2*uh +: 2] + 2'b01;
                if (!upd_taken && bht[2*uh +: 2] != 2'b00) bht[2*uh +: 2] <= bht[2*uh +: 2] - 2'b01;
            end
            if (upd_taken && (upd_branch || upd_jump)) begin
                btb_valid[ui] <= 1'b1;
                btb_jump[ui] <= upd_jump;
                btb_ret[ui] <= upd_ret && !upd_call;
                btb_tag[ui] <= upd_pc[31:2+BI];
                btb_tgt[ui] <= upd_target[31:2];
            end
        end
    end

    logic unused;
    assign unused = &{1'b0, pc[1:0], upd_pc[1:0], upd_target[1:0], bht_l[0]};
endmodule
