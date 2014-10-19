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
    qocell *startOfCore;
    qocell *endOfCode;
    qocell *endOfCore;
    qocell *ip;
    int     debugLevel;
    int     maxSteps;
    size_t  coreSize;
    qocell  d; // address register; effective address;
    qocell  p; // index into stack frame (top of program stack)
    qocell  g; // index into global variable list
    qocell  a, b; // register: accumulators
    qocell  c;    // program counter
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
#define QOP_RD_DBIT(op) (((op) & 0x0800) >> 11)
#define QOP_RD_PBIT(op) (((op) & 0x0200) >> 10)
#define QOP_RD_GBIT(op) (((op) & 0x0400) >>  9)
#define QOP_RD_IBIT(op) (((op) & 0x0100) >>  8)
#define QOP_RD_ADDR(op) (((op) & 0x00ff)      )
//
#define QOP_WR_FUNC(op) (((op) & 0x0f  ) << 12)
#define QOP_WR_DBIT(op) (((op) & 0x01  ) << 11)
#define QOP_WR_PBIT(op) (((op) & 0x01  ) << 10)
#define QOP_WR_GBIT(op) (((op) & 0x01  ) <<  9)
#define QOP_WR_IBIT(op) (((op) & 0x01  ) <<  8)
#define QOP_WR_ADDR(op) (((op) & 0xff  )      )

#define QFNLOAD  0x00
#define QFNEXOP  0x01
#define QFNADD   0x02
#define QFNSTORE 0x03
#define QFNCALL  0x04
#define QFNJMP   0x05
#define QFNJMPT  0x06
#define QFNJMPF  0x07
#define QFNDUMP  0x0E
#define QFNHALT  0x0F

#define qoalloc(x) qo_alloc(__FILE__, __FUNCTION__, __LINE__, (x))
void *qo_alloc(const char *file, const char *function, int line, size_t size);
qovm *qovm_alloc(size_t coreSize, int debugLevel, int maxSteps);
void  qovm_dump(qovm *vm);
void  qovm_dump_opcode(qovm *vm, size_t address, qocell op);
void  qovm_emit_code(qovm *vm, qocell op);
void  qovm_emit_data(qovm *vm, qocell data);
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
    vm->startOfCore = vm->endOfCode = vm->ip = vm->core;
    vm->endOfCore = vm->core + coreSize;
    vm->coreSize = coreSize;
    vm->debugLevel = debugLevel;
    vm->maxSteps = -1;

    vm->a = vm->b = vm->c = vm->d = vm->g = vm->g = 0;

    if (vm->debugLevel) {
        int idx;
        for (idx = 0; idx <= coreSize; idx++) {
            vm->core[idx] = -1;
        }
        vm->maxSteps = maxSteps;
    }

    return vm;
}

void qovm_dump(qovm *vm) {
    printf("...vm: ------------------------------------\n");
    printf(".....: vm             %p\n", vm);
    printf(".....: coreSize       %zu\n", vm->coreSize);
//    printf(".....: endOfCode      %ld\n", vm->endOfCode - vm->core);
//    printf(".....: debugLevel     %d\n", vm->debugLevel);
//    printf(".....: startOfCore    %p\n", vm->startOfCore);
    printf(".....: programCounter %8d\n", vm->c);
//    printf(".....: endOfCore      %p\n", vm->endOfCore);
    printf(".....: c              %04x\n", vm->c);
    printf(".....: d              %04x\n", vm->d);
    printf(".....: a              %04x\n", vm->a);
    printf(".....: b              %04x\n", vm->b);
    printf(".....: p              %04x\n", vm->p);
    printf(".....: g              %04x\n", vm->g);
}

void qovm_dump_opcode(qovm *vm, size_t address, qocell op) {
    printf("...vm: ------------------------------------\n");
    printf(".....: address        %zu\n", address);
    printf(".....: op.function    0x%04x %s\n", QOP_RD_FUNC(op), qovm_util_op2mnemonic(QOP_RD_FUNC(op)));
    printf(".....: ...dpgi addr   %c%c%c%c %02x %4d\n", QOP_RD_DBIT(op) ? 'D':'d', QOP_RD_PBIT(op) ? 'P':'p', QOP_RD_GBIT(op) ? 'G':'g', QOP_RD_IBIT(op) ? 'I':'i', QOP_RD_ADDR(op), (int)(QOP_RD_ADDR(op)));
}

void qovm_emit_code(qovm *vm, qocell op) {
    // verify that we have space in the core
    //
    if (!(vm->core <= vm->endOfCode && vm->endOfCode < vm->endOfCore)) {
        printf("error: %s %d\n\tcode segment out of range\n", __FUNCTION__, __LINE__);
        qovm_dump(vm);
        exit(2);
    }

    printf(".info: emitc(pc %8ld => 0x%04x)\n", vm->endOfCode - vm->core, op);
    
    // emit the instruction
    //
    *(vm->endOfCode++) = op;
}

void qovm_emit_data(qovm *vm, qocell data) {
    // verify that we have space in the core
    //
    if (!(vm->core <= vm->endOfCode && vm->endOfCode < vm->endOfCore)) {
        printf("error: %s %d\n\tdata segment out of range\n", __FUNCTION__, __LINE__);
        qovm_dump(vm);
        exit(2);
    }

    printf(".info: emitd(pc %8ld => 0x%04x)\n", vm->endOfCode - vm->core, data);
    
    // emit the instruction
    //
    *(vm->endOfCode++) = data;
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
    if (!(0 <= vm->c && vm->c < vm->coreSize)) {
        printf("error: %s %d\n\tprogram counter out of range\n", __FUNCTION__, __LINE__);
        qovm_dump(vm);
        exit(2);
    }

    qovm_dump_opcode(vm, vm->c, vm->core[vm->c]);

    // fetch the instruction from core
    qocell code     = vm->core[vm->c++];
    qocell function = QOP_RD_FUNC(code);
    qocell dBit     = QOP_RD_DBIT(code);
    qocell pBit     = QOP_RD_PBIT(code);
    qocell gBit     = QOP_RD_GBIT(code);
    qocell iBit     = QOP_RD_IBIT(code);
    qocell addrBits = QOP_RD_ADDR(code);

    // if the D bit is set, the address is the value of the next cell.
    // other wise, it is just the program counter plus the address offset.
    //
    if (dBit) {
        vm->d = vm->core[vm->c++];
    } else {
        vm->d = vm->c + addrBits;
    }

    // second step of address calculation
    // if the P bit is set, the P register is added to D
    //
    if (pBit) {
        vm->d += vm->p;
    }

    // third stage of address calculation
    // if the G bit is set, the G register is added to D
    //
    if (gBit) {
        vm->d += vm->g;
    }
    
    // last stage of address calculation
    // if the I bit is set, the D register is an indirect reference
    //
    if (iBit) {
        if (vm->d > vm->coreSize) {
            printf("error: %s %d\n\tindirect address out of range\n", __FUNCTION__, __LINE__);
            qovm_dump(vm);
            exit(2);
        }
        vm->d = vm->core[vm->d];
    }

    switch (function) {
        case QFNADD:
            vm->a += vm->d;
            break;
        case QFNCALL:
            vm->d += vm->p;
            vm->core[vm->d] = vm->p;
            vm->core[vm->d + 1] = vm->c;
            vm->p = vm->d;
            vm->c = vm->a;
            break;
        case QFNDUMP:
            qovm_dump(vm);
            break;
        case QFNEXOP:
            printf(".warn: exop not implemented\n");
            break;
        case QFNHALT:
            printf(".halt: halting....\n");
            break;
        case QFNJMP:
            vm->c = vm->d;
            break;
        case QFNJMPF:
            if (!vm->a) {
                vm->c = vm->d;
            }
            break;
        case QFNJMPT:
            if (vm->a) {
                vm->c = vm->d;
            }
            break;
        case QFNLOAD:
            vm->b = vm->a;
            vm->a = vm->d;
            break;
        case QFNSTORE:
            if (vm->d > vm->coreSize) {
                printf("error: %s %d\n\taddress out of range\n", __FUNCTION__, __LINE__);
                qovm_dump(vm);
                exit(2);
            }
            vm->core[vm->d] = vm->a;
            break;
        default:
            printf("error:\t%s %d\nerror: unknown function 0x%02x\n", __FUNCTION__, __LINE__, function);
            exit(2);
    }
}

void  qovm_load_icode(qovm *vm, const char *code) {
    printf(".code: %s\n", code);
    unsigned char function     = 0;
    unsigned char dBit         = 0;
    unsigned char pBit         = 0;
    unsigned char gBit         = 0;
    unsigned char iBit         = 0;
    unsigned char oBits        = 0;
    const char   *mnemonic     = 0;
    qocell        op;

    while (*code && vm->endOfCode < vm->endOfCore) {
        if (isspace(*code)) {
            while (isspace(*code)) {
                code++;
            }
            continue;
        }
        
        if (*code == ';') {
            // comment to the end of the line
            while (*code && !(*code == '\n')) {
                code++;
            }
            if (*code) {
                code++;
            }
        }

        if (isdigit(*code) || ('A' <= *code && *code <= 'F')) {
            unsigned long number = 0;
            do {
                if (isdigit(*code)) {
                    number = (number << 4) + (*(code++) - '0');
                } else if ('A' <= *code && *code <= 'F') {
                    number = (number << 4) + (*(code++) - 'A' + 10);
                } else {
                    break;
                }
            } while (*code);

            op  = (qocell)number;
            qovm_emit_data(vm, op);

            // reset everything to prepare for the next instruction
            //
            function = 0;
            mnemonic = 0;
            dBit = pBit = gBit = iBit = oBits = 0;

            continue;
        }

        switch (*(code++)) {
            case 'a': function = QFNADD  ; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'k': function = QFNCALL ; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'f': function = QFNJMPF ; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'h': function = QFNHALT ; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'j': function = QFNJMP  ; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'l': function = QFNLOAD ; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'q': function = QFNDUMP ; mnemonic = qovm_util_op2mnemonic(function); break;
            case 's': function = QFNSTORE; mnemonic = qovm_util_op2mnemonic(function); break;
            case 't': function = QFNJMPT ; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'x': function = QFNEXOP ; mnemonic = qovm_util_op2mnemonic(function); break;
            case 'd': dBit = 0x01; break;
            case 'i': iBit = 0x01; break;
            case 'g': gBit = 0x01; break;
            case 'p': pBit = 0x01; break;
                break;
            default: // ignore all unknown input
                break;
        }

        if (mnemonic) {
            op  = QOP_WR_FUNC(function);
            op ^= QOP_WR_DBIT(dBit);
            op ^= QOP_WR_PBIT(pBit);
            op ^= QOP_WR_GBIT(gBit);
            op ^= QOP_WR_IBIT(iBit);
            qovm_emit_code(vm, op);

            // reset everything to prepare for the next instruction
            //
            function = 0;
            mnemonic = 0;
            dBit = pBit = gBit = iBit = oBits = 0;
        }
    }
    if (*code) {
        printf("error:\t%s %d\nerror: out of core memory\n", __FUNCTION__, __LINE__);
        exit(2);
    }
}

void qovm_reset(qovm *vm) {
    vm->a = vm->b = vm->c = vm->d = vm->g = vm->g = 0;
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
        case QFNADD: return "add";
        case QFNCALL: return "call";
        case QFNDUMP: return "dump";
        case QFNEXOP: return "exop";
        case QFNHALT: return "halt";
        case QFNJMP: return "jmp";
        case QFNJMPF: return "jmpf";
        case QFNJMPT: return "jmpt";
        case QFNLOAD: return "load";
        case QFNSTORE: return "store";
    }
    printf("error:\t%s %d\nerror: unknown function 0x%02x\n", __FUNCTION__, __LINE__, op);
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

    qovm *vm = qovm_alloc(64 * 1024, 10, 8);
    qovm_dump(vm);

    qovm_load_icode(vm, "q da 1234 q h gpil px ga pgs k j t f l l 1234 DEAD BEEF ; comments welcome");
    //qovm_dump(vm);

    int idx;
    for (idx = 0; idx < 100; idx++) {
        qovm_exec(vm);
    }
    return 0;
}
