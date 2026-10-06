// Byte-lane alignment for loads and stores (combinational, shared by both
// cores).  Memory is a 32-bit word interface with a 4-bit byte mask.
module lsu_align (
    input  logic [2:0]  funct3,     // LB/LH/LW/LBU/LHU or SB/SH/SW
    input  logic [1:0]  offset,     // address bits [1:0]
    input  logic [31:0] store_data, // rs2
    output logic [31:0] wdata,      // store data in its byte lanes
    output logic [3:0]  wmask,
    input  logic [31:0] rdata,      // the addressed memory word
    output logic [31:0] load_data,  // extended load result
    output logic        misaligned
);
    logic [31:0] shifted;
    logic [4:0]  shamt;

    assign shamt = {offset, 3'b000};

    always_comb begin
        case (funct3[1:0])
            2'b00: begin
                wdata = {4{store_data[7:0]}};
                wmask = 4'b0001 << offset;
                misaligned = 1'b0;
            end
            2'b01: begin
                wdata = {2{store_data[15:0]}};
                wmask = 4'b0011 << offset;
                misaligned = offset[0];
            end
            default: begin
                wdata = store_data;
                wmask = 4'b1111;
                misaligned = (offset != 2'b00);
            end
        endcase

        shifted = rdata >> shamt;
        case (funct3)
            3'b000:  load_data = {{24{shifted[7]}}, shifted[7:0]};
            3'b001:  load_data = {{16{shifted[15]}}, shifted[15:0]};
            3'b100:  load_data = {24'b0, shifted[7:0]};
            3'b101:  load_data = {16'b0, shifted[15:0]};
            default: load_data = shifted;
        endcase
    end
endmodule
