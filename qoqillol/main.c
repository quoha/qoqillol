//
//  qoqillol/main.c
//
//  Created by Michael Henderson on 10/15/14.
//  Copyright (c) 2014 Michael D Henderson. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>

typedef struct qocfg qocfg;
typedef struct qotpt qotpt;

typedef unsigned short qocell;

typedef long qovmreg;
typedef struct qovm {
    qocell *pc;
    qocell *startOfCore;
    qocell *endOfCode;
    qocell *endOfCore;
    qocell *ip;
    int     debugLevel;
    int     maxSteps;
    size_t  coreSize;
    qovmreg a, b; // register: accumulators
    qovmreg c;    // program counter
    qovmreg d;    // computed address of current instruction
    qovmreg p, g; // index registers
    qocell  core[1];
} qovm;

// http://www.gtoal.com/languages/bcpl/amiga/bcpl/booting.txt
//
// instruction layout of 16 bits
//   bits 15..12 are the function bits
//   bits 11...8 are the op size in bytes (0 to 15)
//   bit   7     is  the indirection bit
//   bits  6     is  the G index modification bit
//   bits  5     is  the P index modification bit
//   bits  4...0     is  not used
//
#define QOP_RD_FUNC(op) (((op) & 0xf000) >> 12)
#define QOP_RD_SIZE(op) (((op) & 0x0f00) >>  8)
#define QOP_RD_IBIT(op) (((op) & 0x0080) >>  7)
#define QOP_RD_MODG(op) (((op) & 0x0040) >>  6)
#define QOP_RD_MODP(op) (((op) & 0x0020) >>  5)
#define QOP_RD_BITS(op) (((op) & 0x001f)      )
//
#define QOP_WR_FUNC(op) (((op) & 0x0f) << 12)
#define QOP_WR_SIZE(op) (((op) & 0x0f) <<  8)
#define QOP_WR_IBIT(op) (((op) & 0x08) <<  7)
#define QOP_WR_MODG(op) (((op) & 0x04) <<  6)
#define QOP_WR_MODP(op) (((op) & 0x02) <<  5)
#define QOP_WR_BITS(op) (((op) & 0x1f)      )

#define qoalloc(x) qo_alloc(__FILE__, __FUNCTION__, __LINE__, (x))
void *qo_alloc(const char *file, const char *function, int line, size_t size);
qovm *qovm_alloc(size_t coreSize);
void  qovm_dump(qovm *vm);
void  qovm_dump_opcode(size_t address, qocell op);
void  qovm_emit(qovm *vm, qocell op);
void  qovm_exec(qovm *vm);
void  qovm_load_icode(qovm *vm, const char *code);
void  qovm_reset(qovm *vm);
const char *qovm_util_op2mnemonic(qocell op);

void *qo_alloc(const char *file, const char *function, int line, size_t size) {
    void *v = malloc(size);
    if (!v) {
        perror(__FUNCTION__);
        fprintf(stderr, "debug:\t%s %s %d\n", file, function, line);
        fprintf(stderr, "error: out of memory - requested %zu bytes\n", size);
        exit(2);
    }
    return v;
}

qovm *qovm_alloc(size_t coreSize) {
    coreSize = (coreSize < 16) ? 16 : coreSize;
    qovm *vm = qoalloc(sizeof(*vm) + (sizeof(qocell) * coreSize));
    vm->startOfCore = vm->endOfCode = vm->ip = vm->pc = vm->core;
    vm->endOfCore = vm->core + coreSize;
    vm->coreSize = coreSize;
    vm->debugLevel = 10;
    int idx;
    for (idx = 0; idx <= coreSize; idx++) {
        vm->core[idx] = 0xffff;
    }
    vm->maxSteps = 6;
    return vm;
}

void qovm_dump(qovm *vm) {
    printf("...vm: ------------------------------------\n");
    printf(".....: vm             %p\n", vm);
    printf(".....: coreSize       %zu\n", vm->coreSize);
    printf(".....: endOfCode      %ld\n", vm->endOfCode - vm->core);
    printf(".....: debugLevel     %d\n", vm->debugLevel);
    printf(".....: programCounter %p\n", vm->pc);
}

void qovm_dump_opcode(size_t address, qocell op) {
    printf("...vm: ------------------------------------\n");
    printf(".....: address        %zu\n", address);
    printf(".....: op             0x%04x\n", op);
    printf(".....: ...function    0x%04x %s\n", QOP_RD_FUNC(op), qovm_util_op2mnemonic(QOP_RD_FUNC(op)));
    printf(".....: ...opSize      0x%04x\n", QOP_RD_SIZE(op));
    printf(".....: ...indirection 0x%04x\n", QOP_RD_IBIT(op));
    printf(".....: ...gIndexMod   0x%04x\n", QOP_RD_MODG(op));
    printf(".....: ...pIndexMod   0x%04x\n", QOP_RD_MODP(op));
    printf(".....: ...unusedBits  0x%04x\n", QOP_RD_BITS(op));
}

void qovm_emit(qovm *vm, qocell op) {
    // verify that we have space in the core
    //
    if (!(vm->core <= vm->endOfCode && vm->endOfCode < vm->endOfCore)) {
        printf("error: %s %d\n\tcode segment out of range\n", __FUNCTION__, __LINE__);
        qovm_dump(vm);
        exit(2);
    }
    printf(".info: iemit(pc %8ld => 0x%04x)\n", vm->endOfCode - vm->core, op);
    *(vm->endOfCode++) = op;
}

// An instruction is executed as follows.
// Firstly, it is fetched from the store and vm->pc (C) is incremented by the operand size.
// Then, the computed address is formed by assigning the address field to D
// conditionally adding P or G as specified by the modification field, and indirecting if
// required.
// Finally, the operation specified by the function field is performed.
void qovm_exec(qovm *vm) {
    if (vm->maxSteps-- <= 0) {
        printf("error: %s %d\nerror: exceeded step limit\n", __FUNCTION__, __LINE__);
        exit(2);
    }

    // verify that we're executing steps in the core
    //
    if (!(vm->core <= vm->pc && vm->pc < vm->endOfCore)) {
        printf("error: %s %d\n\tprogram counter out of range\n", __FUNCTION__, __LINE__);
        qovm_dump(vm);
        exit(2);
    }

    qovm_dump_opcode(vm->pc - vm->core, *(vm->pc));

    // fetch the instruction from core
    qocell  code    = *(vm->pc++);
    qocell *operand = vm->pc;
    qocell *address = 0;

    qocell opSize            = QOP_RD_SIZE(code);
    qocell function          = QOP_RD_FUNC(code);
    qocell indirectionBit    = QOP_RD_IBIT(code);
    qocell gIndexModBit      = QOP_RD_MODG(code);
    qocell pIndexModBit      = QOP_RD_MODP(code);
    qocell unusedBits        = QOP_RD_BITS(code);

    vm->pc = vm->pc + (opSize >> 4) + 1;

#if 0
    switch (opSize) {
        case 0x00:            // no     operand
            vm->pc += 0;
            break;
        case 0x10:            //  8 bit operand
            vm->pc += 1;
            break;
        case 0x20:            // 32 bit operand
            vm->pc += 4;
            break;
        case 0x30:            // 64 bit operand
            vm->pc += 8;
            break;
    }
#endif
}

void  qovm_load_icode(qovm *vm, const char *code) {
    const char   *mnemonic     = 0;
    unsigned char function     = 0;
    int           indirectBit  = 0;
    int           gIndexModBit = 0;
    int           pIndexModBit = 0;
    unsigned long number       = 0;
    qocell        op;

    while (*code && vm->endOfCode < vm->endOfCore) {
        switch (*code) {
            case '0': number = (number << 4) + 0x00; break;
            case '1': number = (number << 4) + 0x01; break;
            case '2': number = (number << 4) + 0x02; break;
            case '3': number = (number << 4) + 0x03; break;
            case '4': number = (number << 4) + 0x04; break;
            case '5': number = (number << 4) + 0x05; break;
            case '6': number = (number << 4) + 0x06; break;
            case '7': number = (number << 4) + 0x07; break;
            case '8': number = (number << 4) + 0x08; break;
            case '9': number = (number << 4) + 0x09; break;
            case 'A': number = (number << 4) + 0x0a; break;
            case 'B': number = (number << 4) + 0x0b; break;
            case 'C': number = (number << 4) + 0x0c; break;
            case 'D': number = (number << 4) + 0x0d; break;
            case 'E': number = (number << 4) + 0x0e; break;
            case 'F': number = (number << 4) + 0x0f; break;
            case 'i': indirectBit  = 0x01; break;
            case 'g': gIndexModBit = 0x01; break;
            case 'p': pIndexModBit = 0x01; break;
            case 'l': function = 0x00; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'x': function = 0x01; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'a': function = 0x02; mnemonic = qovm_util_op2mnemonic(function); break;
            case 's': function = 0x03; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'k': function = 0x04; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'j': function = 0x05; mnemonic = qovm_util_op2mnemonic(function); break;
            case 't': function = 0x06; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'f': function = 0x07; mnemonic = qovm_util_op2mnemonic(function); break;
            case ';':
                // comment to the end of the line
                while (*code && !(*code == '\n')) {
                    code++;
                }
                if (*code) {
                    code++;
                }
                break;
            default: // ignore all unknown input
                break;
        }
        if (mnemonic) {
            op  = QOP_WR_FUNC(function);
            op ^= QOP_WR_IBIT(indirectBit);
            op ^= QOP_WR_MODG(indirectBit);
            op ^= QOP_WR_MODP(indirectBit);
            printf(".info: icode(%-5s 0x%04x number => %8lu/0x%08lx)\n", mnemonic, op, number, number);
            qovm_emit(vm, op);

            // reset everything to prepare for the next instruction
            //
            function = 0;
            mnemonic = 0;
            number   = 0;
            indirectBit = gIndexModBit = pIndexModBit = 0;
        }
        code++;
    }
    if (*code) {
        printf("error:\t%s %d\nerror: out of core memory\n", __FUNCTION__, __LINE__);
        exit(2);
    }
}

void qovm_reset(qovm *vm) {
    vm->pc = vm->core;
}

// op bits hex function
//  l 0000 x00 load from core
//  x 0001 x01 execute operation
//  a 0010 x02 add
//  s 0011 x03 store to core
//  k 0100 x04 call
//  j 0101 x05 jump always
//  t 0110 x06 jump if true
//  f 0111 x07 jump if false
//
const char *qovm_util_op2mnemonic(qocell op) {
    switch (op) {
        case 0x00: return "load";
        case 0x01: return "exop";
        case 0x02: return "add";
        case 0x03: return "store";
        case 0x04: return "call";
        case 0x05: return "jmp";
        case 0x06: return "jmpt";
        case 0x07: return "jmpf";
    }
    printf("error:\t%s %d\nerror: unknown function 0x%04x\n", __FUNCTION__, __LINE__, QOP_RD_FUNC(op));
    exit(2);
    /* NOT REACHED */
    return "unknown";
}

int main(int argc, const char * argv[]) {
    printf(".info: sizeof(char     ) %3lu bytes\n", sizeof(char     ));
    printf(".info: sizeof(short    ) %3lu bytes\n", sizeof(short    ));
    printf(".info: sizeof(long     ) %3lu bytes\n", sizeof(long     ));
    printf(".info: sizeof(long long) %3lu bytes\n", sizeof(long long));
    printf(".info: sizeof(float    ) %3lu bytes\n", sizeof(float    ));
    printf(".info: sizeof(double   ) %3lu bytes\n", sizeof(double   ));
    printf(".info: sizeof(pointer  ) %3lu bytes\n", sizeof(void *   ));

    qovm *vm = qovm_alloc(64);
    qovm_dump(vm);

    qovm_load_icode(vm, "px ga pgs kjtf FFl 1913l ; comments welcome");
    //qovm_dump(vm);

    int idx;
    for (idx = 0; idx < 100; idx++) {
        qovm_exec(vm);
    }
    return 0;
}
