// Snooping bus: one transaction at a time ("atomic bus"), round-robin
// arbitration over 2N masters (the N L1 data caches, then the N instruction
// caches), write-back memory behind it.  See docs/DESIGN.md, section 3.
//
// A transaction:
//   IDLE   the arbiter picks the next requester (round robin) and latches
//          its command, address and data
//   SNOOP  coherent commands (RD, RDX, UPGR, IRD) are shown to every data
//          cache except the requester's own (an instruction fetch also asks
//          its own core's data cache); the bus waits until every one has
//          answered, and remembers whether any keeps a copy (shared) and
//          whether one supplied a dirty line
//   MEM    memory is read when nobody supplied the line; a dirty line
//          supplied for a BusRd is written to memory (both copies become S,
//          which is clean); write-backs and uncached accesses go to memory
//   RESP   one-cycle acknowledge to the requester with the line and the
//          shared flag
// Nothing overlaps, so every coherent request is ordered against every
// other one: the bus order is the coherence order of every line.
`include "rv_defs.svh"

module bus #(
    parameter int N = 2
) (
    input  logic             clk,
    input  logic             rst,
    // data caches (requester side)
    input  logic [N-1:0]     d_req,
    input  logic [3*N-1:0]   d_cmd,
    input  logic [32*N-1:0]  d_addr,
    input  logic [128*N-1:0] d_wdata,
    input  logic [16*N-1:0]  d_wstrb,
    output logic [N-1:0]     d_ack,
    // instruction caches
    input  logic [N-1:0]     i_req,
    input  logic [32*N-1:0]  i_addr,
    output logic [N-1:0]     i_ack,
    // shared response lines
    output logic [127:0]     rdata,
    output logic             shared,
    // snoop lines (to and from the data caches)
    output logic [N-1:0]     snp_valid,
    output logic [2:0]       snp_cmd,
    output logic [31:0]      snp_addr,
    input  logic [N-1:0]     snp_done,
    input  logic [N-1:0]     snp_hit,
    input  logic [N-1:0]     snp_supply,
    input  logic [128*N-1:0] snp_data,
    // memory
    output logic             mem_req,
    output logic             mem_we,
    output logic [31:0]      mem_addr,
    output logic [127:0]     mem_wdata,
    output logic [15:0]      mem_wstrb,
    input  logic             mem_ack,
    input  logic [127:0]     mem_rdata,
    // trace for the harness: {resp, supplied, shared, gnt, owner[3:0], cmd[2:0], 21'd0, addr[31:0]} (sim/trace.h)
    output logic [63:0]      trace
);
    localparam int M = 2 * N;
    localparam int OW = (M > 1) ? $clog2(M) : 1;
    localparam logic [1:0] B_IDLE = 2'd0, B_SNOOP = 2'd1, B_MEM = 2'd2, B_RESP = 2'd3;

    logic [1:0]   state;
    logic [OW-1:0] rr, owner, win;
    // 32-bit zero-extended copies for comparisons with loop indices (Yosys 0.33
    // does not parse int'() casts).
    wire [31:0] rr32    = {{(32-OW){1'b0}}, rr};
    wire [31:0] owner32 = {{(32-OW){1'b0}}, owner};
    wire [31:0] win32   = {{(32-OW){1'b0}}, win};
    logic         found;
    logic [2:0]   cmd;
    logic [31:0]  addr;
    logic [127:0] wdata, line;
    logic [15:0]  wstrb;
    logic [N-1:0] pend;
    logic         sup_v, shr;

    // ------------------------------------------------------------ arbiter
    logic [M-1:0] reqs;
    assign reqs = {i_req, d_req};

    always_comb begin
        found = 1'b0;
        win = '0;
        for (int i = 0; i < M; i++) begin
            int j;
            j = rr32 + i;
            if (j >= M) j = j - M;
            if (!found && reqs[j]) begin
                found = 1'b1;
                win = OW'(j);
            end
        end
    end

    // command of the winner; a fetch is always IRD
    logic [2:0]   w_cmd;
    logic [31:0]  w_addr;
    logic [127:0] w_wdata;
    logic [15:0]  w_wstrb;
    always_comb begin
        w_cmd = `CMD_IRD;
        w_addr = 32'd0;
        w_wdata = 128'd0;
        w_wstrb = 16'd0;
        for (int k = 0; k < N; k++) begin
            if (win32 == k) begin
                w_cmd = d_cmd[3*k +: 3];
                w_addr = d_addr[32*k +: 32];
                w_wdata = d_wdata[128*k +: 128];
                w_wstrb = d_wstrb[16*k +: 16];
            end
            if (win32 == N + k) w_addr = i_addr[32*k +: 32];
        end
    end

    // the data caches that must answer: all but the requester's own (for a
    // fetch, all of them)
    logic [N-1:0] snoopers;
    always_comb begin
        for (int k = 0; k < N; k++) snoopers[k] = (win32 != k);
    end

    logic coherent;
    assign coherent = (w_cmd == `CMD_RD || w_cmd == `CMD_RDX || w_cmd == `CMD_UPGR || w_cmd == `CMD_IRD);

    // ------------------------------------------------------------- snoops
    logic [N-1:0] still;
    logic         any_sup, any_hit;
    logic [127:0] sup_data;
    assign still = pend & ~snp_done;
    always_comb begin
        any_sup = 1'b0;
        any_hit = 1'b0;
        sup_data = 128'd0;
        for (int k = 0; k < N; k++) begin
            if (pend[k] && snp_done[k] && snp_supply[k]) begin
                any_sup = 1'b1;
                sup_data = snp_data[128*k +: 128];
            end
            if (pend[k] && snp_done[k] && snp_hit[k]) any_hit = 1'b1;
        end
    end

    logic sup_now;
    assign sup_now = sup_v || any_sup;

    always_ff @(posedge clk) begin
        if (rst) begin
            state <= B_IDLE;
            rr <= '0;
            owner <= '0;
            pend <= '0;
            snp_valid <= '0;
            sup_v <= 1'b0;
            shr <= 1'b0;
            cmd <= `CMD_RD;
            addr <= 32'd0;
        end else begin
            snp_valid <= '0;
            case (state)
                B_IDLE: if (found) begin
                    owner <= win;
                    rr <= (win32 == M - 1) ? '0 : win + 1'b1;
                    cmd <= w_cmd;
                    addr <= w_addr;
                    wdata <= w_wdata;
                    wstrb <= w_wstrb;
                    sup_v <= 1'b0;
                    shr <= 1'b0;
                    if (coherent) begin
                        pend <= snoopers;
                        snp_valid <= snoopers;
                        state <= B_SNOOP;
                    end else begin
                        state <= B_MEM;
                    end
                end
                B_SNOOP: begin
                    pend <= still;
                    if (any_sup) begin
                        sup_v <= 1'b1;
                        line <= sup_data;
                    end
                    if (any_hit) shr <= 1'b1;
                    if (still == '0) begin   // (the snoopers answer one or more cycles after the pulse)
                        if (cmd == `CMD_UPGR || (sup_now && cmd != `CMD_RD)) state <= B_RESP;
                        else state <= B_MEM;   // read memory, or flush a supplied line for a BusRd
                    end
                end
                B_MEM: if (mem_ack) begin
                    if (!(sup_v && cmd == `CMD_RD)) line <= mem_rdata;
                    state <= B_RESP;
                end
                default: state <= B_IDLE;   // B_RESP
            endcase
        end
    end

    assign snp_cmd = cmd;
    assign snp_addr = addr;

    // memory port
    assign mem_req = (state == B_MEM);
    assign mem_we = (cmd == `CMD_WB) || (cmd == `CMD_RD && sup_v) || (cmd == `CMD_UNC && wstrb != 16'd0);
    assign mem_addr = addr;
    assign mem_wdata = (cmd == `CMD_RD) ? line : wdata;
    assign mem_wstrb = (cmd == `CMD_RD) ? 16'hffff : wstrb;

    // response
    always_comb begin
        for (int k = 0; k < N; k++) begin
            d_ack[k] = (state == B_RESP) && owner32 == k;
            i_ack[k] = (state == B_RESP) && owner32 == N + k;
        end
    end
    assign rdata = line;
    assign shared = shr;

    assign trace = {(state == B_RESP), sup_v, shr, (state == B_IDLE && found),
                    (state == B_IDLE) ? {{(4-OW){1'b0}}, win} : {{(4-OW){1'b0}}, owner}, (state == B_IDLE) ? w_cmd : cmd, 21'd0,
                    (state == B_IDLE) ? w_addr : addr};
endmodule
