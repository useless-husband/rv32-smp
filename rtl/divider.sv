// Iterative divider for the pipelined core: one quotient bit per cycle
// (restoring division on magnitudes, sign fixed at the end).  `done` rises
// 33 cycles after `start` (a divide spends 34 cycles in EX); division by
// zero is done after one cycle with the ISA's results (quotient all ones,
// remainder = dividend).
// op: funct3[1:0] of DIV(00)/DIVU(01)/REM(10)/REMU(11).
module divider (
    input  logic        clk,
    input  logic        rst,
    input  logic        start,     // accepted when idle
    input  logic        kill,      // abandon (the instruction was squashed)
    input  logic        ack,       // result consumed, return to idle
    input  logic [1:0]  op,
    input  logic [31:0] a,
    input  logic [31:0] b,
    output logic        done,
    output logic [31:0] result
);
    localparam logic [1:0] IDLE = 2'd0, RUN = 2'd1, DONE = 2'd2;
    logic [1:0]  state;
    logic [5:0]  count;
    logic [31:0] q, r, d, dividend;
    logic        neg_q, neg_r, is_rem, div0;
    logic [32:0] cand;   // remainder shifted left with the next dividend bit
    logic [33:0] diff;

    assign cand = {r, q[31]};
    assign diff = {1'b0, cand} - {2'b0, d};

    always_ff @(posedge clk) begin
        if (rst || kill) begin
            state <= IDLE;
        end else begin
            case (state)
                IDLE: if (start) begin
                    is_rem <= op[1];
                    neg_q <= !op[0] && (a[31] ^ b[31]);
                    neg_r <= !op[0] && a[31];
                    q <= (!op[0] && a[31]) ? -a : a;
                    d <= (!op[0] && b[31]) ? -b : b;
                    dividend <= a;
                    r <= 32'd0;
                    count <= 6'd0;
                    div0 <= (b == 32'd0);
                    state <= (b == 32'd0) ? DONE : RUN;
                end
                RUN: begin
                    // shift the next dividend bit into the remainder, subtract if it fits
                    if (!diff[33]) begin
                        r <= diff[31:0];
                        q <= {q[30:0], 1'b1};
                    end else begin
                        r <= cand[31:0];
                        q <= {q[30:0], 1'b0};
                    end
                    count <= count + 6'd1;
                    if (count == 6'd31) state <= DONE;
                end
                default: if (ack) state <= IDLE;
            endcase
        end
    end

    assign done = (state == DONE);
    always_comb begin
        if (div0) result = is_rem ? dividend : 32'hffff_ffff;
        else if (is_rem) result = neg_r ? -r : r;
        else result = neg_q ? -q : q;
    end

    logic unused;
    assign unused = &{1'b0, diff[32]};
endmodule
