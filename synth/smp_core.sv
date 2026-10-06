// Synthesis wrapper: NHARTS cores and the snooping bus, with the memory port
// brought out to the top (memory is off-chip).  The simulation-only inputs
// are tied off (fetch_stall = 0) and the per-cycle trace outputs are left
// unconnected, so Yosys optimises that logic away: this is the real
// on-chip multicore.  Used by make synth / make loopcheck.
`include "rv_defs.svh"

module smp_core #(
    parameter int NHARTS = 2,
    parameter int ICACHE_SETS = 256,
    parameter int DCACHE_SETS = 128
) (
    input  logic                  clk,
    input  logic                  rst,
    input  logic [NHARTS-1:0]     hart_run,
    output logic                  mem_req,
    output logic                  mem_we,
    output logic [31:0]           mem_addr,
    output logic [127:0]          mem_wdata,
    output logic [15:0]           mem_wstrb,
    input  logic                  mem_ack,
    input  logic [127:0]          mem_rdata,
    output logic [NHARTS-1:0]     commit_valid
);
    localparam int N1 = NHARTS;
    logic [N1-1:0] d_req, d_ack, i_req, i_ack, snp_valid, snp_done, snp_hit, snp_supply;
    logic [3*N1-1:0]   d_cmd;
    logic [32*N1-1:0]  d_addr, i_addr;
    logic [128*N1-1:0] d_wdata, snp_data;
    logic [16*N1-1:0]  d_wstrb;
    logic [127:0]      rdata;
    logic              shared;
    logic [2:0]        snp_cmd;
    logic [31:0]       snp_addr;

    genvar h;
    generate
        for (h = 0; h < NHARTS; h = h + 1) begin : g_hart
            // only commit_valid is kept (so the cores are not optimised away);
            // the other commit/trace outputs are observation-only and left open
            core #(.HARTID(h), .ICACHE_SETS(ICACHE_SETS), .DCACHE_SETS(DCACHE_SETS)) u_core (
                .clk(clk), .sys_rst(rst), .run(hart_run[h]), .fetch_stall(1'b0),
                .dbus_req(d_req[h]), .dbus_cmd(d_cmd[3*h +: 3]), .dbus_addr(d_addr[32*h +: 32]),
                .dbus_wdata(d_wdata[128*h +: 128]), .dbus_wstrb(d_wstrb[16*h +: 16]), .dbus_ack(d_ack[h]),
                .ibus_req(i_req[h]), .ibus_addr(i_addr[32*h +: 32]), .ibus_ack(i_ack[h]),
                .bus_rdata(rdata), .bus_shared(shared),
                .snp_valid(snp_valid[h]), .snp_cmd(snp_cmd), .snp_addr(snp_addr), .snp_done(snp_done[h]),
                .snp_hit(snp_hit[h]), .snp_supply(snp_supply[h]), .snp_data(snp_data[128*h +: 128]),
                .commit_valid(commit_valid[h]), .commit_pc(), .commit_insn(), .commit_trap(),
                .commit_cause(), .commit_rd_we(), .commit_rd(), .commit_rd_val(),
                .commit_mem_we(), .commit_mem_addr(), .commit_mem_wdata(),
                .commit_mem_wmask(), .perf_events(), .trace());
        end
    endgenerate

    bus #(.N(NHARTS)) u_bus (
        .clk(clk), .rst(rst),
        .d_req(d_req), .d_cmd(d_cmd), .d_addr(d_addr), .d_wdata(d_wdata), .d_wstrb(d_wstrb), .d_ack(d_ack),
        .i_req(i_req), .i_addr(i_addr), .i_ack(i_ack), .rdata(rdata), .shared(shared),
        .snp_valid(snp_valid), .snp_cmd(snp_cmd), .snp_addr(snp_addr), .snp_done(snp_done),
        .snp_hit(snp_hit), .snp_supply(snp_supply), .snp_data(snp_data),
        .mem_req(mem_req), .mem_we(mem_we), .mem_addr(mem_addr), .mem_wdata(mem_wdata),
        .mem_wstrb(mem_wstrb), .mem_ack(mem_ack), .mem_rdata(mem_rdata), .trace());
endmodule
