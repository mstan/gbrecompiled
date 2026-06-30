#include <stdio.h>
#include "gb_timing.h"
static int fails = 0;
#define CK(c) do{ if(!(c)){ printf("FAIL: %s\n", #c); fails++; } }while(0)
int main(void){
    /* main table */
    CK(GB_OP_TIMING[0x00].kind == GB_ACC_NONE);          /* NOP */
    CK(GB_OP_TIMING[0x34].kind == GB_ACC_RMW && GB_OP_TIMING[0x34].split_t == 4); /* INC (HL) */
    CK(GB_OP_TIMING[0x35].kind == GB_ACC_RMW);           /* DEC (HL) */
    CK(GB_OP_TIMING[0xA6].kind == GB_ACC_READ);          /* AND (HL) */
    CK(GB_OP_TIMING[0x77].kind == GB_ACC_WRITE);         /* LD (HL),A */
    CK(GB_OP_TIMING[0x36].kind == GB_ACC_WRITE);         /* LD (HL),n */
    CK(GB_OP_TIMING[0x7E].kind == GB_ACC_READ);          /* LD A,(HL) */
    CK(GB_OP_TIMING[0xEA].kind == GB_ACC_WRITE);         /* LD (nn),A */
    CK(GB_OP_TIMING[0xF0].kind == GB_ACC_READ);          /* LDH A,(n) */
    CK(GB_OP_TIMING[0x76].kind == GB_ACC_NONE);          /* HALT, not a store */
    CK(GB_OP_TIMING[0xC5].kind == GB_ACC_NONE);          /* PUSH BC (stack, excluded) */
    /* CB table */
    CK(GB_CB_TIMING[0x06].kind == GB_ACC_RMW);           /* RLC (HL) */
    CK(GB_CB_TIMING[0x46].kind == GB_ACC_READ);          /* BIT 0,(HL) read-only */
    CK(GB_CB_TIMING[0x7E].kind == GB_ACC_READ);          /* BIT 7,(HL) read-only */
    CK(GB_CB_TIMING[0x86].kind == GB_ACC_RMW);           /* RES 0,(HL) */
    CK(GB_CB_TIMING[0xFE].kind == GB_ACC_RMW);           /* SET 7,(HL) */
    CK(GB_CB_TIMING[0x40].kind == GB_ACC_NONE);          /* BIT 0,B (not (HL)) */
    /* counts: main RMW=2, CB RMW=24, CB read(HL)=8 */
    int op_rmw=0, cb_rmw=0, cb_rd=0;
    for(int i=0;i<256;i++){ if(GB_OP_TIMING[i].kind==GB_ACC_RMW) op_rmw++;
        if(GB_CB_TIMING[i].kind==GB_ACC_RMW) cb_rmw++;
        if(GB_CB_TIMING[i].kind==GB_ACC_READ) cb_rd++; }
    CK(op_rmw==2); CK(cb_rmw==24); CK(cb_rd==8);
    printf("counts: main_rmw=%d cb_rmw=%d cb_read=%d\n", op_rmw, cb_rmw, cb_rd);
    printf(fails? "SELFTEST: FAIL (%d)\n" : "SELFTEST: PASS\n", fails);
    return fails?1:0;
}
