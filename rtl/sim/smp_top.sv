// Simulation top: NHARTS cores (core.sv), the snooping bus (bus.sv) and the
// latency-configurable memory model.  Per-hart signals are flat vectors,
// hart k in bits [W*k +: W] (Yosys 0.33 has no multi-dimensional packed
// arrays).  Parameters can be overridden with -G at build time.
`include "rv_defs.svh"

module smp_top #(
    parameter int NHARTS = 2,
    parameter int MEM_LATENCY = 10,
    parameter int ICACHE_SETS = 256,
    parameter int DCACHE_SETS = 128,
    parameter int BUG = 0,
    parameter int LOCKOUT = 32
) (
    input  logic                  clk,
    input  logic                  rst,
    input  logic [NHARTS-1:0]     hart_run,
    input  logic [NHARTS-1:0]     hart_stall,     // timing noise: bubbles injected at fetch
    input  logic [7:0]            mem_jitter,     // timing noise: extra latency of the next memory request
    output logic [NHARTS-1:0]     commit_valid,
    output logic [32*NHARTS-1:0]  commit_pc,
    output logic [32*NHARTS-1:0]  commit_insn,
    output logic [NHARTS-1:0]     commit_trap,
    output logic [32*NHARTS-1:0]  commit_cause,
    output logic [NHARTS-1:0]     commit_rd_we,
    output logic [5*NHARTS-1:0]   commit_rd,
    output logic [32*NHARTS-1:0]  commit_rd_val,
    output logic [NHARTS-1:0]     commit_mem_we,
    output logic [32*NHARTS-1:0]  commit_mem_addr,
    output logic [32*NHARTS-1:0]  commit_mem_wdata,
    output logic [4*NHARTS-1:0]   commit_mem_wmask,
    output logic [`NUM_EVENTS*NHARTS-1:0] perf_events,
    output logic [256*NHARTS-1:0] l1_trace,
    output logic [63:0]           bus_trace,
    output logic [31:0]           cfg_nharts,
    output logic [7:0]            cfg_bug,
    output logic                  mmio_we,
    output logic [31:0]           mmio_addr,
    output logic [31:0]           mmio_wdata
);
    logic [NHARTS-1:0]     d_req, d_ack, i_req, i_ack, snp_valid, snp_done, snp_hit, snp_supply;
    logic [3*NHARTS-1:0]   d_cmd;
    logic [32*NHARTS-1:0]  d_addr, i_addr;
    logic [128*NHARTS-1:0] d_wdata, snp_data;
    logic [16*NHARTS-1:0]  d_wstrb;
    logic [127:0]          rdata, mem_wdata, mem_rdata;
    logic                  shared, mem_req, mem_we, mem_ack;
    logic [2:0]            snp_cmd;
    logic [31:0]           snp_addr, mem_addr;
    logic [15:0]           mem_wstrb;

    assign cfg_nharts = NHARTS;
    assign cfg_bug = 8'(BUG);

    genvar h;
    generate
        for (h = 0; h < NHARTS; h = h + 1) begin : g_hart
            core #(.HARTID(h), .ICACHE_SETS(ICACHE_SETS), .DCACHE_SETS(DCACHE_SETS), .BUG(BUG),
                   .LOCKOUT(LOCKOUT)) u_core (
                .clk(clk), .sys_rst(rst), .run(hart_run[h]), .fetch_stall(hart_stall[h]),
                .dbus_req(d_req[h]), .dbus_cmd(d_cmd[3*h +: 3]), .dbus_addr(d_addr[32*h +: 32]),
                .dbus_wdata(d_wdata[128*h +: 128]), .dbus_wstrb(d_wstrb[16*h +: 16]), .dbus_ack(d_ack[h]),
                .ibus_req(i_req[h]), .ibus_addr(i_addr[32*h +: 32]), .ibus_ack(i_ack[h]),
                .bus_rdata(rdata), .bus_shared(shared),
                .snp_valid(snp_valid[h]), .snp_cmd(snp_cmd), .snp_addr(snp_addr), .snp_done(snp_done[h]),
                .snp_hit(snp_hit[h]), .snp_supply(snp_supply[h]), .snp_data(snp_data[128*h +: 128]),
                .commit_valid(commit_valid[h]), .commit_pc(commit_pc[32*h +: 32]),
                .commit_insn(commit_insn[32*h +: 32]), .commit_trap(commit_trap[h]),
                .commit_cause(commit_cause[32*h +: 32]), .commit_rd_we(commit_rd_we[h]),
                .commit_rd(commit_rd[5*h +: 5]), .commit_rd_val(commit_rd_val[32*h +: 32]),
                .commit_mem_we(commit_mem_we[h]), .commit_mem_addr(commit_mem_addr[32*h +: 32]),
                .commit_mem_wdata(commit_mem_wdata[32*h +: 32]), .commit_mem_wmask(commit_mem_wmask[4*h +: 4]),
                .perf_events(perf_events[`NUM_EVENTS*h +: `NUM_EVENTS]), .trace(l1_trace[256*h +: 256]));
        end
    endgenerate

    bus #(.N(NHARTS)) u_bus (
        .clk(clk), .rst(rst),
        .d_req(d_req), .d_cmd(d_cmd), .d_addr(d_addr), .d_wdata(d_wdata), .d_wstrb(d_wstrb), .d_ack(d_ack),
        .i_req(i_req), .i_addr(i_addr), .i_ack(i_ack), .rdata(rdata), .shared(shared),
        .snp_valid(snp_valid), .snp_cmd(snp_cmd), .snp_addr(snp_addr), .snp_done(snp_done),
        .snp_hit(snp_hit), .snp_supply(snp_supply), .snp_data(snp_data),
        .mem_req(mem_req), .mem_we(mem_we), .mem_addr(mem_addr), .mem_wdata(mem_wdata),
        .mem_wstrb(mem_wstrb), .mem_ack(mem_ack), .mem_rdata(mem_rdata), .trace(bus_trace));

    mem_model #(.LATENCY(MEM_LATENCY), .NHARTS(NHARTS)) u_mem (
        .clk(clk), .rst(rst), .jitter(mem_jitter), .req(mem_req), .we(mem_we), .addr(mem_addr), .wdata(mem_wdata),
        .wstrb(mem_wstrb), .ack(mem_ack), .rdata(mem_rdata),
        .mmio_we(mmio_we), .mmio_addr(mmio_addr), .mmio_wdata(mmio_wdata));
endmodule
