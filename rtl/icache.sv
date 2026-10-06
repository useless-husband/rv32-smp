// Instruction cache: direct mapped, 16-byte lines, SETS lines.
//
// The tag and data arrays have a synchronous read port (they map to block
// RAM), so they are read with the address the fetch stage will hold in the
// NEXT cycle (addr_next); in the cycle the address is in IF (addr) the line
// and its tag are at the array outputs and the hit check is a compare.
// Valid bits are flip-flops so FENCE.I can clear them in one cycle.
//
// On a miss the cache reads the whole line from memory, writes it, and
// spends one more cycle re-reading the arrays before it reports a hit.  The
// fetch address may change during a refill (a redirect); the refill still
// completes and the new address is looked up afterwards.  A miss on a fetch
// that the pipeline is discarding in the same cycle starts no refill.
module icache #(
    parameter int SETS = 256
) (
    input  logic         clk,
    input  logic         rst,
    input  logic [31:0]  addr_next,
    input  logic [31:0]  addr,
    input  logic         kill,       // this fetch is being discarded (redirect): do not refill it
    output logic         hit,
    output logic [31:0]  insn,
    input  logic         inv_req,
    output logic         inv_done,
    output logic         ev_miss,
    output logic         bus_req,
    output logic [31:0]  bus_addr,
    input  logic         bus_ack,
    input  logic [127:0] bus_rdata
);
    localparam int IW = $clog2(SETS);
    localparam int TW = 28 - IW;
    localparam logic [1:0] IDLE = 2'd0, REFILL = 2'd1, WAIT = 2'd2;

    logic [127:0]  data [0:SETS-1];
    logic [TW-1:0] tags [0:SETS-1];
    logic [SETS-1:0] valid;
    logic [127:0]  data_q;
    logic [TW-1:0] tag_q;
    logic [1:0]    state;
    logic [31:0]   miss_addr;

    logic [IW-1:0] idx, idx_next, midx;
    assign idx = addr[4 +: IW];
    assign idx_next = addr_next[4 +: IW];
    assign midx = miss_addr[4 +: IW];

    // block-RAM style arrays: synchronous read, one write port
    always_ff @(posedge clk) begin
        data_q <= data[idx_next];
        tag_q <= tags[idx_next];
        if (state == REFILL && bus_ack) begin
            data[midx] <= bus_rdata;
            tags[midx] <= miss_addr[31:4+IW];
        end
    end

    assign hit = (state == IDLE) && valid[idx] && (tag_q == addr[31:4+IW]);
    assign insn = data_q[32*addr[3:2] +: 32];
    assign inv_done = (state == IDLE) && inv_req;
    assign ev_miss = (state == IDLE) && !hit && !inv_req && !kill;
    assign bus_req = (state == REFILL);
    assign bus_addr = {miss_addr[31:4], 4'b0000};

    always_ff @(posedge clk) begin
        if (rst) begin
            state <= IDLE;
            valid <= '0;
        end else begin
            case (state)
                IDLE:
                    if (inv_req) begin
                        valid <= '0;
                    end else if (!hit && !kill) begin
                        miss_addr <= addr;
                        state <= REFILL;
                    end
                REFILL:
                    if (bus_ack) begin
                        valid[midx] <= 1'b1;
                        state <= WAIT;
                    end
                default: state <= IDLE;  // WAIT: arrays re-read the refilled line
            endcase
        end
    end

    logic unused;
    assign unused = &{1'b0, addr[1:0], addr_next[31:4+IW], addr_next[3:0], miss_addr[3:0]};
endmodule
