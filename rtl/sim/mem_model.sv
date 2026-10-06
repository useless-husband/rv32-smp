// Simulation memory behind the bus: 1 MiB of RAM at 0x80000000 plus the I/O
// registers.  Each request (a 16-byte line read or a strobed line write) is
// accepted when the memory is idle and acknowledged LATENCY cycles later; the
// whole line moves in that one beat.  RAM is every address with bit 31 set
// (wrapped to 1 MiB); I/O reads return zero except MMIO_NHARTS (0x10000008).
`include "rv_defs.svh"

module mem_model #(
    parameter int LATENCY = 10,       // cycles from request to acknowledge (>= 1)
    parameter int NHARTS = 1
) (
    input  logic         clk,
    input  logic         rst,
    input  logic [7:0]   jitter,      // extra cycles for a request accepted this cycle
    input  logic         req,
    input  logic         we,
    input  logic [31:0]  addr,
    input  logic [127:0] wdata,
    input  logic [15:0]  wstrb,
    output logic         ack,
    output logic [127:0] rdata,
    output logic         mmio_we,     // pulses with the ack of an I/O write
    output logic [31:0]  mmio_addr,
    output logic [31:0]  mmio_wdata
);
    localparam int LINES = `RAM_BYTES / 16;
    logic [127:0] ram [0:LINES-1];
    logic [31:0]  words [0:LINES*4-1];
    string program_hex;

    initial begin
        for (int i = 0; i < LINES * 4; i++) words[i] = 32'd0;
        if ($value$plusargs("program=%s", program_hex)) $readmemh(program_hex, words);
        for (int i = 0; i < LINES; i++)
            ram[i] = {words[4*i+3], words[4*i+2], words[4*i+1], words[4*i]};
    end

    logic        busy;
    logic [15:0] count;
    logic [15:0] lat;

    logic [$clog2(LINES)-1:0] line;
    assign line = addr[4 +: $clog2(LINES)];

    always_ff @(posedge clk) begin
        if (rst) begin
            busy <= 1'b0;
            count <= 16'd0;
        end else if (!busy) begin
            if (req) begin
                busy <= 1'b1;
                count <= 16'd1;
                lat <= 16'(LATENCY) + {8'd0, jitter};
            end
        end else if (ack) begin
            busy <= 1'b0;
            if (we && addr[31])
                for (int b = 0; b < 16; b++)
                    if (wstrb[b]) ram[line][8*b +: 8] <= wdata[8*b +: 8];
        end else begin
            count <= count + 16'd1;
        end
    end

    assign ack = busy && count >= lat;
    assign rdata = addr[31] ? ram[line] : (addr[31:4] == 28'h100_0000) ? {32'd0, 32'(NHARTS), 64'd0} : 128'd0;

    // I/O: the written word is the lane with strobes set
    always_comb begin
        mmio_we = ack && we && !addr[31] && wstrb != 16'd0;
        mmio_addr = {addr[31:4], 4'b0000};
        mmio_wdata = wdata[31:0];
        for (int l = 0; l < 4; l++)
            if (wstrb[4*l +: 4] != 4'd0) begin
                mmio_addr = {addr[31:4], 2'(l), 2'b00};
                mmio_wdata = wdata[32*l +: 32];
            end
    end

    logic unused;
    assign unused = &{1'b0, addr[3:0], addr[30:20]};
endmodule
