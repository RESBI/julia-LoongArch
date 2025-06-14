// This file is a part of Julia. License is MIT: https://julialang.org/license
// LoongArch64 version by a helpful assistant.

#include "common.h"
#include "../../src/jl_exported_funcs.inc"

#define SEP ;

// The temporary register used to hold the target address.
// According to the LoongArch psABI, $t0-$t8 ($12-$20) are temporary registers.
// We use $t0 ($12) here.
#define TRAMPOLINE_REG $t0

#define XX(name) \
.global CNAME(name) SEP \
.cfi_startproc SEP \
.p2align    2 SEP \
 CNAME(name)##: SEP \
    pcaddu12i TRAMPOLINE_REG, %pcrel_hi(CNAMEADDR(name)) SEP \
    ld.d      TRAMPOLINE_REG, TRAMPOLINE_REG, %pcrel_lo(CNAME(name)) SEP \
    jirl      $zero, TRAMPOLINE_REG, 0 SEP \
.cfi_endproc SEP

JL_RUNTIME_EXPORTED_FUNCS(XX)
JL_CODEGEN_EXPORTED_FUNCS(XX)
#undef XX
#undef TRAMPOLINE_REG
