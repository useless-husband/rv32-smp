// 32 x 32-bit register file: two asynchronous read ports, one synchronous
// write port, x0 hard-wired to zero.  With BYPASS=1 a read of the register
// being written in the same cycle returns the new value (the pipeline uses
// this for the WB -> ID path).  The single-cycle core must use BYPASS=0,
// because there the write data depends on the read data.
module regfile #(
    parameter bit BYPASS = 1'b0
) (
    input  logic        clk,
    input  logic [4:0]  ra1,
    input  logic [4:0]  ra2,
    output logic [31:0] rd1,
    output logic [31:0] rd2,
    input  logic        we,
    input  logic [4:0]  wa,
    input  logic [31:0] wd
);
    logic [31:0] regs [0:31];

    always_ff @(posedge clk)
        if (we && wa != 5'd0)
            regs[wa] <= wd;

    always_comb begin
        if (ra1 == 5'd0) rd1 = 32'd0;
        else if (BYPASS && we && wa == ra1) rd1 = wd;
        else rd1 = regs[ra1];
        if (ra2 == 5'd0) rd2 = 32'd0;
        else if (BYPASS && we && wa == ra2) rd2 = wd;
        else rd2 = regs[ra2];
    end
endmodule
