// Private L1 data cache with the MESI protocol on a snooping bus.
//
// Organisation: 2 ways x SETS sets x 16-byte lines (4 KiB with SETS = 128),
// write-back, write-allocate, one LRU bit per set.  Tags and data are in
// synchronous-read arrays (block RAM); the MESI state of each line is three
// flip-flop bits (valid, exclusive, dirty: I = 0xx, S = 100, E = 110,
// M = 111).  Addresses with bit 31 clear are I/O and go to the bus uncached.
//
// The controller does three jobs with one set of array ports:
//   * core accesses (only in IDLE): loads hit in S/E/M; stores, LR, SC and
//     AMOs need E or M (E -> M silently on a write).  An SC without a
//     matching reservation fails at once, without the bus.
//   * misses: pick a victim (a dirty one moves to the one-entry write-back
//     buffer), ask the bus for BusRd / BusRdX / BusUpgr and, in the cycle the
//     bus answers, install the line AND perform the waiting access.  Doing
//     the access in the same cycle means no other request can take the line
//     away between the fill and its use, so two caches cannot livelock by
//     stealing a line from each other before either uses it.  The dirty
//     victim is written back afterwards (BusWB), while the core goes on; a
//     new miss waits until the buffer is empty.
//   * snoops: the bus pulses snp_valid for every coherent request of another
//     master; the request is latched, the arrays are read at its index, and
//     one cycle later the cache answers (snp_done) with whether it keeps a
//     copy (snp_hit) and, if it held the line dirty (in the array or in the
//     write-back buffer), the data (snp_supply, snp_data).  Snoops are served
//     in IDLE, while waiting for the bus (REQ, UNC) - never ignored, or two
//     waiting caches would deadlock.
//
// Races the controller resolves (and BUG re-introduces, see rv_defs.svh):
//   * an upgrade waiting for the bus loses its S copy to another cache's
//     BusRdX/BusUpgr: the request becomes BusRdX            (BUG_LOST_UPGRADE)
//   * a request for a line that sits in the write-back buffer: the buffer
//     answers like an M line and is cancelled/updated        (BUG_WB_NO_SNOOP)
//   * a remote invalidation of the reserved line clears the
//     LR reservation                                         (BUG_LRSC_NO_CLEAR)
//   * a read miss takes E only when no other cache has a copy (BUG_E_IGNORES_SHARED)
//   * LR lock-out: for LOCKOUT cycles after an LR, while the core is not
//     waiting for the cache, snoops to the reserved line are deferred so the
//     SC normally succeeds (forward progress for LR/SC loops); the window
//     always closes                                          (BUG_LOCKOUT_FOREVER)
`include "rv_defs.svh"

module l1d #(
    parameter int SETS = 128,           // sets per way; capacity = 2 * SETS * 16 bytes
    parameter int BUG = 0,              // `BUG_* (0: the correct protocol)
    parameter int LOCKOUT = 32          // LR lock-out window in cycles
) (
    input  logic         clk,
    input  logic         rst,
    // core side (MEM stage)
    input  logic [31:0]  addr_next,     // address of the access in MEM next cycle
    input  logic         req,
    input  logic         we,            // store
    input  logic         atomic,        // LR / SC / AMO (amo_op says which)
    input  logic [4:0]   amo_op,
    input  logic [31:0]  addr,
    input  logic [31:0]  wdata,         // store data in its byte lanes; rs2 for atomics
    input  logic [3:0]   wmask,
    output logic         ready,         // the access completes this cycle
    output logic [31:0]  rdata,         // addressed word (loads, LR, AMO old value), SC result
    output logic         wrote,         // ... and it wrote memory (store, AMO, successful SC)
    output logic [31:0]  wword,         // the word it left in memory
    // requester side of the bus
    output logic         bus_req,
    output logic [2:0]   bus_cmd,
    output logic [31:0]  bus_addr,
    output logic [127:0] bus_wdata,
    output logic [15:0]  bus_wstrb,
    input  logic         bus_ack,
    input  logic [127:0] bus_rdata,
    input  logic         bus_shared,
    // snooper side
    input  logic         snp_valid,
    input  logic [2:0]   snp_cmd,
    input  logic [31:0]  snp_addr,
    output logic         snp_done,
    output logic         snp_hit,
    output logic         snp_supply,
    output logic [127:0] snp_data,
    // performance events
    output logic         ev_access,
    output logic         ev_miss,
    output logic         ev_wb,
    output logic         ev_rd,
    output logic         ev_rdx,
    output logic         ev_upgr,
    output logic         ev_inv,
    output logic         ev_supply,
    output logic         ev_scfail,
    // trace record for the simulation harness (layout in sim/trace.h)
    output logic [255:0] trace
);
    localparam int IW = $clog2(SETS);
    localparam int TW = 28 - IW;
    localparam logic [1:0] IDLE = 2'd0, REQ = 2'd1, SNP = 2'd2, UNC = 2'd3;

    logic [127:0]  data0 [0:SETS-1];
    logic [127:0]  data1 [0:SETS-1];
    logic [TW-1:0] tag0  [0:SETS-1];
    logic [TW-1:0] tag1  [0:SETS-1];
    logic [SETS-1:0] v0, v1, x0, x1, dt0, dt1, lru;   // valid, exclusive, dirty; lru = way to evict

    logic [1:0]    state, ret_state;
    logic [IW-1:0] idx, sidx, ra, ra_q;
    logic [TW-1:0] tag_a, stag;
    logic          cacheable;

    assign idx = addr[4 +: IW];
    assign tag_a = addr[31:4+IW];
    assign cacheable = addr[31];

    // ----------------------------------------------------------- snoop latch
    logic          sp;               // a snoop is waiting to be served
    logic [2:0]    sp_cmd;
    logic [31:0]   sp_addr;
    logic          snoop_start, defer;
    assign sidx = sp_addr[4 +: IW];
    assign stag = sp_addr[31:4+IW];

    // ------------------------------------------------------------ the arrays
    logic [127:0]  d0_q, d1_q, d0, d1;
    logic [TW-1:0] t0_q, t1_q;
    logic          wr_en, wr_way, twr;
    logic [127:0]  wline;
    logic          arr_stale;        // the tags read last cycle may predate a fill

    assign ra = snoop_start ? sidx : addr_next[4 +: IW];

    always_ff @(posedge clk) begin
        d0_q <= data0[ra];
        d1_q <= data1[ra];
        t0_q <= tag0[ra];
        t1_q <= tag1[ra];
        ra_q <= ra;
        if (wr_en && !wr_way) data0[idx] <= wline;
        if (wr_en && wr_way) data1[idx] <= wline;
        if (twr && !wr_way) tag0[idx] <= tag_a;
        if (twr && wr_way) tag1[idx] <= tag_a;
    end

    // the line written in a cycle is what the next read of that set must see
    logic          byp_v, byp_way;
    logic [IW-1:0] byp_idx;
    logic [127:0]  byp_line;
    assign d0 = (byp_v && !byp_way && byp_idx == ra_q) ? byp_line : d0_q;
    assign d1 = (byp_v && byp_way && byp_idx == ra_q) ? byp_line : d1_q;

    // ------------------------------------------------------- core-side lookup
    logic h0, h1, hit, hway, hx, arr_ok;
    logic [127:0] hline;
    assign arr_ok = !arr_stale && ra_q == idx;
    assign h0 = v0[idx] && t0_q == tag_a;
    assign h1 = v1[idx] && t1_q == tag_a;
    assign hit = h0 || h1;
    assign hway = h1;
    assign hx = h1 ? x1[idx] : x0[idx];
    assign hline = h1 ? d1 : d0;

    logic is_sc, is_lr, excl, resv_v, resv_match, sc_fast_fail;
    logic [27:0] resv_line;
    assign is_sc = atomic && amo_op == `AMO_SC;
    assign is_lr = atomic && amo_op == `AMO_LR;
    assign excl = we || atomic;
    assign resv_match = resv_v && resv_line == addr[31:4];
    assign sc_fast_fail = is_sc && !resv_match;

    logic core_ok, hit_ok, do_hit, do_miss, do_unc, wb_v;
    assign core_ok = (state == IDLE) && req && cacheable && arr_ok;
    assign hit_ok = hit && (!excl || hx);
    assign do_hit = core_ok && (sc_fast_fail || hit_ok);
    assign do_miss = core_ok && !sc_fast_fail && !hit_ok && !wb_v && !snoop_start;
    assign do_unc = (state == IDLE) && req && !cacheable && !wb_v && !snoop_start;

    // --------------------------------------------- performing an access
    // At a hit the line comes from the arrays; at a fill from the bus (or,
    // for an upgrade, from the arrays: the S copy is current).
    logic [2:0]    fcmd;
    logic          fway, fill_ack;
    logic [127:0]  base, newline;
    logic [31:0]   oldw, neww, amo_res, st_merge, wmask32;
    logic          sc_ok, writes_mem;

    assign fill_ack = (state == REQ) && bus_ack;
    assign base = (state == REQ) ? ((fcmd == `CMD_UPGR) ? (fway ? d1 : d0) : bus_rdata) : hline;
    assign oldw = base[32*addr[3:2] +: 32];
    assign wmask32 = {{8{wmask[3]}}, {8{wmask[2]}}, {8{wmask[1]}}, {8{wmask[0]}}};
    assign st_merge = (oldw & ~wmask32) | (wdata & wmask32);

    always_comb begin
        case (amo_op)
            `AMO_SWAP: amo_res = wdata;
            `AMO_ADD:  amo_res = oldw + wdata;
            `AMO_XOR:  amo_res = oldw ^ wdata;
            `AMO_AND:  amo_res = oldw & wdata;
            `AMO_OR:   amo_res = oldw | wdata;
            `AMO_MIN:  amo_res = ($signed(oldw) < $signed(wdata)) ? oldw : wdata;
            `AMO_MAX:  amo_res = ($signed(oldw) > $signed(wdata)) ? oldw : wdata;
            `AMO_MINU: amo_res = (oldw < wdata) ? oldw : wdata;
            `AMO_MAXU: amo_res = (oldw > wdata) ? oldw : wdata;
            default:   amo_res = wdata;   // SC
        endcase
    end

    assign sc_ok = is_sc && resv_match;
    assign writes_mem = we || (atomic && !is_lr && (!is_sc || sc_ok));
    assign neww = atomic ? amo_res : st_merge;
    always_comb begin
        newline = base;
        newline[32*addr[3:2] +: 32] = writes_mem ? neww : oldw;
    end

    assign ready = do_hit || fill_ack || ((state == UNC) && bus_ack);
    assign rdata = (state == UNC) ? bus_rdata[32*addr[3:2] +: 32] : is_sc ? {31'd0, !sc_ok} : oldw;
    assign wrote = writes_mem && (do_hit || fill_ack);
    assign wword = neww;

    assign wr_en = (do_hit && writes_mem && !sc_fast_fail) || fill_ack;
    assign wr_way = (state == REQ) ? fway : hway;
    assign wline = newline;
    assign twr = fill_ack && fcmd != `CMD_UPGR;

    // ------------------------------------------------ victim and write-back
    logic          vway, vvalid, vdirty;
    logic [TW-1:0] vtag;
    logic [127:0]  wb_line;
    logic [31:0]   wb_addr;
    assign vway = !v0[idx] ? 1'b0 : !v1[idx] ? 1'b1 : lru[idx];
    assign vvalid = vway ? v1[idx] : v0[idx];
    assign vdirty = vway ? dt1[idx] : dt0[idx];
    assign vtag = vway ? t1_q : t0_q;

    logic upgrade_miss;
    assign upgrade_miss = excl && hit;          // the line is here, but only in S

    // ----------------------------------------------------------------- snoop
    logic sh0, sh1, sh, sway, s_m, wbhit, s_inval, s_supply_arr, s_supply_wb, resv_hit;
    logic [127:0] sline;
    assign sh0 = v0[sidx] && t0_q == stag;
    assign sh1 = v1[sidx] && t1_q == stag;
    assign sh = (state == SNP) && (sh0 || sh1);
    assign sway = sh1;
    assign s_m = sway ? dt1[sidx] : dt0[sidx];
    assign sline = sway ? d1 : d0;
    assign wbhit = (state == SNP) && wb_v && wb_addr[31:4] == sp_addr[31:4] && (BUG != `BUG_WB_NO_SNOOP);
    assign s_inval = sp_cmd == `CMD_RDX || sp_cmd == `CMD_UPGR;
    assign s_supply_arr = sh && s_m && sp_cmd != `CMD_UPGR;
    assign s_supply_wb = wbhit && sp_cmd != `CMD_UPGR;
    assign resv_hit = resv_v && resv_line == sp_addr[31:4];

    logic [5:0] lock_cnt;
    assign defer = (state == IDLE) && lock_cnt != 6'd0 && resv_hit && sp_cmd != `CMD_IRD;
    assign snoop_start = sp && !defer && (state != SNP);

    assign snp_done = (state == SNP);
    assign snp_hit = sh && !s_inval;
    assign snp_supply = s_supply_arr || s_supply_wb;
    assign snp_data = s_supply_arr ? sline : wb_line;

    // --------------------------------------------------------- bus requests
    logic [1:0] rq;   // which request is outstanding: 0 none, 1 fill, 2 uncached, 3 write-back
    always_comb begin
        if (state == REQ || (state == SNP && ret_state == REQ)) rq = 2'd1;
        else if (state == UNC || (state == SNP && ret_state == UNC)) rq = 2'd2;
        else if (wb_v) rq = 2'd3;
        else rq = 2'd0;
    end
    assign bus_req = rq != 2'd0;
    always_comb begin
        bus_cmd = `CMD_WB;
        bus_addr = wb_addr;
        bus_wdata = wb_line;
        bus_wstrb = 16'hffff;
        if (rq == 2'd1) begin
            bus_cmd = fcmd;
            bus_addr = {addr[31:4], 4'b0000};
        end else if (rq == 2'd2) begin
            bus_cmd = `CMD_UNC;
            bus_addr = {addr[31:4], 4'b0000};
            bus_wdata = {4{wdata}};
            bus_wstrb = we ? ({12'd0, wmask} << (4 * addr[3:2])) : 16'h0000;
        end
    end
    logic wb_ack;
    assign wb_ack = bus_ack && rq == 2'd3;

    // ------------------------------------------------------------ controller
    logic        shared_eff;
    assign shared_eff = bus_shared && (BUG != `BUG_E_IGNORES_SHARED);

    always_ff @(posedge clk) begin
        if (rst) begin
            state <= IDLE;
            ret_state <= IDLE;
            v0 <= '0;
            v1 <= '0;
            x0 <= '0;
            x1 <= '0;
            dt0 <= '0;
            dt1 <= '0;
            lru <= '0;
            byp_v <= 1'b0;
            sp <= 1'b0;
            wb_v <= 1'b0;
            resv_v <= 1'b0;
            lock_cnt <= 6'd0;
            arr_stale <= 1'b0;
            fcmd <= `CMD_RD;
            fway <= 1'b0;
        end else begin
            byp_v <= wr_en;
            byp_way <= wr_way;
            byp_idx <= idx;
            byp_line <= wline;
            arr_stale <= fill_ack;
            if (lock_cnt != 6'd0 && BUG != `BUG_LOCKOUT_FOREVER) lock_cnt <= lock_cnt - 6'd1;
            if (snp_valid) begin
                sp <= 1'b1;
                sp_cmd <= snp_cmd;
                sp_addr <= snp_addr;
            end
            if (wb_ack) wb_v <= 1'b0;

            // core access that hits (also in the cycle a snoop starts: it was decided first)
            if (do_hit) begin
                if (!sc_fast_fail) lru[idx] <= !hway;
                if (writes_mem && !hway) dt0[idx] <= 1'b1;
                if (writes_mem && hway) dt1[idx] <= 1'b1;
                if (is_lr) begin
                    resv_v <= 1'b1;
                    resv_line <= addr[31:4];
                    lock_cnt <= 6'(LOCKOUT);
                end
                if (is_sc) begin
                    resv_v <= 1'b0;
                    lock_cnt <= 6'd0;
                end
            end
            if (core_ok && !sc_fast_fail && !hit_ok) lock_cnt <= 6'd0;   // the core waits: no lock-out

            if (snoop_start) begin
                ret_state <= state;
                state <= SNP;
            end else begin
                case (state)
                    IDLE: begin
                        if (do_miss) begin
                            if (upgrade_miss) begin
                                fcmd <= `CMD_UPGR;
                                fway <= hway;
                            end else begin
                                fcmd <= excl ? `CMD_RDX : `CMD_RD;
                                fway <= vway;
                                if (!vway) v0[idx] <= 1'b0;
                                else v1[idx] <= 1'b0;
                                if (vvalid && vdirty) begin
                                    wb_v <= 1'b1;
                                    wb_line <= vway ? d1 : d0;
                                    wb_addr <= {vtag, idx, 4'b0000};
                                end
                                if (vvalid && resv_v && resv_line == {vtag, idx}) resv_v <= 1'b0;
                            end
                            state <= REQ;
                        end else if (do_unc) begin
                            state <= UNC;
                        end
                    end
                    REQ: if (bus_ack) begin
                        if (!fway) begin
                            v0[idx] <= 1'b1;
                            x0[idx] <= fcmd != `CMD_RD || !shared_eff;
                            dt0[idx] <= fcmd != `CMD_RD;
                        end else begin
                            v1[idx] <= 1'b1;
                            x1[idx] <= fcmd != `CMD_RD || !shared_eff;
                            dt1[idx] <= fcmd != `CMD_RD;
                        end
                        lru[idx] <= !fway;
                        if (is_lr) begin
                            resv_v <= 1'b1;
                            resv_line <= addr[31:4];
                            lock_cnt <= 6'(LOCKOUT);
                        end
                        if (is_sc) begin
                            resv_v <= 1'b0;
                            lock_cnt <= 6'd0;
                        end
                        state <= IDLE;
                    end
                    UNC: if (bus_ack) state <= IDLE;
                    default: begin   // SNP: answer this cycle
                        sp <= snp_valid;
                        state <= ret_state;
                        if (sh) begin
                            if (s_inval) begin
                                if (!sway) v0[sidx] <= 1'b0;
                                else v1[sidx] <= 1'b0;
                            end else if (sp_cmd == `CMD_RD) begin
                                if (!sway) begin x0[sidx] <= 1'b0; dt0[sidx] <= 1'b0; end
                                else begin x1[sidx] <= 1'b0; dt1[sidx] <= 1'b0; end
                            end
                        end
                        if (wbhit && (sp_cmd == `CMD_RD || sp_cmd == `CMD_RDX)) wb_v <= 1'b0;
                        if (s_inval && resv_hit && BUG != `BUG_LRSC_NO_CLEAR) begin
                            resv_v <= 1'b0;
                            lock_cnt <= 6'd0;
                        end
                        if (s_inval && ret_state == REQ && fcmd == `CMD_UPGR && addr[31:4] == sp_addr[31:4] &&
                            BUG != `BUG_LOST_UPGRADE)
                            fcmd <= `CMD_RDX;
                    end
                endcase
            end
        end
    end

    // --------------------------------------------------------------- events
    assign ev_access = do_hit;
    assign ev_miss = do_miss;
    assign ev_wb = wb_ack;
    assign ev_rd = do_miss && !excl;
    assign ev_rdx = do_miss && excl && !upgrade_miss;
    assign ev_upgr = do_miss && upgrade_miss;
    assign ev_inv = sh && s_inval;
    assign ev_supply = (state == SNP) && snp_supply;
    assign ev_scfail = ready && is_sc && !sc_ok;

    // ---------------------------------------------------------------- trace
    // MESI state of a line from its three bits
    function automatic logic [1:0] mesi(input logic v, input logic x, input logic d);
        mesi = !v ? `ST_I : !x ? `ST_S : d ? `ST_M : `ST_E;
    endfunction

    logic [1:0] hit_old, hit_new, snp_old, snp_new, vic_st, fill_st;
    logic [2:0] perf_kind;
    assign hit_old = !hit ? `ST_I : hway ? mesi(1'b1, x1[idx], dt1[idx]) : mesi(1'b1, x0[idx], dt0[idx]);
    assign hit_new = (writes_mem && !sc_fast_fail) ? `ST_M : hit_old;
    assign snp_old = !sh ? `ST_I : sway ? mesi(1'b1, x1[sidx], dt1[sidx]) : mesi(1'b1, x0[sidx], dt0[sidx]);
    assign snp_new = !sh ? `ST_I : s_inval ? `ST_I : (sp_cmd == `CMD_RD) ? `ST_S : snp_old;
    assign vic_st = !vvalid ? `ST_I : vway ? mesi(1'b1, x1[idx], dt1[idx]) : mesi(1'b1, x0[idx], dt0[idx]);
    assign fill_st = (fcmd != `CMD_RD) ? `ST_M : shared_eff ? `ST_S : `ST_E;
    assign perf_kind = !atomic ? (we ? 3'd1 : 3'd0) : is_lr ? 3'd2 : is_sc ? 3'd3 : 3'd4;

    logic tr_perf, tr_vic, tr_snp_resv, tr_snp_upg;
    assign tr_perf = (do_hit || fill_ack);
    assign tr_vic = do_miss && !upgrade_miss && vvalid;
    assign tr_snp_resv = (state == SNP) && s_inval && resv_hit && BUG != `BUG_LRSC_NO_CLEAR;
    assign tr_snp_upg = (state == SNP) && s_inval && ret_state == REQ && fcmd == `CMD_UPGR &&
                        addr[31:4] == sp_addr[31:4] && BUG != `BUG_LOST_UPGRADE;

    assign trace[31:0] = {snp_new, snp_old, hit_new, hit_old, fcmd, sp_cmd,
                          (do_miss ? (upgrade_miss ? `CMD_UPGR : excl ? `CMD_RDX : `CMD_RD) : 3'd0), perf_kind,
                          sc_ok, shared_eff, tr_snp_upg, tr_snp_resv, wbhit, tr_vic && vdirty, wb_ack,
                          fill_ack, (state == SNP), tr_vic, do_miss, tr_perf};
    assign trace[63:32] = {addr[31:2], 2'b00};
    assign trace[95:64] = oldw;
    assign trace[127:96] = writes_mem ? neww : oldw;
    assign trace[159:128] = {addr[31:4], 4'b0000};
    assign trace[191:160] = {vtag, idx, 4'b0000};
    assign trace[223:192] = {sp_addr[31:4], 4'b0000};
    assign trace[255:224] = {14'd0, lock_cnt, sp, defer, s_supply_wb, s_supply_arr, wmask, fill_st, vic_st};

    logic unused;
    assign unused = &{1'b0, addr_next[31:4+IW], addr_next[3:0], sp_addr[3:0], addr[1:0]};
endmodule
