//
//  qoqillol/main.c
//
//  Created by Michael Henderson on 10/15/14.
//  Copyright (c) 2014 Michael D Henderson. All rights reserved.
//

#include <stdio.h>
#include <string.h>
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

// http://www.cl.cam.ac.uk/~mr10/bcplman.pdf
//
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
#define QOP_WR_FUNC(op) (((op) & 0x0f  ) << 12)
#define QOP_WR_SIZE(op) (((op) & 0x0f  ) <<  8)
#define QOP_WR_IBIT(op) (((op) & 0x08  ) <<  7)
#define QOP_WR_MODG(op) (((op) & 0x04  ) <<  6)
#define QOP_WR_MODP(op) (((op) & 0x02  ) <<  5)
#define QOP_WR_BITS(op) (((op) & 0x1f  )      )

#define qoalloc(x) qo_alloc(__FILE__, __FUNCTION__, __LINE__, (x))
void *qo_alloc(const char *file, const char *function, int line, size_t size);
qovm *qovm_alloc(size_t coreSize, int debugLevel, int maxSteps);
void  qovm_dump(qovm *vm);
void  qovm_dump_opcode(qovm *vm, qocell *op);
void  qovm_emit(qovm *vm, qocell op, void *data);
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

qovm *qovm_alloc(size_t coreSize, int debugLevel, int maxSteps) {
    coreSize = (coreSize < 16) ? 16 : coreSize;
    qovm *vm = qoalloc(sizeof(*vm) + (sizeof(qocell) * coreSize));
    vm->startOfCore = vm->endOfCode = vm->ip = vm->pc = vm->core;
    vm->endOfCore = vm->core + coreSize;
    vm->coreSize = coreSize;
    vm->debugLevel = debugLevel;
    vm->maxSteps = -1;

    if (vm->debugLevel) {
        int idx;
        for (idx = 0; idx <= coreSize; idx++) {
            vm->core[idx] = 0xffff;
        }
        vm->maxSteps = maxSteps;
    }

    return vm;
}

void qovm_dump(qovm *vm) {
    printf("...vm: ------------------------------------\n");
    printf(".....: vm             %p\n", vm);
    printf(".....: coreSize       %zu\n", vm->coreSize);
    printf(".....: endOfCode      %ld\n", vm->endOfCode - vm->core);
    printf(".....: debugLevel     %d\n", vm->debugLevel);
    printf(".....: startOfCore    %p\n", vm->startOfCore);
    printf(".....: programCounter %p  %8ld\n", vm->pc, vm->pc - vm->startOfCore);
    printf(".....: endOfCore      %p\n", vm->endOfCore);
}

void qovm_dump_cell(qocell op) {
}

void qovm_dump_opcode(qovm *vm, qocell *op) {
    printf("...vm: ------------------------------------\n");
    printf(".....: address        %zu\n", op - vm->core);
    printf(".....: op             0x%04x\n", *op);
    printf(".....: ...function    0x%04x %s\n", QOP_RD_FUNC(*op), qovm_util_op2mnemonic(QOP_RD_FUNC(*op)));
    printf(".....: ...opSize      0x%04x (cells, not bytes)\n", QOP_RD_SIZE(*op));
    printf(".....: ...indirection 0x%04x\n", QOP_RD_IBIT(*op));
    printf(".....: ...gIndexMod   0x%04x\n", QOP_RD_MODG(*op));
    printf(".....: ...pIndexMod   0x%04x\n", QOP_RD_MODP(*op));
    printf(".....: ...unusedBits  0x%04x\n", QOP_RD_BITS(*op));

    int idx;
    for (idx = 1; idx <= QOP_RD_SIZE(*op); idx++) {
        printf(".....: ......operand  0x%04x\n", op[idx]);
    }
}

void qovm_emit(qovm *vm, qocell op, void *data) {
    int bytesToCopy = QOP_RD_SIZE(op);

    // verify that we have space in the core
    //
    if (!(vm->core <= vm->endOfCode && vm->endOfCode + bytesToCopy < vm->endOfCore)) {
        printf("error: %s %d\n\tcode segment out of range\n", __FUNCTION__, __LINE__);
        qovm_dump(vm);
        exit(2);
    }
    printf(".info: iemit(pc %8ld => 0x%04x)\n", vm->endOfCode - vm->core, op);

    // emit the instruction
    //
    *(vm->endOfCode++) = op;

    // TODO: emit any data attached to the instruction
    //
    // the instruction may span multiple cells.
    // bump the program counter to account for that.
    //
    memcpy(vm->endOfCode, data, bytesToCopy);
    vm->endOfCode += bytesToCopy;
}

// An instruction is executed as follows.
// Firstly, it is fetched from the store and vm->pc (C) is incremented by the operand size.
// Then, the computed address is formed by assigning the address field to D
// conditionally adding P or G as specified by the modification field, and indirecting if
// required.
// Finally, the operation specified by the function field is performed.
void qovm_exec(qovm *vm) {
    if (vm->debugLevel && vm->maxSteps != -1) {
        if (vm->maxSteps-- <= 0) {
            printf("error: %s %d\nerror: exceeded step limit\n", __FUNCTION__, __LINE__);
            exit(2);
        }
    }

    // verify that we're executing steps in the core
    //
    if (!(vm->core <= vm->pc && vm->pc < vm->endOfCore)) {
        printf("error: %s %d\n\tprogram counter out of range\n", __FUNCTION__, __LINE__);
        qovm_dump(vm);
        exit(2);
    }

    qovm_dump_opcode(vm, vm->pc);

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

    // the instruction may span multiple cells. the cells store data for the
    // particular instruction. bump the program counter to account for that.
    //
    vm->pc += opSize;
}

void  qovm_load_icode(qovm *vm, const char *code) {
    unsigned char function     = 0;
    int           indirectBit  = 0;
    int           gIndexModBit = 0;
    int           pIndexModBit = 0;
    const char   *mnemonic     = 0;
    unsigned long number       = 0;
    int           numNibs      = 0;
    qocell        opSize       = 0;
    qocell        op;
    unsigned char  data[sizeof(qocell) * 4];
    unsigned char *byte = data;

    while (*code && vm->endOfCode < vm->endOfCore) {
        switch (*code) {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                if (numNibs++ == 2) {
                    byte[0] = byte[0] << 4;
                    numNibs = 0;
                }
                byte[0] = byte[0] & (*code - '0');
                break;
            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
                if (numNibs++ == 2) {
                    byte[0] = byte[0] << 4;
                    numNibs = 0;
                }
                byte[0] = byte[0] & (*code - '0');
                break;
//            case '0': number = (number << 4) + 0x00; numNibs++; break;
//            case '1': number = (number << 4) + 0x01; numNibs++; break;
//            case '2': number = (number << 4) + 0x02; numNibs++; break;
//            case '3': number = (number << 4) + 0x03; numNibs++; break;
//            case '4': number = (number << 4) + 0x04; numNibs++; break;
//            case '5': number = (number << 4) + 0x05; numNibs++; break;
//            case '6': number = (number << 4) + 0x06; numNibs++; break;
//            case '7': number = (number << 4) + 0x07; numNibs++; break;
//            case '8': number = (number << 4) + 0x08; numNibs++; break;
//            case '9': number = (number << 4) + 0x09; numNibs++; break;
//            case 'A': number = (number << 4) + 0x0a; numNibs++; break;
//            case 'B': number = (number << 4) + 0x0b; numNibs++; break;
//            case 'C': number = (number << 4) + 0x0c; numNibs++; break;
//            case 'D': number = (number << 4) + 0x0d; numNibs++; break;
//            case 'E': number = (number << 4) + 0x0e; numNibs++; break;
//            case 'F': number = (number << 4) + 0x0f; numNibs++; break;
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
            // opSize is the number of cells of data needed. while we can
            // have up to 16 cells, we don't. i'm not sure why. maybe something
            // to do with not wanting to store anything larger than the largest
            // c-type value? TODO: think on this decision. opSize must always
            // be in number of opcells to avoid possibility of data alignement
            // issues.
            //
            if (numNibs == 0) {
                opSize = 0x00;
            } else if (numNibs <= 4) {
                opSize = 0x01;
            } else if (numNibs <= 8) {
                opSize = 0x02;
            } else if (numNibs <= 12) {
                opSize = 0x03;
            } else {
                // TODO: warn of the truncation?
                opSize = 0x04;
            }

            op  = QOP_WR_FUNC(function);
            op ^= QOP_WR_SIZE(opSize);
            op ^= QOP_WR_IBIT(indirectBit);
            op ^= QOP_WR_MODG(indirectBit);
            op ^= QOP_WR_MODP(indirectBit);

            printf(".info: icode(%-5s 0x%04x %2d number => %8lu/0x%08lx)\n", mnemonic, op, opSize, number, number);
            qovm_emit(vm, op, &number);

            // reset everything to prepare for the next instruction
            //
            function = 0;
            mnemonic = 0;
            number   = 0;
            numNibs  = 0;
            opSize   = 0;
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
    printf(".info: sizeof(qocellr  ) %3lu bytes\n", sizeof(qocell   ));

    qovm *vm = qovm_alloc(16 * 1024, 10, 8);
    qovm_dump(vm);

    qovm_load_icode(vm, "0000l 00px EEga FFpgs AAk DEADBEEFCAFEF00Dj CAFE t DDDDf FFl 1913l ; comments welcome");
    //qovm_dump(vm);

    int idx;
    for (idx = 0; idx < 100; idx++) {
        qovm_exec(vm);
    }
    return 0;
}
