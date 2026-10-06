// RV32I integer ALU (combinational).
`include "rv_defs.svh"

module alu (
    input  logic [3:0]  op,
    input  logic [31:0] a,
    input  logic [31:0] b,
    output logic [31:0] y
);
    logic [4:0] sh;
    assign sh = b[4:0];

    always_comb begin
        case (op)
            `ALU_ADD:  y = a + b;
            `ALU_SUB:  y = a - b;
            `ALU_SLL:  y = a << sh;
            `ALU_SLT:  y = {31'b0, $signed(a) < $signed(b)};
            `ALU_SLTU: y = {31'b0, a < b};
            `ALU_XOR:  y = a ^ b;
            `ALU_SRL:  y = a >> sh;
            `ALU_SRA:  y = $unsigned($signed(a) >>> sh);
            `ALU_OR:   y = a | b;
            `ALU_AND:  y = a & b;
            default:   y = a + b;
        endcase
    end
endmodule
