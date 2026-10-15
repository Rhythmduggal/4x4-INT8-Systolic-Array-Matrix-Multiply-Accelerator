# 4x4 INT8 Systolic-Array Matrix-Multiply Accelerator

A parameterized, output-stationary systolic array written in Verilog that computes
**C = A x B** for 4x4 signed INT8 matrices with 32-bit accumulation. Matrix multiplication
is the dominant operation in neural-network inference and training, and a systolic array
is the standard hardware structure for it (Google's TPU and most ML accelerators use the
same idea). This project implements the full block: processing elements, array, control
FSM, operand storage, and a simple host interface, together with a self-checking
testbench and a synthesis script.

---

## Contents

1. [Features](#features)
2. [Directory layout](#directory-layout)
3. [Background: how a systolic array works](#background-how-a-systolic-array-works)
4. [Architecture](#architecture)
5. [Dataflow and timing](#dataflow-and-timing)
6. [Control FSM](#control-fsm)
7. [Interface and usage protocol](#interface-and-usage-protocol)
8. [Parameters](#parameters)
9. [Verification](#verification)
10. [Synthesis results](#synthesis-results)
11. [How to run](#how-to-run)
12. [Design decisions and trade-offs](#design-decisions-and-trade-offs)
13. [Known limitations](#known-limitations)
14. [Roadmap](#roadmap)

---

## Features

- Output-stationary dataflow: each PE keeps its own partial sum, so results never move.
- Signed 8-bit inputs, signed 32-bit accumulators (no overflow for K up to 2^16 of full-scale products).
- Fully parameterized grid (`N`), operand width (`DW`) and accumulator width (`ACC`) in the array and PE.
- Deterministic latency: 13 cycles from `start` to `done`, independent of the data.
- Internal operand storage with a simple write port; writes are ignored while the engine is busy.
- Self-checking testbench with a behavioral golden model, corner cases, random regression, and a latency check.
- Synthesizable, free of latches and of vendor primitives; synthesis script included.

## Directory layout

    sysarray/
    |-- rtl/
    |   |-- pe.v                 processing element (MAC + operand forwarding)
    |   |-- systolic_array.v     NxN grid of PEs built with generate loops
    |   `-- matmul_accel.v       top level: operand RAMs, FSM, skewed feeders, read port
    |-- tb/
    |   `-- tb_matmul_accel.v    self-checking testbench
    |-- syn/
    |   `-- synth.ys             Yosys synthesis script
    |-- Makefile                 sim / wave / synth / clean targets
    `-- README.md

## Background: how a systolic array works

Computing C[i][j] = sum over k of A[i][k] * B[k][j] needs every element of A and B to be
used N times. A naive design would read the same operands from memory repeatedly. A
systolic array avoids this: operands enter at the edges of a grid of simple cells and are
passed neighbor-to-neighbor each cycle, so one memory read is reused across a whole row
or column of cells.

In this design (output-stationary):

- A values enter on the left edge and move **right**, one PE per cycle.
- B values enter on the top edge and move **down**, one PE per cycle.
- Every PE multiplies the A and B values currently passing through it and adds the
  product into its local accumulator.
- After all K terms have passed, PE(i,j) holds C[i][j].

For the right pair of operands to arrive together, the inputs must be **skewed**: row i of
A starts i cycles late and column j of B starts j cycles late.

## Architecture

    B[.][0]   B[.][1]   B[.][2]   B[.][3]        (top edge, skewed by column)
       |         |         |         |
       v         v         v         v
A[0] > PE00 ---> PE01 ---> PE02 ---> PE03        row 0
       |         |         |         |
       v         v         v         v
A[1] > PE10 ---> PE11 ---> PE12 ---> PE13        row 1 (skewed by 1)
       |         |         |         |
       v         v         v         v
A[2] > PE20 ---> PE21 ---> PE22 ---> PE23        row 2 (skewed by 2)
       |         |         |         |
       v         v         v         v
A[3] > PE30 ---> PE31 ---> PE32 ---> PE33        row 3 (skewed by 3)

**`pe.v`** - one processing element. On each clock edge it registers `a_in` to `a_out` and
`b_in` to `b_out` (forwarding the operands) and, when not cleared, performs
`acc <= acc + a_in * b_in` as a signed multiply-accumulate. Reset is asynchronous
(active low); `clear` is a synchronous accumulator clear used between matrices.

**`systolic_array.v`** - instantiates the N x N grid with nested `generate` loops and wires
each PE's outputs to its right and lower neighbors. Ports are flattened vectors
(`a_row`, `b_col`, `c_flat`) so the module works with any simulator or synthesis tool,
including those with limited support for unpacked-array ports.

**`matmul_accel.v`** - the top level. It contains:

- two 16-entry x 8-bit operand memories (matrix A and matrix B, row-major),
- the control FSM and cycle counter `t`,
- the combinational skew logic that selects `A[i][t-i]` for row i and `B[t-j][j]` for column j
  and drives zeros outside the valid window,
- a combinational read port into the array's accumulators.

## Dataflow and timing

Let `t` be the cycle index inside the RUN state (0 to 9). The feeder drives row i with
A[i][t-i] whenever 0 <= t-i < K, and column j with B[t-j][j] whenever 0 <= t-j < K. An
operand takes one extra cycle to cross each PE, so A[i][k] reaches PE(i,j) at cycle
`i + k + j`, and B[k][j] reaches PE(i,j) at cycle `j + k + i`. They arrive together, and the
product is accumulated that cycle.

The last product, A[3][3] x B[3][3], is accumulated in PE(3,3) at cycle 3+3+3 = 9, so the
RUN state lasts K + 2(N-1) = 10 cycles.

Timeline from the `start` pulse:

| Phase | Cycles | What happens |
|---|---|---|
| IDLE to CLEAR | 1 | `start` sampled |
| CLEAR | 1 | all accumulators cleared |
| RUN | 10 | skewed operands stream in, MACs accumulate |
| DONE | 1 | `done` pulse generated; results stable |

The testbench measures **13 cycles** from the start pulse until `done` is observed, and checks
this on every run. Throughput for a single matrix is 64 MACs / 13 cycles (about 4.9 MAC per
cycle) against a peak of 16 MAC per cycle; the gap is the fill/drain time of the array, which
shrinks relative to useful work as K grows or when runs are pipelined (see the roadmap).

## Control FSM

    IDLE --start--> CLEAR --> RUN --(t == 9)--> DONE --> IDLE
                                 ^__ t++ _|

- **IDLE**: waiting; the write port is active; `busy` is low.
- **CLEAR**: asserts `clear` to zero every accumulator.
- **RUN**: asserts the feeders; `t` counts 0 to 9.
- **DONE**: one cycle where `done` is registered high, then return to IDLE.

`busy` is high in every state except IDLE, and operand writes are ignored while it is high,
so an in-flight computation cannot be corrupted.

## Interface and usage protocol

| Signal | Dir | Width | Description |
|---|---|---|---|
| `clk` | in | 1 | Clock |
| `rst_n` | in | 1 | Asynchronous active-low reset |
| `wr_en` | in | 1 | Write enable (ignored while `busy`) |
| `wr_sel` | in | 1 | 0 = write matrix A, 1 = write matrix B |
| `wr_addr` | in | 4 | Element index, `row*4 + col` |
| `wr_data` | in | 8 | Signed INT8 value |
| `start` | in | 1 | Pulse for one cycle to begin |
| `busy` | out | 1 | High from `start` until the FSM returns to IDLE |
| `done` | out | 1 | One-cycle pulse when results are valid |
| `rd_addr` | in | 4 | Result element index, `row*4 + col` |
| `rd_data` | out | 32 | Signed INT32 `C[row][col]` (combinational read) |

Typical sequence:

1. Write all 16 elements of A (`wr_sel=0`) and all 16 of B (`wr_sel=1`).
2. Pulse `start`.
3. Wait for `done`.
4. Set `rd_addr` and read `rd_data` for each of the 16 results.

Results remain valid until the next `start` clears the accumulators.

## Parameters

| Parameter | Default | Where | Meaning |
|---|---|---|---|
| `N` | 4 | array, top | Array dimension (also the reduction length K) |
| `DW` | 8 | all | Operand width in bits |
| `ACC` | 32 | all | Accumulator width in bits |

The array and PE scale with `N`, `DW` and `ACC`. The top-level host interface (4-bit
address ports, `LAST_T`) is written for N = 4; changing N also requires widening the
address ports there.

## Verification

The testbench `tb/tb_matmul_accel.v` is self-checking: it prints a failure for every
mismatch and ends with `RESULT: PASS` or `RESULT: FAIL`.

**Golden model.** A behavioral triple loop in the testbench computes the expected 4x4
result with signed arithmetic. Every output element of every run is compared against it.

**Directed tests**

| Test | Purpose |
|---|---|
| Identity x counting matrix | Output must equal B; catches indexing and skew mistakes |
| All -128 x all -128 | Maximum-magnitude negative operands; checks signed multiply and accumulation |
| +127 x -128 | Maximum positive times maximum negative; checks sign handling |
| All zeros | Checks clear and that no stale data leaks through |

**Random regression.** 500 pairs of random matrices are run back to back with a fixed seed
for reproducibility. Because runs are consecutive with no reset, this also checks that the
accumulator clear works correctly between operations.

**Protocol checks**

- Writes asserted while `busy` must not change the stored operands (verified by comparing
  the final result against the golden model of the original operands).
- The cycle count from `start` to `done` must equal 13 on every run.

**Mutation check.** To confirm the testbench is sensitive to real bugs, the feeder
window condition was deliberately altered (one fewer valid operand per row/column). The
same testbench then reported 8,016 failures, so it is capable of catching skew/timing errors.

**Result:** 505 tests, 0 errors (Icarus Verilog).

## Synthesis results

Synthesized with Yosys 0.33 using `syn/synth.ys` (flatten, `synth`, then `abc` to a generic
gate set of AND/NAND/OR/NOR/XOR/XNOR/MUX). This is **technology-independent**: it is
useful for relative area and register budgeting, not for timing or power sign-off.

| Cell type | Count |
|---|---|
| NAND | 7,478 |
| XOR | 4,725 |
| AND | 2,541 |
| OR | 912 |
| XNOR | 892 |
| MUX | 501 |
| NOT | 123 |
| NOR | 77 |
| Flip-flops (all types) | 968 |
| **Total cells** | **18,217** |

Flip-flop breakdown:

- 704 in the PE array (16 accumulators of 32 bits plus operand-forwarding registers; synthesis
  removed the 64 forwarding bits on the last row and column, which drive nothing),
- 256 for the two 16x8-bit operand memories,
- 8 for control (2-bit state, 5-bit counter, `done`).

Most of the logic is the sixteen 8x8 signed multipliers and 32-bit adders (the XOR/NAND mass).

## How to run

Requires Icarus Verilog (simulation), Yosys (synthesis), and optionally GTKWave.

    make sim      # compile and run the regression, prints RESULT: PASS/FAIL
    make wave     # run, then open tb.vcd in GTKWave
    make synth    # run Yosys; cell statistics are printed from syn/synth.log
    make clean    # remove build products

Useful signals to inspect in the waveform: `dut.state`, `dut.t`, `dut.a_row`, `dut.b_col`,
and `dut.u_array.g_row[3].g_col[3].u_pe.acc` (the last PE to finish).

## Design decisions and trade-offs

- **Output-stationary** was chosen over weight-stationary because it needs no weight
  preload phase and no separate accumulator buffer; results are read straight from the PEs.
  The cost is that both operands stream every run.
- **Internal operand RAM** keeps the demo self-contained. In an SoC these would be fed by a
  DMA or a shared SRAM, which is what the roadmap's bus wrapper is for.
- **Zero-padded skew** instead of per-PE valid signals. Feeding zeros outside the valid
  window is functionally safe (adding 0 changes nothing) and removes per-PE control, at the cost of
  a small amount of wasted toggling.
- **Asynchronous reset with synchronous clear**: reset brings the design to a known state;
  `clear` lets the FSM reuse the array between matrices without a global reset.
- **Flattened ports** on the array for tool portability.

## Known limitations

- Fixed 4x4 problem size at the top level; larger matrices require tiling in software or a
  tiling controller.
- Operands are written one element per cycle (32 cycles to load), which is far slower than the
  compute itself; it is not hidden by double-buffering.
- No overflow or saturation handling beyond the 32-bit accumulator width.
- No back-to-back pipelining of consecutive matrices; each run pays the fill/drain latency.
- Synthesis numbers are generic-gate counts; no timing closure, power, or physical results yet.
- Verification is directed plus random simulation; there is no formal proof or functional
  coverage collection yet.

## Roadmap

1. **Physical implementation:** run through OpenLane/OpenROAD on SKY130 and record area,
   power, worst negative slack, and DRC/LVS status.
2. **UVM verification:** agent, constrained-random sequences, scoreboard against a reference
   model, functional coverage, and SVA for the FSM and valid windows.
3. **Bus interface:** AXI4-Lite register wrapper (control/status, operand windows, result
   window) so a CPU can drive it.
4. **Throughput:** double-buffered operand memories and overlapped runs to hide fill/drain.
5. **Power:** clock gating and operand isolation on idle PEs, with before/after comparison.
6. **Scaling:** parameterize the top level for N and K, and add a tiling controller.
7. **DFT:** scan-chain insertion and fault-coverage reporting.
