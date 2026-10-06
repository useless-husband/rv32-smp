# rv32-smp -- run every target from the repository root.
#
# Every path below is relative on purpose: the checkout may live in a
# directory whose name contains spaces or non-ASCII characters, which GNU make
# (and Verilator's generated makefiles) cannot handle in absolute paths.  For
# the same reason the Verilator models are compiled with direct compiler
# calls instead of Verilator's own makefiles.

SHELL := /bin/bash
PYTHON ?= python3
VERILATOR ?= verilator
YOSYS ?= yosys
CC ?= cc
CXX ?= c++
JOBS ?= 4

CLANG ?= $(if $(wildcard /opt/homebrew/opt/llvm/bin/clang),/opt/homebrew/opt/llvm/bin/clang,clang)
LLD ?= $(if $(wildcard /opt/homebrew/opt/lld/bin/ld.lld),/opt/homebrew/opt/lld/bin/ld.lld,ld.lld)

RISCV_TESTS_URL := https://github.com/riscv-software-src/riscv-tests
RISCV_TESTS_SHA := bcffa2b3188b040c611f90dc0b6e422f54775a09
TP := build/third_party

RVARCH := --target=riscv32-unknown-elf -march=rv32ima_zicsr_zifencei -mabi=ilp32
RVCFLAGS := $(RVARCH) -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -Wall -Isw/runtime -Imodel
RVLDFLAGS := -T sw/runtime/link.ld --gc-sections

.PHONY: all test clean
all: test
.SECONDARY:

# ----------------------------------------------------------- golden model
ISS_SRC := model/rv_iss.c model/rv_fp.c model/disasm.c
ISS_HDR := model/rv_iss.h model/rv_fp.h model/rv_platform.h
build/rvsim: $(ISS_SRC) model/rvsim.c $(ISS_HDR)
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -o $@ $(ISS_SRC) model/rvsim.c

ISS_OBJ := build/iss/rv_iss.o build/iss/rv_fp.o build/iss/disasm.o
build/iss/%.o: model/%.c $(ISS_HDR)
	@mkdir -p build/iss
	$(CC) -std=c11 -O2 -Wall -c -o $@ $<

# ------------------------------------------------- protocol model checker
build/mc: model/mc.cpp model/mesi.hpp
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -o $@ model/mc.cpp

# ---------------------------------------------------------- bare-metal sw
RT_OBJ := build/sw/crt0.o build/sw/rt.o build/sw/smp.o
build/sw/crt0.o: sw/runtime/crt0.S model/rv_platform.h
	@mkdir -p build/sw
	$(CLANG) $(RVCFLAGS) -c -o $@ $<
build/sw/%.o: sw/runtime/%.c sw/runtime/rt.h sw/runtime/smp.h model/rv_platform.h
	@mkdir -p build/sw
	$(CLANG) $(RVCFLAGS) -c -o $@ $<
build/sw/%.elf: sw/demo/%.c $(RT_OBJ) sw/runtime/link.ld sw/runtime/smp.h
	$(CLANG) $(RVCFLAGS) -c -o build/sw/$*.o $<
	$(LLD) $(RVLDFLAGS) -o $@ $(RT_OBJ) build/sw/$*.o
build/sw/%.elf: tests/programs/%.c $(RT_OBJ) sw/runtime/link.ld sw/runtime/smp.h
	$(CLANG) $(RVCFLAGS) -c -o build/sw/$*.o $<
	$(LLD) $(RVLDFLAGS) -o $@ $(RT_OBJ) build/sw/$*.o
build/sw/%.elf: tests/museum/%.c $(RT_OBJ) sw/runtime/link.ld sw/runtime/smp.h
	$(CLANG) $(RVCFLAGS) -c -o build/sw/$*.o $<
	$(LLD) $(RVLDFLAGS) -o $@ $(RT_OBJ) build/sw/$*.o

PROGRAMS := $(patsubst sw/demo/%.c,build/sw/%.elf,$(wildcard sw/demo/*.c)) \
            $(patsubst tests/programs/%.c,build/sw/%.elf,$(wildcard tests/programs/*.c)) \
            $(patsubst tests/museum/%.c,build/sw/%.elf,$(wildcard tests/museum/*.c))
sw: $(PROGRAMS)

clean:
	rm -rf build

# ------------------------------------------------------- riscv-tests (ISA)
# The lists below are the pinned commit's isa/rv32{ui,um,ua}/Makefrag;
# tests/system/test_riscv_tests.py checks they match.
RV32UI := simple add addi and andi auipc beq bge bgeu blt bltu bne fence_i jal jalr \
          lb lbu lh lhu lw ld_st lui ma_data or ori sb sh sw st_ld sll slli slt slti sltiu sltu \
          sra srai srl srli sub xor xori
RV32UM := div divu mul mulh mulhsu mulhu rem remu
RV32UA := amoadd_w amoand_w amomax_w amomaxu_w amomin_w amominu_w amoor_w amoswap_w amoxor_w lrsc
RVTESTS := $(addprefix rv32ui-,$(RV32UI)) $(addprefix rv32um-,$(RV32UM)) $(addprefix rv32ua-,$(RV32UA))
RVTEST_ELFS := $(addprefix build/rvtests/,$(addsuffix .elf,$(RVTESTS)))
ENV_OBJ := build/rvtests/env/trap_entry.o build/rvtests/env/trap.o

$(TP)/riscv-tests/.stamp:
	rm -rf $(TP)/riscv-tests && mkdir -p $(TP)/riscv-tests
	cd $(TP)/riscv-tests && git init -q && git fetch -q --depth 1 $(RISCV_TESTS_URL) $(RISCV_TESTS_SHA) \
	  && git checkout -q FETCH_HEAD
	@touch $@

build/rvtests/env/trap_entry.o: tests/env/trap_entry.S
	@mkdir -p build/rvtests/env
	$(CLANG) $(RVCFLAGS) -c -o $@ $<
build/rvtests/env/trap.o: tests/env/trap.c model/rv_platform.h
	@mkdir -p build/rvtests/env
	$(CLANG) $(RVCFLAGS) -c -o $@ $<

build/rvtests/rv32u%.elf: $(TP)/riscv-tests/.stamp tests/env/riscv_test.h $(ENV_OBJ) sw/runtime/link.ld
	@mkdir -p build/rvtests
	$(CLANG) $(RVCFLAGS) -Itests/env -I$(TP)/riscv-tests/isa/macros/scalar \
	  -c -o build/rvtests/rv32u$*.o $(TP)/riscv-tests/isa/rv32u$(firstword $(subst -, ,$*))/$(lastword $(subst -, ,$*)).S
	$(LLD) $(RVLDFLAGS) -o $@ build/rvtests/rv32u$*.o $(ENV_OBJ)

rvtests: $(RVTEST_ELFS)

.PHONY: iss-test rvtests sw
iss-test: build/rvsim rvtests
	@pass=0; fail=0; for t in $(RVTESTS); do \
	  if ./build/rvsim --quiet build/rvtests/$$t.elf; then pass=$$((pass+1)); else echo "FAIL $$t"; fail=$$((fail+1)); fi; \
	done; echo "golden model: $$pass passed, $$fail failed"; [ $$fail -eq 0 ]

# ------------------------------------------------------ Verilator models
# build/vsmpN: N harts, the correct protocol; build/vsmp2-bugK: two harts
# with protocol variant K (rtl/rv_defs.svh, BUG_*).
VROOT := $(shell $(VERILATOR) --getenv VERILATOR_ROOT 2>/dev/null)
VDEFS := -DVM_COVERAGE=0 -DVM_SC=0 -DVM_TIMING=0 -DVM_TRACE=0 -DVM_TRACE_FST=0 -DVM_TRACE_VCD=0 \
         -DVM_TRACE_SAIF=0
VFLAGS := --cc -O3 --x-assign fast --x-initial fast --noassert -Irtl --prefix Vtop -Wno-fatal
RTL := rtl/decoder.sv rtl/alu.sv rtl/regfile.sv rtl/lsu_align.sv rtl/csr_file.sv rtl/bpred.sv rtl/divider.sv \
       rtl/icache.sv rtl/l1d.sv rtl/core.sv rtl/bus.sv
SIM_RTL := $(RTL) rtl/sim/mem_model.sv rtl/sim/smp_top.sv
SIM_CXX_DEPS := sim/smp_main.cpp sim/trace.h sim/memcheck.h model/mesi.hpp $(ISS_OBJ) rtl/rv_defs.svh
VSIM_CXX = $(CXX) -std=c++17 -O2 -w $(VDEFS) -DMESI_AGREEMENT -Imodel -Isim -Ibuild/$(1) -I$(VROOT)/include \
  -I$(VROOT)/include/vltstd build/$(1)/*.cpp $(VROOT)/include/verilated.cpp \
  $(VROOT)/include/verilated_threads.cpp sim/smp_main.cpp $(ISS_OBJ)
# extra -G overrides, e.g. SMP_G="-GMEM_LATENCY=30"
SMP_G ?=

build/vsmp%: $(SIM_RTL) $(SIM_CXX_DEPS)
	rm -rf build/vm_smp$*
	mkdir -p build/vm_smp$*
	$(VERILATOR) $(VFLAGS) -Mdir build/vm_smp$* --top-module smp_top \
	  -GNHARTS=$(firstword $(subst -bug, ,$*)) $(if $(findstring -bug,$*),-GBUG=$(lastword $(subst -bug, ,$*)),) \
	  $(SMP_G) $(SIM_RTL)
	$(call VSIM_CXX,vm_smp$*) -o $@ -lpthread

SIMS := build/vsmp1 build/vsmp2 build/vsmp4
BUGS := 1 2 3 4 5
BUG_SIMS := $(addprefix build/vsmp2-bug,$(BUGS))
sims: $(SIMS)

# ------------------------------------------------------------------ lint
.PHONY: lint loopcheck sims
lint:
	for n in 1 2 4; do $(VERILATOR) --lint-only -Wall -Irtl --top-module smp_top -GNHARTS=$$n $(SIM_RTL) || exit 1; done
	for b in $(BUGS); do $(VERILATOR) --lint-only -Wall -Irtl --top-module smp_top -GBUG=$$b $(SIM_RTL) || exit 1; done
	@echo "lint: verilator -Wall clean (1, 2, 4 harts; every bug variant)"

# yosys check -assert: no combinational loops, no multiple drivers
loopcheck:
	@mkdir -p build/synth
	for n in 1 2 4; do \
	  $(YOSYS) -q -l build/synth/check$$n.log -p "read_verilog -sv -Irtl $(RTL) synth/smp_core.sv; \
	    chparam -set NHARTS $$n smp_core; hierarchy -top smp_core; proc; flatten; opt_clean; check -assert" \
	    > /dev/null 2>&1 || { grep -iE "loop|warning|error" build/synth/check$$n.log | head -20; exit 1; }; done
	@echo "loopcheck: no combinational loops, no multiple drivers (yosys check -assert, 1/2/4 harts)"

# ----------------------------------------------------------------- tests
.PHONY: mc mc-bugs system soak litmus museum bench mutants synth report demo
mc: build/mc
	@mkdir -p build/museum
	./build/mc --all | tee build/mc.md

# fetch the litmus suite (reference results included) into the gitignored data/
.PHONY: litmus-fetch litmus
litmus-fetch:
	@test -f data/litmus/model-results/flat.logs || \
	  { mkdir -p data && rm -rf data/litmus && \
	    git clone -q --depth 1 https://github.com/litmus-tests/litmus-tests-riscv data/litmus && \
	    echo "fetched litmus-tests-riscv into data/litmus"; }

litmus: build/vsmp4 build/sw/litmus.elf litmus-fetch
	./build/vsmp4 --seed 7 --stall-pct 20 --jitter 8 --max-cycles 80000000 build/sw/litmus.elf | tee build/litmus.json

system: $(SIMS) $(BUG_SIMS) rvtests sw build/rvsim
	$(PYTHON) -m pytest -q tests/system

test: lint loopcheck iss-test mc system
