#!/usr/bin/env python3
"""Z3 bit-vector proofs for integer helpers used by generated EE code (docs/architecture/fuzzing.md, Step 3).

Each proof states a specification independently of the C++ formula, encodes the C++ formula as transcribed from
the source (ps2xRuntime/include/ps2_runtime_macros.h, ps2xRecomp/src/lib/*_translator.cpp), and asks Z3 for a
counterexample to "formula == specification" over every input. "proved" means unsat: no counterexample exists.

Scope, stated so that nothing more is read into it:
  * integer semantics only; no floating-point equivalence follows from any of these proofs;
  * the transcription of the C++ formula into Z3 is by hand: a proof covers the formula as written here, and the
    unit tests (ps2_runtime_expansion_tests.cpp, "integer overflow helpers") pin the C++ to the same input/output
    pairs at the boundaries;
  * that the C++ is free of undefined behaviour is not proved here; it is shown by computing in unsigned types and
    by UBSan in CI (docs/architecture/fuzzing.md).
Every proof has a negative control: a deliberately wrong variant that Z3 must refute, so a vacuous encoding fails.

usage: z3_int_semantics.py   (exit 0 iff every proof holds and every control is refuted; 77 = z3 not installed,
                               reported by ctest as SKIPPED, never as passed)
"""
import sys

try:
    import z3
except ImportError:
    print("z3 (pip package z3-solver) is not installed: proofs NOT run")
    sys.exit(77)

RAM_SIZE = 32 * 1024 * 1024  # PS2_RAM_SIZE (ps2xRuntime/include/runtime/ps2_memory.h)
RAM_MASK = RAM_SIZE - 1


def prove(name, claim):
    s = z3.Solver()
    s.add(z3.Not(claim))
    r = s.check()
    if r == z3.unsat:
        return True, f"proved   {name}"
    if r == z3.sat:
        return False, f"FAILED   {name}: counterexample {s.model()}"
    return False, f"UNKNOWN  {name}: {r}"


def refuted(name, claim):
    """Negative control: the claim must have a counterexample."""
    s = z3.Solver()
    s.add(z3.Not(claim))
    r = s.check()
    return (r == z3.sat), (f"refuted  {name} (control)" if r == z3.sat else f"FAILED   control {name} not refuted: {r}")


def signed_overflow_spec(a, b, w, op):
    """Exact result outside [-2^(w-1), 2^(w-1)-1], computed in w+1 bits (cannot itself overflow)."""
    ea, eb = z3.SignExt(1, a), z3.SignExt(1, b)
    exact = ea + eb if op == "+" else ea - eb
    lo = z3.SignExt(1, z3.BitVecVal(1 << (w - 1), w))      # -2^(w-1)
    hi = z3.SignExt(1, z3.BitVecVal((1 << (w - 1)) - 1, w))  #  2^(w-1)-1
    return z3.Or(exact < lo, exact > hi)


def ov_formula(a, b, w, op):
    """ADD32_OV / SUB32_OV / ADD64_OV / SUB64_OV as written (unsigned arithmetic, sign bit of a mask)."""
    sign = z3.BitVecVal(1 << (w - 1), w)
    r = a + b if op == "+" else a - b
    if op == "+":
        m = ~(a ^ b) & (a ^ r)
    else:
        m = (a ^ b) & (a ^ r)
    return (m & sign) != 0, r


def proofs():
    out = []
    for w in (32, 64):
        a, b = z3.BitVecs(f"a{w} b{w}", w)
        for op in ("+", "-"):
            ov, r = ov_formula(a, b, w, op)
            name = f"{'ADD' if op == '+' else 'SUB'}{w}_OV overflow flag == exact result out of signed range"
            out.append(prove(name, ov == signed_overflow_spec(a, b, w, op)))
            exact_low = z3.Extract(w - 1, 0, (z3.SignExt(1, a) + z3.SignExt(1, b)) if op == "+"
                                   else (z3.SignExt(1, a) - z3.SignExt(1, b)))
            out.append(prove(f"{'ADD' if op == '+' else 'SUB'}{w}_OV result == low {w} bits of the exact result",
                             r == exact_low))
            # control: the flag without the sign-agreement term is wrong
            wrong = ((a ^ r) & z3.BitVecVal(1 << (w - 1), w)) != 0
            out.append(refuted(f"{op}{w} flag without operand-sign term", wrong == signed_overflow_spec(a, b, w, op)))

    # ADDI / DADDI: the immediate is sign-extended before the add (MIPS: GPR[rs] + sign_extend(imm16)).
    rs32 = z3.BitVec("rs32", 32)
    rs64 = z3.BitVec("rs64", 64)
    imm = z3.BitVec("imm16", 16)
    ov, _ = ov_formula(rs32, z3.SignExt(16, imm), 32, "+")
    out.append(prove("ADDI flag with sign-extended imm16", ov == signed_overflow_spec(rs32, z3.SignExt(16, imm), 32, "+")))
    ov, r = ov_formula(rs64, z3.SignExt(48, imm), 64, "+")
    out.append(prove("DADDI flag with sign-extended imm16", ov == signed_overflow_spec(rs64, z3.SignExt(48, imm), 64, "+")))
    # DADDIU: GPR_U64 + (uint64_t)(int64_t)(int32_t)simm  ==  rs + sign_extend(imm16)  (mod 2^64)
    simm32 = z3.SignExt(16, imm)
    out.append(prove("DADDIU wraps as rs + sign_extend(imm16)", rs64 + z3.SignExt(32, simm32) == rs64 + z3.SignExt(48, imm)))
    out.append(refuted("DADDIU with zero-extended imm", rs64 + z3.ZeroExt(48, imm) == rs64 + z3.SignExt(48, imm)))

    # PLZCW: number of leading bits equal to the sign bit, excluding the sign bit itself (spec), against
    # ps2_plzcw32 (special cases, invert when negative, countl_zero - 1).
    x = z3.BitVec("x", 32)
    sign = z3.Extract(31, 31, x)
    runs = [z3.And(*[z3.Extract(31 - j, 31 - j, x) == sign for j in range(1, k + 1)]) for k in range(1, 32)]
    spec = z3.Sum(*[z3.If(c, z3.BitVecVal(1, 32), z3.BitVecVal(0, 32)) for c in runs])
    y = z3.If(sign == 1, ~x, x)
    clz = z3.Sum(*[z3.If(z3.Extract(31, 32 - k, y) == 0, z3.BitVecVal(1, 32), z3.BitVecVal(0, 32)) for k in range(1, 33)])
    impl = z3.If(z3.Or(x == 0, x == 0xFFFFFFFF), z3.BitVecVal(31, 32), clz - 1)
    out.append(prove("ps2_plzcw32 == leading sign-bit copies", impl == spec))
    out.append(refuted("plzcw without the -1", clz == spec))

    # Fast RDRAM paths: the contiguous test admits only in-range accesses; the wrapped path indexes in range.
    addr = z3.BitVec("addr", 32)
    for size in (2, 4, 8, 16):
        offset = addr & RAM_MASK if size != 16 else addr & RAM_MASK & ~z3.BitVecVal(0xF, 32)
        contiguous = z3.ULE(offset, z3.BitVecVal(RAM_SIZE - size, 32))
        end = z3.ZeroExt(1, offset) + size
        out.append(prove(f"Ps2FastRead/Write{size * 8}: contiguous => offset + {size} <= RAM_SIZE",
                         z3.Implies(contiguous, z3.ULE(end, RAM_SIZE))))
        i = z3.BitVec(f"i{size}", 32)
        out.append(prove(f"Ps2FastRead/Write{size * 8}: wrapped index < RAM_SIZE",
                         z3.Implies(z3.ULT(i, size), z3.ULT((offset + i) & RAM_MASK, RAM_SIZE))))
    out.append(refuted("contiguous test without the '- bytes' term",
                       z3.Implies(z3.ULE(addr & RAM_MASK, RAM_MASK), z3.ULE(z3.ZeroExt(1, addr & RAM_MASK) + 4, RAM_SIZE))))

    # DIV / DIV1: the C division is reached only when it is defined (divisor != 0, not INT32_MIN / -1).
    n, d = z3.BitVecs("n d", 32)
    reached = z3.And(d != 0, z3.Not(z3.And(d == 0xFFFFFFFF, n == 0x80000000)))
    defined = z3.And(d != 0, z3.Not(z3.And(d == -1, n == -(1 << 31))))
    out.append(prove("DIV: C '/' and '%' reached only with defined operands", z3.Implies(reached, defined)))
    out.append(refuted("DIV guard without the INT32_MIN/-1 case", z3.Implies(d != 0, defined)))
    return out


def main():
    results = proofs()
    for _, line in results:
        print(line)
    failed = [line for ok, line in results if not ok]
    print(f"{len(results) - len(failed)}/{len(results)} hold")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
