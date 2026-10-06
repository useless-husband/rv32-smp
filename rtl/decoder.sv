// Instruction decoder for RV32IMA + Zicsr + Zifencei (machine mode).
// Purely combinational.  An illegal encoding clears every side-effect output
// (no register write, no memory access) and sets `illegal`.  Whether a CSR
// number exists is decided by csr_file, not here.
//
// The A extension (opcode AMO, word size only): LR.W, SC.W and the nine
// AMOs.  They are decoded as loads (they write rd from memory, so the
// load-use interlock applies) with is_atomic set and amo_op = funct5; the
// address is rs1 itself (immediate 0).  The aq/rl bits are accepted and
// need no action: this core performs every memory access in program order
// and in one place (see docs/DESIGN.md, memory model).
`include "rv_defs.svh"

module decoder (
    input  logic [31:0] insn,
    output logic [3:0]  alu_op,
    output logic [1:0]  a_sel,
    output logic        b_imm,      // ALU operand B: 1 = immediate, 0 = rs2
    output logic [31:0] imm,
    output logic [4:0]  rd,
    output logic [4:0]  rs1,
    output logic [4:0]  rs2,
    output logic        uses_rs1,
    output logic        uses_rs2,
    output logic        rd_we,      // writes rd, and rd is not x0
    output logic [2:0]  wb_sel,
    output logic [2:0]  funct3,
    output logic        is_branch,
    output logic        is_jal,
    output logic        is_jalr,
    output logic        is_load,
    output logic        is_store,
    output logic        is_atomic,  // A extension (is_load is also set)
    output logic [4:0]  amo_op,     // funct5 of an A instruction (AMO_* in rv_defs.svh)
    output logic        is_mdu,     // M extension
    output logic        is_div,     // DIV/DIVU/REM/REMU
    output logic        is_csr,
    output logic        csr_writes, // CSR instruction that writes its CSR
    output logic        is_ecall,
    output logic        is_ebreak,
    output logic        is_mret,
    output logic        is_fencei,
    output logic        illegal
);
    logic [6:0] opcode, funct7;
    logic [31:0] imm_i, imm_s, imm_b, imm_u, imm_j;
    logic writes, legal;

    assign opcode = insn[6:0];
    assign funct3 = insn[14:12];
    assign funct7 = insn[31:25];
    assign rd     = insn[11:7];
    assign rs1    = insn[19:15];
    assign rs2    = insn[24:20];
    assign amo_op = insn[31:27];

    assign imm_i = {{20{insn[31]}}, insn[31:20]};
    assign imm_s = {{20{insn[31]}}, insn[31:25], insn[11:7]};
    assign imm_b = {{19{insn[31]}}, insn[31], insn[7], insn[30:25], insn[11:8], 1'b0};
    assign imm_u = {insn[31:12], 12'b0};
    assign imm_j = {{11{insn[31]}}, insn[31], insn[19:12], insn[20], insn[30:21], 1'b0};

    always_comb begin
        alu_op = `ALU_ADD;
        a_sel = `A_RS1;
        b_imm = 1'b1;
        imm = imm_i;
        uses_rs1 = 1'b0;
        uses_rs2 = 1'b0;
        writes = 1'b0;
        wb_sel = `WB_ALU;
        is_branch = 1'b0;
        is_jal = 1'b0;
        is_jalr = 1'b0;
        is_load = 1'b0;
        is_store = 1'b0;
        is_mdu = 1'b0;
        is_csr = 1'b0;
        is_ecall = 1'b0;
        is_ebreak = 1'b0;
        is_mret = 1'b0;
        is_fencei = 1'b0;
        is_atomic = 1'b0;
        legal = 1'b0;

        case (opcode)
            7'b0110111: begin // LUI
                legal = 1'b1; writes = 1'b1; a_sel = `A_ZERO; imm = imm_u;
            end
            7'b0010111: begin // AUIPC
                legal = 1'b1; writes = 1'b1; a_sel = `A_PC; imm = imm_u;
            end
            7'b1101111: begin // JAL
                legal = 1'b1; writes = 1'b1; is_jal = 1'b1; imm = imm_j; wb_sel = `WB_PC4;
            end
            7'b1100111: begin // JALR
                legal = (funct3 == 3'b000);
                writes = 1'b1; is_jalr = 1'b1; uses_rs1 = 1'b1; wb_sel = `WB_PC4;
            end
            7'b1100011: begin // branches
                legal = (funct3 != 3'b010) && (funct3 != 3'b011);
                is_branch = 1'b1; uses_rs1 = 1'b1; uses_rs2 = 1'b1; imm = imm_b;
            end
            7'b0000011: begin // loads
                legal = (funct3 == 3'b000) || (funct3 == 3'b001) || (funct3 == 3'b010) ||
                        (funct3 == 3'b100) || (funct3 == 3'b101);
                writes = 1'b1; is_load = 1'b1; uses_rs1 = 1'b1; wb_sel = `WB_MEM;
            end
            7'b0100011: begin // stores
                legal = (funct3 == 3'b000) || (funct3 == 3'b001) || (funct3 == 3'b010);
                is_store = 1'b1; uses_rs1 = 1'b1; uses_rs2 = 1'b1; imm = imm_s;
            end
            7'b0010011: begin // OP-IMM
                writes = 1'b1; uses_rs1 = 1'b1;
                case (funct3)
                    3'b000: begin legal = 1'b1; alu_op = `ALU_ADD; end
                    3'b010: begin legal = 1'b1; alu_op = `ALU_SLT; end
                    3'b011: begin legal = 1'b1; alu_op = `ALU_SLTU; end
                    3'b100: begin legal = 1'b1; alu_op = `ALU_XOR; end
                    3'b110: begin legal = 1'b1; alu_op = `ALU_OR; end
                    3'b111: begin legal = 1'b1; alu_op = `ALU_AND; end
                    3'b001: begin legal = (funct7 == 7'b0000000); alu_op = `ALU_SLL; end
                    default: begin // 3'b101
                        legal = (funct7 == 7'b0000000) || (funct7 == 7'b0100000);
                        alu_op = funct7[5] ? `ALU_SRA : `ALU_SRL;
                    end
                endcase
            end
            7'b0110011: begin // OP and M extension
                writes = 1'b1; uses_rs1 = 1'b1; uses_rs2 = 1'b1; b_imm = 1'b0;
                if (funct7 == 7'b0000001) begin
                    legal = 1'b1; is_mdu = 1'b1; wb_sel = `WB_MDU;
                end else begin
                    legal = (funct7 == 7'b0000000) ||
                            (funct7 == 7'b0100000 && (funct3 == 3'b000 || funct3 == 3'b101));
                    case (funct3)
                        3'b000: alu_op = funct7[5] ? `ALU_SUB : `ALU_ADD;
                        3'b001: alu_op = `ALU_SLL;
                        3'b010: alu_op = `ALU_SLT;
                        3'b011: alu_op = `ALU_SLTU;
                        3'b100: alu_op = `ALU_XOR;
                        3'b101: alu_op = funct7[5] ? `ALU_SRA : `ALU_SRL;
                        3'b110: alu_op = `ALU_OR;
                        default: alu_op = `ALU_AND;
                    endcase
                end
            end
            7'b0101111: begin // A extension, word size
                is_atomic = 1'b1; is_load = 1'b1; writes = 1'b1; wb_sel = `WB_MEM;
                uses_rs1 = 1'b1; uses_rs2 = (amo_op != `AMO_LR); imm = 32'd0;
                legal = (funct3 == 3'b010) && (amo_op == `AMO_LR ? rs2 == 5'd0 :
                         amo_op == `AMO_SC || amo_op == `AMO_SWAP || amo_op == `AMO_ADD ||
                         amo_op == `AMO_XOR || amo_op == `AMO_AND || amo_op == `AMO_OR ||
                         amo_op == `AMO_MIN || amo_op == `AMO_MAX || amo_op == `AMO_MINU ||
                         amo_op == `AMO_MAXU);
            end
            7'b0001111: begin // FENCE (no-op: accesses are already performed in order), FENCE.I
                legal = (funct3 == 3'b000) || (funct3 == 3'b001);
                is_fencei = (funct3 == 3'b001);
            end
            7'b1110011: begin // SYSTEM
                if (funct3 == 3'b000) begin
                    is_ecall  = (insn == 32'h0000_0073);
                    is_ebreak = (insn == 32'h0010_0073);
                    is_mret   = (insn == 32'h3020_0073);
                    legal = is_ecall || is_ebreak || is_mret || (insn == 32'h1050_0073); // WFI = no-op
                end else if (funct3 != 3'b100) begin
                    legal = 1'b1; is_csr = 1'b1; writes = 1'b1; wb_sel = `WB_CSR;
                    uses_rs1 = !funct3[2];
                end
            end
            default: legal = 1'b0;
        endcase

        if (insn[1:0] != 2'b11)
            legal = 1'b0;
        illegal = !legal;
        if (!legal) begin
            writes = 1'b0;
            is_branch = 1'b0; is_jal = 1'b0; is_jalr = 1'b0;
            is_load = 1'b0; is_store = 1'b0; is_atomic = 1'b0; is_mdu = 1'b0; is_csr = 1'b0;
            is_ecall = 1'b0; is_ebreak = 1'b0; is_mret = 1'b0; is_fencei = 1'b0;
            uses_rs1 = 1'b0; uses_rs2 = 1'b0;
        end
    end

    assign rd_we = writes && (rd != 5'd0);
    assign is_div = is_mdu && funct3[2];
    // CSRRW/CSRRWI always write; the set/clear forms only when rs1/uimm != 0
    assign csr_writes = is_csr && ((funct3[1:0] == 2'b01) || (rs1 != 5'd0));
endmodule
