// One hart: the 5-stage pipelined RV32IMA core (IF, ID, EX, MEM, WB) with
// full forwarding, a one-cycle load-use stall, branch resolution in EX with a
// BTB + 2-bit-counter predictor, an iterative divider, an I-cache and a MESI
// L1 data cache on the shared snooping bus.  The pipeline is the one of
// rv32-pipeline (core B); what changed for the multicore is listed in
// docs/DESIGN.md, section 2.
//
// Who may stall whom (each stage holds when a later one holds):
//   MEM holds   : the L1 is not ready (miss, snoop being answered, bus busy),
//                 or FENCE.I still invalidating the I-cache
//   EX holds    : MEM holds, or a divide is still running
//   ID holds    : EX holds, or a load-use hazard (atomics count as loads),
//                 or a CSR instruction waiting for EX/MEM/WB to drain
//   IF holds    : ID holds, or the I-cache misses (ID then gets a bubble)
// Redirects (they discard every younger instruction):
//   from EX  : the next PC differs from the predicted one (mispredicted
//              branch/jump, trap, MRET)       -> IF and ID are flushed
//   from MEM : FENCE.I after the I-cache is invalidated -> IF, ID, EX flushed
//
// Memory accesses (loads, stores, LR, SC, AMOs) are performed in MEM, one at
// a time and in program order, and the L1 blocks until each one is done:
// there is no store buffer.  That is why FENCE needs no action here.
`include "rv_defs.svh"

module core #(
    parameter int  HARTID      = 0,
    parameter int  ICACHE_SETS = 256,   // 4 KiB direct mapped
    parameter int  DCACHE_SETS = 128,   // 4 KiB, 2 ways
    parameter int  BTB_ENTRIES = 128,
    parameter int  BHT_ENTRIES = 256,
    parameter int  RAS_DEPTH   = 8,
    parameter bit  BP_ENABLE   = 1'b1,
    parameter int  BUG         = 0,     // protocol variant of the L1 (`BUG_*)
    parameter int  LOCKOUT     = 32     // LR lock-out window of the L1
) (
    input  logic         clk,
    input  logic         sys_rst,       // resets everything
    input  logic         run,           // 0: the pipeline is held in reset (its caches still answer snoops)
    input  logic         fetch_stall,   // simulation timing noise: no fetch this cycle (0 in synthesis)
    // the L1 data cache on the bus (see bus.sv)
    output logic         dbus_req,
    output logic [2:0]   dbus_cmd,
    output logic [31:0]  dbus_addr,
    output logic [127:0] dbus_wdata,
    output logic [15:0]  dbus_wstrb,
    input  logic         dbus_ack,
    output logic         ibus_req,
    output logic [31:0]  ibus_addr,
    input  logic         ibus_ack,
    input  logic [127:0] bus_rdata,
    input  logic         bus_shared,
    input  logic         snp_valid,
    input  logic [2:0]   snp_cmd,
    input  logic [31:0]  snp_addr,
    output logic         snp_done,
    output logic         snp_hit,
    output logic         snp_supply,
    output logic [127:0] snp_data,
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
    output logic [`NUM_EVENTS-1:0] perf_events,
    output logic [255:0] trace      // L1 trace record for the harness (sim/trace.h)
);
    // the pipeline's reset; the caches use sys_rst
    logic rst;
    assign rst = sys_rst || !run;

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
    assign f_fire = ic_hit && !d_hold && !redirect_ex && !redirect_mem && !fetch_stall;

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
    logic        dc_is_atomic;
    logic [4:0]  dc_amo_op;

    decoder u_dec (
        .insn(d_insn), .alu_op(dc_alu_op), .a_sel(dc_a_sel), .b_imm(dc_b_imm), .imm(dc_imm),
        .rd(dc_rd), .rs1(dc_rs1), .rs2(dc_rs2), .uses_rs1(dc_uses_rs1), .uses_rs2(dc_uses_rs2),
        .rd_we(dc_rd_we), .wb_sel(dc_wb_sel), .funct3(dc_funct3), .is_branch(dc_is_branch),
        .is_jal(dc_is_jal), .is_jalr(dc_is_jalr), .is_load(dc_is_load), .is_store(dc_is_store),
        .is_atomic(dc_is_atomic), .amo_op(dc_amo_op), .is_mdu(dc_is_mdu), .is_div(dc_is_div), .is_csr(dc_is_csr), .csr_writes(dc_csr_writes),
        .is_ecall(dc_is_ecall), .is_ebreak(dc_is_ebreak), .is_mret(dc_is_mret),
        .is_fencei(dc_is_fencei), .illegal(dc_illegal));

    logic [31:0] rf_rd1, rf_rd2;
    logic        w_valid, w_rd_we;
    logic [4:0]  w_rd;
    logic [31:0] w_result;

    regfile #(.BYPASS(1'b1)) u_rf (
        .clk(clk), .ra1(dc_rs1), .ra2(dc_rs2), .rd1(rf_rd1), .rd2(rf_rd2),
        .we(w_valid && w_rd_we), .wa(w_rd), .wd(w_result));


    // hazards detected in ID
    logic e_valid, e_is_load, e_rd_we, m_valid;
    logic [4:0] e_rd;
    logic load_use, serialize;

    assign load_use = e_valid && e_is_load && e_rd_we &&
                      ((dc_uses_rs1 && dc_rs1 == e_rd) || (dc_uses_rs2 && dc_rs2 == e_rd));
    // CSR instructions run alone so counter reads see every older instruction retired
    assign serialize = dc_is_csr && (e_valid || m_valid || w_valid);
    assign d_hazard = d_valid && (load_use || serialize);
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
    logic        e_is_atomic;
    logic [4:0]  e_amo_op;

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
            e_is_atomic <= dc_is_atomic;
            e_amo_op <= dc_amo_op;
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


    assign e_busy = e_valid && e_is_div && !div_done;

    // memory address checks (the access itself happens in MEM)
    logic [31:0] st_wdata, ld_data_unused;
    logic [3:0]  st_wmask;
    logic        e_misaligned;
    lsu_align u_align_ex (
        .funct3(e_funct3), .offset(alu_y[1:0]), .store_data(fwd2),
        .wdata(st_wdata), .wmask(st_wmask),
        .rdata(32'd0), .load_data(ld_data_unused), .misaligned(e_misaligned));

    // CSRs and exceptions
    logic [31:0] csr_rdata, mtvec, mepc, e_cause, e_tval, e_result;
    logic        csr_illegal, csr_writes_instret, e_exc;

    always_comb begin
        e_exc = 1'b1;
        e_cause = `CAUSE_ILLEGAL;
        e_tval = e_insn;
        if (e_illegal || (e_is_csr && csr_illegal)) begin
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
            e_cause = (e_is_atomic && e_amo_op != `AMO_LR) ? `CAUSE_MISALIGNED_STORE : `CAUSE_MISALIGNED_LOAD;
            e_tval = alu_y;
        end else if (e_is_store && e_misaligned) begin
            e_cause = `CAUSE_MISALIGNED_STORE;
            e_tval = alu_y;
        end else if (e_is_atomic && !alu_y[31]) begin   // atomics need cacheable memory
            e_cause = (e_amo_op != `AMO_LR) ? `CAUSE_STORE_FAULT : `CAUSE_LOAD_FAULT;
            e_tval = alu_y;
        end else begin
            e_exc = 1'b0;
        end
    end

    logic        w_trap, w_no_count;
    csr_file #(.HARTID(HARTID)) u_csr (
        .clk(clk), .rst(rst), .addr(e_insn[31:20]), .op(e_funct3[1:0]),
        .src(e_funct3[2] ? {27'd0, e_rs1} : fwd1), .writes(e_csr_writes),
        .we(e_fire && e_is_csr && !e_exc), .rdata(csr_rdata), .illegal(csr_illegal),
        .writes_instret(csr_writes_instret),
        .trap(e_fire && e_exc), .trap_pc(e_pc), .trap_cause(e_cause), .trap_tval(e_tval),
        .mret(e_fire && e_is_mret && !e_exc), .mtvec(mtvec), .mepc(mepc),
        .instret_inc(w_valid && !w_trap && !w_no_count), .events(perf_events));

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
    logic        m_is_store, m_is_atomic, m_is_fencei, m_trap, m_no_count;
    logic [4:0]  m_amo_op;
    logic [31:0] m_insn, m_addr, m_cause, m_wdata;
    logic [3:0]  m_wmask;
    logic [2:0]  m_funct3;
    logic [15:0] m_seq;

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
            m_is_atomic <= e_is_atomic && !e_exc;
            m_amo_op <= e_amo_op;
            m_is_fencei <= e_is_fencei && !e_exc;
            m_trap <= e_exc;
            m_cause <= e_cause;
            m_no_count <= csr_writes_instret;
            m_funct3 <= e_funct3;
            m_addr <= alu_y;
            m_result <= e_result;
            m_wdata <= st_wdata;
            m_wmask <= st_wmask;
        end
    end


    // FENCE.I: invalidate the I-cache, then refetch the next instruction
    // (redirect from MEM).  The D-cache needs no write-back: instruction
    // fetches are snooped (IRD), so a dirty line in any data cache - this
    // hart's or another's - is what the I-cache gets.
    logic fi_ic_done, ic_inv_done, ic_inv_req;
    assign ic_inv_req = m_valid && m_is_fencei && !fi_ic_done;

    always_ff @(posedge clk) begin
        if (rst || m_fire) fi_ic_done <= 1'b0;
        else if (ic_inv_done) fi_ic_done <= 1'b1;
    end

    logic        dc_req, dc_ready, dc_wrote;
    logic [31:0] dc_rdata, dc_wword, ld_data, unused_wdata;
    logic [3:0]  unused_wmask;
    logic        unused_mis;
    logic        dc_ev_access, dc_ev_miss, dc_ev_wb, dc_ev_rd, dc_ev_rdx, dc_ev_upgr, dc_ev_inv, dc_ev_supply,
                 dc_ev_scfail;

    assign dc_req = m_valid && (m_is_load || m_is_store);
    assign m_stall = m_valid && ((dc_req && !dc_ready) || (m_is_fencei && !fi_ic_done));
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
            w_mem_we <= m_is_store || (m_is_atomic && dc_wrote);
            w_mem_addr <= {m_addr[31:2], 2'b00};
            w_mem_wdata <= m_is_atomic ? dc_wword : m_wdata;
            w_mem_wmask <= m_wmask;
        end
    end


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

    // ======================================================== caches, bus
    logic ic_ev_miss;

    icache #(.SETS(ICACHE_SETS)) u_icache (
        .clk(clk), .rst(sys_rst), .addr_next(pc_next), .addr(pc_f), .kill(redirect_ex || redirect_mem),
        .hit(ic_hit), .insn(ic_insn), .inv_req(ic_inv_req), .inv_done(ic_inv_done), .ev_miss(ic_ev_miss),
        .bus_req(ibus_req), .bus_addr(ibus_addr), .bus_ack(ibus_ack), .bus_rdata(bus_rdata));

    l1d #(.SETS(DCACHE_SETS), .BUG(BUG), .LOCKOUT(LOCKOUT)) u_l1d (
        .clk(clk), .rst(sys_rst), .addr_next(m_stall ? m_addr : alu_y), .req(dc_req), .we(m_is_store),
        .atomic(m_is_atomic), .amo_op(m_amo_op), .addr(m_addr), .wdata(m_wdata), .wmask(m_wmask),
        .ready(dc_ready), .rdata(dc_rdata), .wrote(dc_wrote), .wword(dc_wword),
        .bus_req(dbus_req), .bus_cmd(dbus_cmd), .bus_addr(dbus_addr), .bus_wdata(dbus_wdata),
        .bus_wstrb(dbus_wstrb), .bus_ack(dbus_ack), .bus_rdata(bus_rdata), .bus_shared(bus_shared),
        .snp_valid(snp_valid), .snp_cmd(snp_cmd), .snp_addr(snp_addr), .snp_done(snp_done),
        .snp_hit(snp_hit), .snp_supply(snp_supply), .snp_data(snp_data),
        .ev_access(dc_ev_access), .ev_miss(dc_ev_miss), .ev_wb(dc_ev_wb), .ev_rd(dc_ev_rd),
        .ev_rdx(dc_ev_rdx), .ev_upgr(dc_ev_upgr), .ev_inv(dc_ev_inv), .ev_supply(dc_ev_supply),
        .ev_scfail(dc_ev_scfail), .trace(trace));

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
        perf_events[`EV_LOAD_USE] = d_valid && load_use && !e_hold && !redirect_ex && !redirect_mem;
        perf_events[`EV_ICACHE_ACCESS] = f_fire;
        perf_events[`EV_BUS_RD] = dc_ev_rd;
        perf_events[`EV_BUS_RDX] = dc_ev_rdx;
        perf_events[`EV_BUS_UPGR] = dc_ev_upgr;
        perf_events[`EV_SNOOP_INV] = dc_ev_inv;
        perf_events[`EV_SNOOP_SUPPLY] = dc_ev_supply;
        perf_events[`EV_SC_FAIL] = dc_ev_scfail;
    end

    logic unused;
    assign unused = &{1'b0, d_pred_taken, dc_is_mdu, dc_is_jal, ld_data_unused, unused_wdata,
                      unused_wmask, unused_mis, e_is_mdu, mul_p[65:64], w_cause[31:0], f_seq, d_seq, e_seq,
                      m_seq, w_seq};
endmodule
