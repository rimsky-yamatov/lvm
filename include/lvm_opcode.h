#ifndef LVM_OPCODE_H
#define LVM_OPCODE_H

#include <stdint.h>

/*
 * 32-bit little-endian instructions:
 *
 *   | op:8 | A:8 | B:8 | C:8 |    ABC
 *   | op:8 | A:8 |    Bx:16  |    ABx / AsBx
 */
typedef enum {
    OP_MOV, OP_LDK, OP_LDI, OP_LDNIL, OP_LDBOOL,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_AND, OP_OR, OP_XOR, OP_NOT, OP_SHL, OP_SHR,
    OP_EQ, OP_LT, OP_LE,
    OP_JMP, OP_JT, OP_JF,
    OP_CALL, OP_RET,
    OP_HOST,
    OP_NEWARR, OP_NEWMAP, OP_GET, OP_SET, OP_LEN,
    OP_COUNT
} lvm_opcode;

#define LVM_OP(i)  ((uint32_t)(i) & 0xFFu)
#define LVM_A(i)   (((uint32_t)(i) >> 8) & 0xFFu)
#define LVM_B(i)   (((uint32_t)(i) >> 16) & 0xFFu)
#define LVM_C(i)   (((uint32_t)(i) >> 24) & 0xFFu)
#define LVM_BX(i)  ((uint32_t)(i) >> 16)
#define LVM_SBX(i) ((int)((LVM_BX(i) ^ 0x8000u)) - 0x8000)

#define LVM_ABC(op, a, b, c) \
    ((uint32_t)(op) | (uint32_t)(a) << 8 | (uint32_t)(b) << 16 | (uint32_t)(c) << 24)
#define LVM_ABX(op, a, bx) \
    ((uint32_t)(op) | (uint32_t)(a) << 8 | (uint32_t)(bx) << 16)
#define LVM_ASBX(op, a, sbx) \
    LVM_ABX(op, a, (uint32_t)(sbx) & 0xFFFFu)

#endif
