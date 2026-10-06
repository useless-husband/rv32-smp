// Core B: 5-stage pipelined RV32IM core (IF, ID, EX, MEM, WB) with full
// forwarding, a one-cycle load-use stall, branch resolution in EX with a
// BTB + 2-bit-counter predictor, an iterative divider, an I-cache and a
// write-back D-cache sharing one memory bus.  See docs/DESIGN.md.
//
// FPU = 1 adds the F and D extensions: 32 64-bit f registers read in ID
// with their own forwarding paths, a multi-cycle floating-point unit in EX
// (fpu.sv; the instruction waits there, like a divide) and FLD/FSD as two
// word accesses in MEM.  With FPU = 0 every F/D signal is constant and the
// core is the RV32IM one.
//
// Who may stall whom (each stage holds when a later one holds):
//   MEM holds   : D-cache not ready, or FENCE.I still flushing the caches,
//                 or the second word of an FLD/FSD is still to come
//   EX holds    : MEM holds, or a divide or FPU operation is still running
//   ID holds    : EX holds, or a load-use hazard, or a CSR instruction
//                 waiting for EX/MEM/WB to drain
//   IF holds    : ID holds, or the I-cache misses (ID then gets a bubble)
// Redirects (they discard every younger instruction):
//   from EX  : the next PC differs from the predicted one (mispredicted
//              branch/jump, trap, MRET)       -> IF and ID are flushed
//   from MEM : FENCE.I after the caches are done -> IF, ID and EX flushed
`include "rv_defs.svh"

module core_pipe #(
    parameter int  ICACHE_SETS = 256,   // 4 KiB direct mapped
    parameter int  DCACHE_SETS = 128,   // 4 KiB, 2 ways
    parameter int  BTB_ENTRIES = 128,
    parameter int  BHT_ENTRIES = 256,
    parameter int  RAS_DEPTH   = 8,
    parameter bit  BP_ENABLE   = 1'b1,
    parameter bit  FPU         = 1'b0    // 1: RV32IMFD
) (
    input  logic         clk,
    input  logic         rst,
    // memory bus (line based, see mem_arbiter.sv)
    output logic         bus_req,
    output logic         bus_we,
    output logic [31:0]  bus_addr,
    output logic [127:0] bus_wdata,
    output logic [15:0]  bus_wstrb,
    input  logic         bus_ack,
    input  logic [127:0] bus_rdata,
    // commit port
    output logic         commit_valid,
    output logic [31:0]  commit_pc,
    output logic [31:0]  commit_insn,
    output logic         commit_trap,
    output logic [31:0]  commit_cause,
    output logic         commit_rd_we,
    output logic [4:0]   commit_rd,
    output logic [31:0]  commit_rd_val,
    output logic         commit_mem_we,
    output logic [31:0]  commit_mem_addr,
    output logic [31:0]  commit_mem_wdata,
    output logic [3:0]   commit_mem_wmask,
    // ... F/D part (constant zero when FPU = 0)
    output logic         commit_frd_we,      // wrote f register commit_rd
    output logic [63:0]  commit_frd_val,
    output logic [4:0]   commit_fflags,      // exception flags raised
    output logic         commit_mem_dbl,     // FSD: commit_mem_wdata_hi goes to commit_mem_addr + 4
    output logic [31:0]  commit_mem_wdata_hi,
    output logic [`NUM_EVENTS-1:0] perf_events,
    // pipeline-viewer probes (simulation only; unused in synthesis)
    output logic [31:0]  dbg_f_pc,
    output logic [15:0]  dbg_f_seq,
    output logic         dbg_d_valid,
    output logic [15:0]  dbg_d_seq,
    output logic [31:0]  dbg_d_pc,
    output logic [31:0]  dbg_d_insn,
    output logic         dbg_e_valid,
    output logic [15:0]  dbg_e_seq,
    output logic         dbg_m_valid,
    output logic [15:0]  dbg_m_seq,
    output logic         dbg_w_valid,
    output logic [15:0]  dbg_w_seq,
    output logic [5:0]   dbg_why   // stall/flush reasons, see sim/pipeview.inc
);
    // ===================================================== control (global)
    logic m_stall, e_hold, e_busy, d_hold, d_hazard;
    logic e_fire, d_fire, f_fire, m_fire;
    logic redirect_ex, redirect_mem;
    logic [31:0] redirect_ex_pc;

    // ============================================================ IF stage
    logic [31:0] pc_f, pc_next, pc_f4, pred_next;
    logic [15:0] f_seq;
    logic        bp_taken, ic_hit;
    logic [31:0] bp_target, ic_insn;

    assign pc_f4 = pc_f + 32'd4;
    assign pred_next = bp_taken ? bp_target : pc_f4;
    assign f_fire = ic_hit && !d_hold && !redirect_ex && !redirect_mem;

    logic [31:0] m_pc;
    always_comb begin
        if (redirect_mem) pc_next = m_pc + 32'd4;
        else if (redirect_ex) pc_next = redirect_ex_pc;
        else if (f_fire) pc_next = pred_next;
        else pc_next = pc_f;
    end

    always_ff @(posedge clk) begin
        if (rst) begin
            pc_f <= `RESET_PC;
            f_seq <= 16'd0;
        end else begin
            pc_f <= pc_next;
            if (f_fire) f_seq <= f_seq + 16'd1;
        end
    end

    // ============================================================ ID stage
    logic        d_valid, d_pred_taken;
    logic [31:0] d_pc, d_insn, d_pred_next;
    logic [15:0] d_seq;

    always_ff @(posedge clk) begin
        if (rst || redirect_ex || redirect_mem) begin
            d_valid <= 1'b0;
        end else if (!d_hold) begin
            d_valid <= f_fire;
            d_pc <= pc_f;
            d_insn <= ic_insn;
            d_pred_next <= pred_next;
            d_pred_taken <= bp_taken;
            d_seq <= f_seq;
        end
    end

    logic [3:0]  dc_alu_op;
    logic [1:0]  dc_a_sel;
    logic        dc_b_imm;
    logic [31:0] dc_imm;
    logic [4:0]  dc_rd, dc_rs1, dc_rs2;
    logic        dc_uses_rs1, dc_uses_rs2, dc_rd_we;
    logic [2:0]  dc_wb_sel, dc_funct3;
    logic        dc_is_branch, dc_is_jal, dc_is_jalr, dc_is_load, dc_is_store, dc_is_mdu, dc_is_div;
    logic        dc_is_csr, dc_csr_writes, dc_is_ecall, dc_is_ebreak, dc_is_mret, dc_is_fencei, dc_illegal;
    logic [4:0]  dc_rs3, dc_fp_op;
    logic        dc_is_fp, dc_fp_unit, dc_fp_dbl, dc_fp_rm_dyn, dc_uses_frs1, dc_uses_frs2, dc_uses_frs3, dc_frd_we;

    decoder #(.FPU(FPU)) u_dec (
        .insn(d_insn), .alu_op(dc_alu_op), .a_sel(dc_a_sel), .b_imm(dc_b_imm), .imm(dc_imm),
        .rd(dc_rd), .rs1(dc_rs1), .rs2(dc_rs2), .uses_rs1(dc_uses_rs1), .uses_rs2(dc_uses_rs2),
        .rd_we(dc_rd_we), .wb_sel(dc_wb_sel), .funct3(dc_funct3), .is_branch(dc_is_branch),
        .is_jal(dc_is_jal), .is_jalr(dc_is_jalr), .is_load(dc_is_load), .is_store(dc_is_store),
        .is_mdu(dc_is_mdu), .is_div(dc_is_div), .is_csr(dc_is_csr), .csr_writes(dc_csr_writes),
        .is_ecall(dc_is_ecall), .is_ebreak(dc_is_ebreak), .is_mret(dc_is_mret),
        .is_fencei(dc_is_fencei), .illegal(dc_illegal),
        .rs3(dc_rs3), .is_fp(dc_is_fp), .fp_unit(dc_fp_unit), .fp_op(dc_fp_op), .fp_dbl(dc_fp_dbl),
        .fp_rm_dyn(dc_fp_rm_dyn), .uses_frs1(dc_uses_frs1), .uses_frs2(dc_uses_frs2),
        .uses_frs3(dc_uses_frs3), .frd_we(dc_frd_we));

    logic [31:0] rf_rd1, rf_rd2;
    logic        w_valid, w_rd_we;
    logic [4:0]  w_rd;
    logic [31:0] w_result;

    regfile #(.BYPASS(1'b1)) u_rf (
        .clk(clk), .ra1(dc_rs1), .ra2(dc_rs2), .rd1(rf_rd1), .rd2(rf_rd2),
        .we(w_valid && w_rd_we), .wa(w_rd), .wd(w_result));

    // floating-point registers (x and f registers are separate files: an
    // instruction's rs1/rs2/rd fields name one or the other, the decoder says which)
    logic [63:0] frf_rd1, frf_rd2, frf_rd3, w_fresult;
    logic        w_frd_we;

    generate
        if (FPU) begin : g_frf
            fp_regfile u_frf (
                .clk(clk), .ra1(dc_rs1), .ra2(dc_rs2), .ra3(dc_rs3), .rd1(frf_rd1), .rd2(frf_rd2),
                .rd3(frf_rd3), .we(w_valid && w_frd_we), .wa(w_rd), .wd(w_fresult));
        end else begin : g_no_frf
            assign frf_rd1 = 64'd0;
            assign frf_rd2 = 64'd0;
            assign frf_rd3 = 64'd0;
        end
    endgenerate

    // hazards detected in ID
    logic e_valid, e_is_load, e_rd_we, e_frd_we, m_valid;
    logic [4:0] e_rd;
    logic load_use, fload_use, serialize;

    assign load_use = e_valid && e_is_load && e_rd_we &&
                      ((dc_uses_rs1 && dc_rs1 == e_rd) || (dc_uses_rs2 && dc_rs2 == e_rd));
    // the same for an FLW/FLD followed by a reader of that f register
    assign fload_use = e_valid && e_is_load && e_frd_we &&
                       ((dc_uses_frs1 && dc_rs1 == e_rd) || (dc_uses_frs2 && dc_rs2 == e_rd) ||
                        (dc_uses_frs3 && dc_rs3 == e_rd));
    // CSR instructions run alone so counter reads see every older instruction retired
    // (and fflags/frm/mstatus.FS are settled before any younger F/D instruction reaches EX)
    assign serialize = dc_is_csr && (e_valid || m_valid || w_valid);
    assign d_hazard = d_valid && (load_use || fload_use || serialize);
    assign d_hold = e_hold || d_hazard;
    assign d_fire = d_valid && !d_hold && !redirect_ex && !redirect_mem;

    // ============================================================ EX stage
    logic [31:0] e_pc, e_insn, e_imm, e_rs1_val, e_rs2_val, e_pred_next;
    logic [3:0]  e_alu_op;
    logic [1:0]  e_a_sel;
    logic        e_b_imm;
    logic [4:0]  e_rs1, e_rs2;
    logic [2:0]  e_wb_sel, e_funct3;
    logic        e_is_branch, e_is_jal, e_is_jalr, e_is_store, e_is_mdu, e_is_div;
    logic        e_is_csr, e_csr_writes, e_is_ecall, e_is_ebreak, e_is_mret, e_is_fencei, e_illegal;
    logic [15:0] e_seq;
    logic [31:0] fwd1, fwd2;
    logic [4:0]  e_rs3, e_fp_op;
    logic        e_is_fp, e_fp_unit, e_fp_dbl, e_fp_rm_dyn;
    logic [63:0] e_frs1_val, e_frs2_val, e_frs3_val, ffwd1, ffwd2, ffwd3;

    always_ff @(posedge clk) begin
        if (rst || redirect_mem) begin
            e_valid <= 1'b0;
        end else if (!e_hold) begin
            e_valid <= d_fire;
            e_pc <= d_pc;
            e_insn <= d_insn;
            e_pred_next <= d_pred_next;
            e_seq <= d_seq;
            e_alu_op <= dc_alu_op;
            e_a_sel <= dc_a_sel;
            e_b_imm <= dc_b_imm;
            e_imm <= dc_imm;
            e_rd <= dc_rd;
            e_rs1 <= dc_rs1;
            e_rs2 <= dc_rs2;
            e_rd_we <= dc_rd_we;
            e_wb_sel <= dc_wb_sel;
            e_funct3 <= dc_funct3;
            e_is_branch <= dc_is_branch;
            e_is_jal <= dc_is_jal;
            e_is_jalr <= dc_is_jalr;
            e_is_load <= dc_is_load;
            e_is_store <= dc_is_store;
            e_is_mdu <= dc_is_mdu;
            e_is_div <= dc_is_div;
            e_is_csr <= dc_is_csr;
            e_csr_writes <= dc_csr_writes;
            e_is_ecall <= dc_is_ecall;
            e_is_ebreak <= dc_is_ebreak;
            e_is_mret <= dc_is_mret;
            e_is_fencei <= dc_is_fencei;
            e_illegal <= dc_illegal;
            e_rs1_val <= rf_rd1;
            e_rs2_val <= rf_rd2;
        end else begin
            // Held in EX: keep the forwarded operands.  The MEM/WB instructions
            // that supply them move on while EX waits.
            e_rs1_val <= fwd1;
            e_rs2_val <= fwd2;
        end
    end

    // forwarding: MEM stage (not loads: the load-use stall keeps those out), then WB
    logic        m_rd_we, m_is_load;
    logic [4:0]  m_rd;
    logic [31:0] m_result;

    always_comb begin
        if (m_valid && m_rd_we && !m_is_load && m_rd == e_rs1) fwd1 = m_result;
        else if (w_valid && w_rd_we && w_rd == e_rs1) fwd1 = w_result;
        else fwd1 = e_rs1_val;
        if (m_valid && m_rd_we && !m_is_load && m_rd == e_rs2) fwd2 = m_result;
        else if (w_valid && w_rd_we && w_rd == e_rs2) fwd2 = w_result;
        else fwd2 = e_rs2_val;
    end

    // F/D part of the ID/EX register, and the same two forwarding sources
    // for the three floating-point operands
    logic        m_frd_we;
    logic [63:0] m_fresult;

    generate
        if (FPU) begin : g_fp_ex
            always_ff @(posedge clk) begin
                if (!e_hold) begin
                    e_rs3 <= dc_rs3;
                    e_frd_we <= dc_frd_we;
                    e_is_fp <= dc_is_fp;
                    e_fp_unit <= dc_fp_unit;
                    e_fp_op <= dc_fp_op;
                    e_fp_dbl <= dc_fp_dbl;
                    e_fp_rm_dyn <= dc_fp_rm_dyn;
                    e_frs1_val <= frf_rd1;
                    e_frs2_val <= frf_rd2;
                    e_frs3_val <= frf_rd3;
                end else begin
                    e_frs1_val <= ffwd1;
                    e_frs2_val <= ffwd2;
                    e_frs3_val <= ffwd3;
                end
            end
        end else begin : g_no_fp_ex
            assign e_rs3 = 5'd0;
            assign e_frd_we = 1'b0;
            assign e_is_fp = 1'b0;
            assign e_fp_unit = 1'b0;
            assign e_fp_op = 5'd0;
            assign e_fp_dbl = 1'b0;
            assign e_fp_rm_dyn = 1'b0;
            assign e_frs1_val = 64'd0;
            assign e_frs2_val = 64'd0;
            assign e_frs3_val = 64'd0;
            logic unused_fp_id;
            assign unused_fp_id = &{1'b0, dc_rs3, dc_frd_we, dc_is_fp, dc_fp_unit, dc_fp_op, dc_fp_dbl,
                                    dc_fp_rm_dyn, frf_rd1, frf_rd2, frf_rd3};
        end
    endgenerate

    always_comb begin
        if (m_valid && m_frd_we && !m_is_load && m_rd == e_rs1) ffwd1 = m_fresult;
        else if (w_valid && w_frd_we && w_rd == e_rs1) ffwd1 = w_fresult;
        else ffwd1 = e_frs1_val;
        if (m_valid && m_frd_we && !m_is_load && m_rd == e_rs2) ffwd2 = m_fresult;
        else if (w_valid && w_frd_we && w_rd == e_rs2) ffwd2 = w_fresult;
        else ffwd2 = e_frs2_val;
        if (m_valid && m_frd_we && !m_is_load && m_rd == e_rs3) ffwd3 = m_fresult;
        else if (w_valid && w_frd_we && w_rd == e_rs3) ffwd3 = w_fresult;
        else ffwd3 = e_frs3_val;
    end

    // ALU, branch unit, multiplier, divider
    logic [31:0] alu_a, alu_b, alu_y, e_pc4, br_target, target, actual_next;
    logic        taken, jumps;

    assign alu_a = (e_a_sel == `A_PC) ? e_pc : (e_a_sel == `A_ZERO) ? 32'd0 : fwd1;
    assign alu_b = e_b_imm ? e_imm : fwd2;
    alu u_alu (.op(e_alu_op), .a(alu_a), .b(alu_b), .y(alu_y));

    always_comb begin
        case (e_funct3)
            3'b000: taken = (fwd1 == fwd2);
            3'b001: taken = (fwd1 != fwd2);
            3'b100: taken = ($signed(fwd1) < $signed(fwd2));
            3'b101: taken = ($signed(fwd1) >= $signed(fwd2));
            3'b110: taken = (fwd1 < fwd2);
            default: taken = (fwd1 >= fwd2);
        endcase
    end

    assign e_pc4 = e_pc + 32'd4;
    assign br_target = e_pc + e_imm;
    assign jumps = e_is_jal || e_is_jalr || (e_is_branch && taken);
    assign target = e_is_jalr ? {alu_y[31:1], 1'b0} : br_target;

    logic signed [32:0] mul_a, mul_b;
    logic signed [65:0] mul_p;
    logic [31:0] div_y;
    logic        div_done;

    assign mul_a = {(e_funct3[1:0] != 2'b11) & fwd1[31], fwd1};
    assign mul_b = {(e_funct3[1:0] == 2'b01 || e_funct3[1:0] == 2'b00) & fwd2[31], fwd2};
    assign mul_p = mul_a * mul_b;

    divider u_div (
        .clk(clk), .rst(rst), .start(e_valid && e_is_div && !redirect_mem), .kill(redirect_mem),
        .ack(e_fire), .op(e_funct3[1:0]), .a(fwd1), .b(fwd2), .done(div_done), .result(div_y));

    // floating-point unit: like the divider, the instruction waits in EX until it is done
    logic [2:0]  frm, e_fp_rm;
    logic        fs_off, e_fp_ill, fpu_done;
    logic [63:0] fpu_result;
    logic [31:0] fpu_iresult;
    logic [4:0]  fpu_flags;

    // two F/D checks need CSR state: mstatus.FS must be on, a dynamic rounding mode must be valid
    assign e_fp_ill = e_is_fp && (fs_off || (e_fp_rm_dyn && frm > 3'd4));
    assign e_fp_rm = (e_funct3 == 3'b111) ? frm : e_funct3;

    generate
        if (FPU) begin : g_fpu
            fpu u_fpu (
                .clk(clk), .rst(rst), .start(e_valid && e_fp_unit && !e_fp_ill && !redirect_mem),
                .kill(redirect_mem), .ack(e_fire), .op(e_fp_op), .dbl(e_fp_dbl), .rm(e_fp_rm),
                .a(ffwd1), .b(ffwd2), .c(ffwd3), .ia(fwd1), .done(fpu_done), .result(fpu_result),
                .iresult(fpu_iresult), .flags(fpu_flags));
        end else begin : g_no_fpu
            assign fpu_done = 1'b1;
            assign fpu_result = 64'd0;
            assign fpu_iresult = 32'd0;
            assign fpu_flags = 5'd0;
        end
    endgenerate

    assign e_busy = e_valid && ((e_is_div && !div_done) || (e_fp_unit && !e_fp_ill && !fpu_done));

    // memory address checks (the access itself happens in MEM)
    logic [31:0] st_wdata, ld_data_unused;
    logic [3:0]  st_wmask;
    logic        e_misaligned;
    lsu_align u_align_ex (
        .funct3(e_funct3), .offset(alu_y[1:0]), .store_data(e_is_fp ? ffwd2[31:0] : fwd2),
        .wdata(st_wdata), .wmask(st_wmask),
        .rdata(32'd0), .load_data(ld_data_unused), .misaligned(e_misaligned));

    // CSRs and exceptions
    logic [31:0] csr_rdata, mtvec, mepc, e_cause, e_tval, e_result;
    logic        csr_illegal, csr_writes_instret, e_exc;

    always_comb begin
        e_exc = 1'b1;
        e_cause = `CAUSE_ILLEGAL;
        e_tval = e_insn;
        if (e_illegal || (e_is_csr && csr_illegal) || e_fp_ill) begin
            e_cause = `CAUSE_ILLEGAL;
            e_tval = e_insn;
        end else if (e_is_ecall) begin
            e_cause = `CAUSE_ECALL_M;
            e_tval = 32'd0;
        end else if (e_is_ebreak) begin
            e_cause = `CAUSE_BREAKPOINT;
            e_tval = 32'd0;
        end else if (jumps && target[1]) begin
            e_cause = `CAUSE_MISALIGNED_FETCH;
            e_tval = target;
        end else if (e_is_load && e_misaligned) begin
            e_cause = `CAUSE_MISALIGNED_LOAD;
            e_tval = alu_y;
        end else if (e_is_store && e_misaligned) begin
            e_cause = `CAUSE_MISALIGNED_STORE;
            e_tval = alu_y;
        end else begin
            e_exc = 1'b0;
        end
    end

    logic        w_trap, w_no_count;
    csr_file #(.FPU(FPU)) u_csr (
        .clk(clk), .rst(rst), .addr(e_insn[31:20]), .op(e_funct3[1:0]),
        .src(e_funct3[2] ? {27'd0, e_rs1} : fwd1), .writes(e_csr_writes),
        .we(e_fire && e_is_csr && !e_exc), .rdata(csr_rdata), .illegal(csr_illegal),
        .writes_instret(csr_writes_instret),
        .trap(e_fire && e_exc), .trap_pc(e_pc), .trap_cause(e_cause), .trap_tval(e_tval),
        .mret(e_fire && e_is_mret && !e_exc), .mtvec(mtvec), .mepc(mepc),
        .instret_inc(w_valid && !w_trap && !w_no_count), .events(perf_events),
        .fp_flags(fpu_flags), .fp_flags_we(e_fire && e_fp_unit && !e_exc),
        .fp_dirty(e_fire && e_frd_we && !e_exc), .frm(frm), .fs_off(fs_off));

    always_comb begin
        if (e_exc) actual_next = mtvec;
        else if (e_is_mret) actual_next = mepc;
        else if (jumps) actual_next = target;
        else actual_next = e_pc4;
    end

    // Result mux.  The multiplier's product is the last signal to arrive in EX,
    // so it is selected last: everything else is chosen first (e_res_early) and
    // the product needs only one more 2:1 choice.
    logic [31:0] e_res_early;
    logic        e_is_mul;

    always_comb begin
        case (e_wb_sel)
            `WB_PC4: e_res_early = e_pc4;
            `WB_CSR: e_res_early = csr_rdata;
            `WB_MDU: e_res_early = div_y;
            `WB_FPU: e_res_early = FPU ? fpu_iresult : alu_y;   // never selected without the FPU
            default: e_res_early = alu_y;   // loads: the address
        endcase
    end

    assign e_is_mul = (e_wb_sel == `WB_MDU) && !e_is_div;
    assign e_result = !e_is_mul ? e_res_early : (e_funct3[1:0] == 2'b00) ? mul_p[31:0] : mul_p[63:32];

    assign e_hold = m_stall || e_busy;
    assign e_fire = e_valid && !e_hold && !redirect_mem;
    assign redirect_ex = e_fire && (actual_next != e_pred_next);
    assign redirect_ex_pc = actual_next;

    // =========================================================== MEM stage
    logic        m_is_store, m_is_fencei, m_trap, m_no_count;
    logic [31:0] m_insn, m_addr, m_cause, m_wdata;
    logic [3:0]  m_wmask;
    logic [2:0]  m_funct3;
    logic [15:0] m_seq;
    // FLD/FSD: two word accesses.  After the first one m_addr moves on by 4,
    // the two store words swap places and the first load word is kept in m_lo.
    logic        m_mem_dbl, m_beat, m_more, beat_adv;
    logic [31:0] m_wdata_hi, m_lo, m_addr4;
    logic [4:0]  m_fflags;

    always_ff @(posedge clk) begin
        if (rst) begin
            m_valid <= 1'b0;
        end else if (!m_stall) begin
            m_valid <= e_fire;
            m_pc <= e_pc;
            m_insn <= e_insn;
            m_seq <= e_seq;
            m_rd <= e_rd;
            m_rd_we <= e_rd_we && !e_exc;
            m_is_load <= e_is_load && !e_exc;
            m_is_store <= e_is_store && !e_exc;
            m_is_fencei <= e_is_fencei && !e_exc;
            m_trap <= e_exc;
            m_cause <= e_cause;
            m_no_count <= csr_writes_instret;
            m_funct3 <= e_funct3;
            m_addr <= alu_y;
            m_result <= e_result;
            m_wdata <= st_wdata;
            m_wmask <= st_wmask;
        end else if (beat_adv) begin
            m_addr <= m_addr4;
            m_wdata <= m_wdata_hi;
        end
    end

    // F/D part of the EX/MEM register
    generate
        if (FPU) begin : g_fp_mem
            always_ff @(posedge clk) begin
                if (rst) begin
                    m_beat <= 1'b0;
                end else if (!m_stall) begin
                    m_beat <= 1'b0;
                    m_frd_we <= e_frd_we && !e_exc;
                    m_fresult <= fpu_result;
                    m_fflags <= (e_fp_unit && !e_exc) ? fpu_flags : 5'd0;
                    m_mem_dbl <= e_is_fp && e_fp_dbl && (e_is_load || e_is_store) && !e_exc;
                    m_wdata_hi <= ffwd2[63:32];
                end else if (beat_adv) begin
                    m_beat <= 1'b1;
                    m_wdata_hi <= m_wdata;
                    m_lo <= dc_rdata;
                end
            end
        end else begin : g_no_fp_mem
            assign m_beat = 1'b0;
            assign m_frd_we = 1'b0;
            assign m_fresult = 64'd0;
            assign m_fflags = 5'd0;
            assign m_mem_dbl = 1'b0;
            assign m_wdata_hi = 32'd0;
            assign m_lo = 32'd0;
        end
    endgenerate

    // FENCE.I: write back the D-cache, then invalidate the I-cache, then
    // refetch the next instruction (redirect from MEM).
    logic fi_dc_done, fi_ic_done, dc_flush_done, ic_inv_done, dc_flush_req, ic_inv_req;
    assign dc_flush_req = m_valid && m_is_fencei && !fi_dc_done;
    assign ic_inv_req = m_valid && m_is_fencei && fi_dc_done && !fi_ic_done;

    always_ff @(posedge clk) begin
        if (rst || m_fire) begin
            fi_dc_done <= 1'b0;
            fi_ic_done <= 1'b0;
        end else begin
            if (dc_flush_done) fi_dc_done <= 1'b1;
            if (ic_inv_done) fi_ic_done <= 1'b1;
        end
    end

    logic        dc_req, dc_ready, dc_ev_access, dc_ev_miss, dc_ev_wb;
    logic [31:0] dc_rdata, ld_data, unused_wdata;
    logic [3:0]  unused_wmask;
    logic        unused_mis;

    assign dc_req = m_valid && (m_is_load || m_is_store);
    assign m_more = m_mem_dbl && !m_beat;            // the second word is still to come
    assign beat_adv = dc_req && dc_ready && m_more;  // the first word completes this cycle
    assign m_addr4 = m_addr + 32'd4;
    assign m_stall = m_valid && ((dc_req && (!dc_ready || m_more)) ||
                                 (m_is_fencei && !(fi_dc_done && fi_ic_done)));
    assign m_fire = m_valid && !m_stall;
    assign redirect_mem = m_fire && m_is_fencei;

    lsu_align u_align_mem (
        .funct3(m_funct3), .offset(m_addr[1:0]), .store_data(32'd0), .wdata(unused_wdata),
        .wmask(unused_wmask), .rdata(dc_rdata), .load_data(ld_data), .misaligned(unused_mis));

    // ============================================================ WB stage
    logic [31:0] w_pc, w_insn, w_cause, w_mem_addr, w_mem_wdata;
    logic [3:0]  w_mem_wmask;
    logic        w_mem_we;
    logic [15:0] w_seq;
    logic [4:0]  w_fflags;
    logic        w_mem_dbl;
    logic [31:0] w_mem_wdata_hi, m_addr_first;

    assign m_addr_first = m_beat ? m_addr - 32'd4 : m_addr;

    always_ff @(posedge clk) begin
        if (rst) begin
            w_valid <= 1'b0;
        end else begin
            w_valid <= m_fire;
            w_pc <= m_pc;
            w_insn <= m_insn;
            w_seq <= m_seq;
            w_rd <= m_rd;
            w_rd_we <= m_rd_we;
            w_result <= m_is_load ? ld_data : m_result;
            w_trap <= m_trap;
            w_cause <= m_cause;
            w_no_count <= m_no_count;
            w_mem_we <= m_is_store;
            w_mem_addr <= {m_addr_first[31:2], 2'b00};
            w_mem_wdata <= m_beat ? m_wdata_hi : m_wdata;
            w_mem_wmask <= m_wmask;
        end
    end

    // F/D part of the MEM/WB register
    generate
        if (FPU) begin : g_fp_wb
            always_ff @(posedge clk) begin
                w_frd_we <= m_frd_we;
                if (!m_is_load) w_fresult <= m_fresult;
                else if (m_mem_dbl) w_fresult <= {ld_data, m_lo};
                else w_fresult <= {32'hffff_ffff, ld_data};       // FLW: NaN-boxed
                w_fflags <= m_fflags;
                w_mem_dbl <= m_mem_dbl && m_is_store;
                w_mem_wdata_hi <= m_wdata;
            end
        end else begin : g_no_fp_wb
            assign w_frd_we = 1'b0;
            assign w_fresult = 64'd0;
            assign w_fflags = 5'd0;
            assign w_mem_dbl = 1'b0;
            assign w_mem_wdata_hi = 32'd0;
        end
    endgenerate

    assign commit_valid = w_valid;
    assign commit_pc = w_pc;
    assign commit_insn = w_insn;
    assign commit_trap = w_trap;
    assign commit_cause = w_cause;
    assign commit_rd_we = w_rd_we;
    assign commit_rd = w_rd;
    assign commit_rd_val = w_result;
    assign commit_mem_we = w_mem_we;
    assign commit_mem_addr = w_mem_addr;
    assign commit_mem_wdata = w_mem_wdata;
    assign commit_mem_wmask = w_mem_wmask;
    assign commit_frd_we = w_frd_we;
    assign commit_frd_val = w_fresult;
    assign commit_fflags = w_fflags;
    assign commit_mem_dbl = w_mem_dbl;
    assign commit_mem_wdata_hi = w_mem_wdata_hi;

    // ======================================================== caches, bus
    logic         ic_bus_req, ic_bus_ack, ic_ev_miss;
    logic [31:0]  ic_bus_addr;
    logic         dc_bus_req, dc_bus_we, dc_bus_ack;
    logic [31:0]  dc_bus_addr;
    logic [127:0] dc_bus_wdata;
    logic [15:0]  dc_bus_wstrb;

    icache #(.SETS(ICACHE_SETS)) u_icache (
        .clk(clk), .rst(rst), .addr_next(pc_next), .addr(pc_f), .kill(redirect_ex || redirect_mem),
        .hit(ic_hit), .insn(ic_insn), .inv_req(ic_inv_req), .inv_done(ic_inv_done), .ev_miss(ic_ev_miss),
        .bus_req(ic_bus_req), .bus_addr(ic_bus_addr), .bus_ack(ic_bus_ack), .bus_rdata(bus_rdata));

    dcache #(.SETS(DCACHE_SETS)) u_dcache (
        .clk(clk), .rst(rst), .addr_next(beat_adv ? m_addr4 : m_stall ? m_addr : alu_y), .req(dc_req), .we(m_is_store),
        .addr(m_addr), .wdata(m_wdata), .wmask(m_wmask), .ready(dc_ready), .rdata(dc_rdata),
        .flush_req(dc_flush_req), .flush_done(dc_flush_done),
        .ev_access(dc_ev_access), .ev_miss(dc_ev_miss), .ev_wb(dc_ev_wb),
        .bus_req(dc_bus_req), .bus_we(dc_bus_we), .bus_addr(dc_bus_addr), .bus_wdata(dc_bus_wdata),
        .bus_wstrb(dc_bus_wstrb), .bus_ack(dc_bus_ack), .bus_rdata(bus_rdata));

    mem_arbiter u_arb (
        .clk(clk), .rst(rst),
        .m0_req(dc_bus_req), .m0_we(dc_bus_we), .m0_addr(dc_bus_addr), .m0_wdata(dc_bus_wdata),
        .m0_wstrb(dc_bus_wstrb), .m0_ack(dc_bus_ack),
        .m1_req(ic_bus_req), .m1_we(1'b0), .m1_addr(ic_bus_addr), .m1_wdata(128'd0),
        .m1_wstrb(16'd0), .m1_ack(ic_bus_ack),
        .bus_req(bus_req), .bus_we(bus_we), .bus_addr(bus_addr), .bus_wdata(bus_wdata),
        .bus_wstrb(bus_wstrb), .bus_ack(bus_ack));

    // ========================================================= predictor
    // calls and returns, by the register-use hints of the ISA (ra = x1, t0 = x5)
    logic rd_link, rs1_link;
    assign rd_link = (e_rd == 5'd1) || (e_rd == 5'd5);
    assign rs1_link = (e_rs1 == 5'd1) || (e_rs1 == 5'd5);

    bpred #(.BTB_ENTRIES(BTB_ENTRIES), .BHT_ENTRIES(BHT_ENTRIES), .RAS_DEPTH(RAS_DEPTH),
            .ENABLE(BP_ENABLE)) u_bp (
        .clk(clk), .rst(rst), .pc(pc_f), .pred_taken(bp_taken), .pred_target(bp_target),
        .upd_valid(e_fire && !e_exc), .upd_branch(e_is_branch), .upd_jump(e_is_jal || e_is_jalr),
        .upd_call((e_is_jal || e_is_jalr) && rd_link), .upd_ret(e_is_jalr && rs1_link && !rd_link),
        .upd_pc(e_pc), .upd_taken(jumps), .upd_target(target));

    // ==================================================== perf counters
    always_comb begin
        perf_events = '0;
        perf_events[`EV_ICACHE_MISS] = ic_ev_miss;
        perf_events[`EV_DCACHE_ACCESS] = dc_ev_access;
        perf_events[`EV_DCACHE_MISS] = dc_ev_miss;
        perf_events[`EV_DCACHE_WB] = dc_ev_wb;
        perf_events[`EV_BRANCH] = e_fire && !e_exc && e_is_branch;
        perf_events[`EV_BRANCH_MISS] = e_fire && !e_exc && e_is_branch && redirect_ex;
        perf_events[`EV_JUMP] = e_fire && !e_exc && (e_is_jal || e_is_jalr);
        perf_events[`EV_JUMP_MISS] = e_fire && !e_exc && (e_is_jal || e_is_jalr) && redirect_ex;
        perf_events[`EV_LOAD_USE] = d_valid && (load_use || fload_use) && !e_hold && !redirect_ex && !redirect_mem;
        perf_events[`EV_ICACHE_ACCESS] = f_fire;
    end

    // ========================================================= probes
    assign dbg_f_pc = pc_f;
    assign dbg_f_seq = f_seq;
    assign dbg_d_valid = d_valid;
    assign dbg_d_seq = d_seq;
    assign dbg_d_pc = d_pc;
    assign dbg_d_insn = d_insn;
    assign dbg_e_valid = e_valid;
    assign dbg_e_seq = e_seq;
    assign dbg_m_valid = m_valid;
    assign dbg_m_seq = m_seq;
    assign dbg_w_valid = w_valid;
    assign dbg_w_seq = w_seq;
    assign dbg_why = {redirect_mem, redirect_ex, m_stall, e_busy, d_hazard, !ic_hit};

    logic unused;
    assign unused = &{1'b0, d_pred_taken, dc_is_mdu, dc_is_jal, ld_data_unused, unused_wdata,
                      unused_wmask, unused_mis, e_is_mdu, mul_p[65:64], w_cause[31:0], m_addr_first[1:0],
                      e_fp_op, e_fp_rm, e_fp_dbl, ffwd1, ffwd2, ffwd3, fpu_result, m_lo, m_fflags};
endmodule
