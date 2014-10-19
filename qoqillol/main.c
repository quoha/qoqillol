//
//  qoqillol/main.c
//
//  Created by Michael Henderson on 10/15/14.
//  Copyright (c) 2014 Michael D Henderson. All rights reserved.
//

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct qocfg qocfg;
typedef struct qotpt qotpt;

typedef unsigned short qocell;

typedef long qovmreg;

// stacks
//   globalVariables
//   programCall
// registers
//   pc
//   a    accumulator
//   b    auxiliary accumulator
//   c    control register
//   d    address register
//   p    stack frame pointer/index
//   g    global variable base pointer
// core
//
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
    qocell  globalVariableStack[512];
    qocell  programCallStack[512];
    qocell  core[1];
} qovm;

// http://www.cl.cam.ac.uk/~mr10/bcplman.pdf
//
// http://www.gtoal.com/languages/bcpl/amiga/bcpl/booting.txt
//
// instruction layout assuming qocell to be 16 bits.
//   bits 15..12 are the function bits
//   bit  11     is  the D bit, D loaded from following qcell
//   bit  10     is  the P bit, P to be added to D
//   bit   9     is  the G bit, G to be added to D
//   bit   8     is  the I bit, D loaded indirectly from D
//   bits  7...0 are used in various ways
//               short address (8 bits, range -127..+128)
//               short jump    (8 bits, range -127..+128)
// note that with a 16 bit address, core is limited to 65k cells
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
int   qovm_hex_to_data_le(const char *startOfHex, size_t length, unsigned char *data);
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
    if (bytesToCopy) {
        qocell *cell = data;
        while (bytesToCopy-- > 0) {
            *(vm->endOfCode++) = *(cell++);
        }
    }
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

// should understand endianness. this version is little-endian:
//
//   0x0102030405060708  ==>  0x0807060504030201
//
int qovm_hex_to_data_le(const char *hex, size_t length, unsigned char *data) {
    size_t maxLength = 15 * sizeof(qocell);
    if (length > maxLength) {
        printf("error: hex data must not exceed 15 cells/(%zu bytes)\n", maxLength);
        exit(2);
    }

//    const char *c = hex + length - 1;
//    int idx;
//    for (idx = 0; idx < maxLength; idx++) {
//        data[idx] = 0;
//        if (c >= hex) {
//            data[idx] = *c - (isdigit(*c) ? '0' : 'A');
//            c--;
//            if (c >= hex) {
//                data[idx] = (data[idx] << 4) ^ (*c - (isdigit(*c) ? '0' : 'A'));
//                c--;
//            }
//        }
//    }

    int opSize = 0;
//    for (idx = 1; idx < 15; idx++) {
//        if (idx  * sizeof(qocell) <= length) {
//            opSize++;
//        } else {
//            break;
//        }
//    }

    return opSize;
}

void  qovm_load_icode(qovm *vm, const char *code) {
    printf(".code: %s\n", code);
    unsigned char function     = 0;
    int           indirectBit  = 0;
    int           gIndexModBit = 0;
    int           pIndexModBit = 0;
    const char   *mnemonic     = 0;
    int           numNibs      = 0;
    qocell        opSize       = 0;
    qocell        op;
    unsigned char  data[16 * sizeof(qocell)];

    while (*code && vm->endOfCode < vm->endOfCore) {
        if (isspace(*code)) {
            while (isspace(*code)) {
                code++;
            }
            continue;
        } else if (*code == ';') {
            // comment to the end of the line
            while (*code && !(*code == '\n')) {
                code++;
            }
            if (*code) {
                code++;
            }
        }

        const char *startOfHex = code;
        while (*code == '0' || *code == '1' || *code == '2' || *code == '3' || *code == '4' || *code == '5' || *code == '6' || *code == '7' || *code == '8' || *code == '9' || *code == 'A' || *code == 'B' || *code == 'C' || *code == 'D' || *code == 'E' || *code == 'F') {
            code++;
        }
        const char *endOfHex = code;

        switch (*code) {
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
            default: // ignore all unknown input
                break;
        }

        if (mnemonic) {
            // opSize is the number of cells of data needed.
            //
            opSize = qovm_hex_to_data_le(startOfHex, endOfHex - startOfHex, data);

            op  = QOP_WR_FUNC(function);
            op ^= QOP_WR_SIZE(opSize);
            op ^= QOP_WR_IBIT(indirectBit);
            op ^= QOP_WR_MODG(indirectBit);
            op ^= QOP_WR_MODP(indirectBit);

            printf(".info: icode(%-5s 0x%04x %2d)\n", mnemonic, op, opSize);
            if (opSize) {
                int idx;
                for (idx = 0; idx < opSize; idx++) {
                    printf(".....: .........data 0x%02x\n", data[idx]);
                }
            }
            qovm_emit(vm, op, data);

            // reset everything to prepare for the next instruction
            //
            function = 0;
            mnemonic = 0;
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
    printf(".info: sizeof(int      ) %3lu bytes\n", sizeof(int      ));
    printf(".info: sizeof(long     ) %3lu bytes\n", sizeof(long     ));
    printf(".info: sizeof(long long) %3lu bytes\n", sizeof(long long));
    printf(".info: sizeof(float    ) %3lu bytes\n", sizeof(float    ));
    printf(".info: sizeof(double   ) %3lu bytes\n", sizeof(double   ));
    printf(".info: sizeof(pointer  ) %3lu bytes\n", sizeof(void *   ));
    printf(".info: sizeof(qocellr  ) %3lu bytes\n", sizeof(qocell   ));

    union {
        unsigned char data[sizeof(unsigned long)];
        unsigned long number;
    } u;
    u.number = 0x0102030405060708L;
    int idx;
    printf(".info: unsigned long 0x%016lx ", u.number);
    for (idx = 0; idx < sizeof(unsigned long); idx++) {
        printf(" %02x", u.data[idx]);
    }
    printf("\n");
    qovm *vm = qovm_alloc(16 * 1024, 10, 8);
    qovm_dump(vm);

    qovm_load_icode(vm, "1234l 00px EEga FFpgs AAk DEADBEEFCAFEF00Dj CAFE t DDDDf FFl 1913l ; comments welcome");
    //qovm_dump(vm);

    //int idx;
    for (idx = 0; idx < 100; idx++) {
        qovm_exec(vm);
    }
    return 0;
}
