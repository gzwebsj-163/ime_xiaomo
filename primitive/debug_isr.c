#include "primitive.h"
#include "interrupt.h"
#include <stdio.h>
#include <string.h>

int main() {
    PrimitiveComputer c;
    memset(&c, 0, sizeof(c));
    
    c.memory[0x51] = 0x50;
    c.memory[0x50] = 0;
    
    /* ISR code */
    c.memory[0x40 * 2]     = (OP_LD << 4) | 0xB;
    c.memory[0x40 * 2 + 1] = (0x5 << 4) | 0x1;
    c.memory[0x41 * 2]     = (OP_LD << 4) | 0xA;
    c.memory[0x41 * 2 + 1] = (0x5 << 4) | 0x0;
    c.memory[0x42 * 2]     = (OP_ADD << 4) | 0xA;
    c.memory[0x42 * 2 + 1] = (0xA << 4) | 0x5;
    c.memory[0x43 * 2]     = (OP_ST << 4) | 0xA;
    c.memory[0x43 * 2 + 1] = (0xB << 4) | 0x0;
    c.memory[0x44 * 2]     = (OP_IRET << 4) | 0x0;
    c.memory[0x44 * 2 + 1] = 0x00;
    
    c.cpu.reg[5] = 1;
    c.cpu.pc = 0x40;
    c.cpu.running = true;
    
    for (int i = 0; i < 5; i++) {
        uint8_t* ip = &c.memory[c.cpu.pc * 2];
        uint8_t op = (ip[0] >> 4) & 0x0F;
        printf("PC=0x%02X op=%d R10=%d R11=%d MEM[0x50]=%d\n",
               c.cpu.pc, op, c.cpu.reg[0xA], c.cpu.reg[0xB], c.memory[0x50]);
        computer_step(&c);
    }
    
    printf("Final: R10=%d R11=%d MEM[0x50]=%d\n",
           c.cpu.reg[0xA], c.cpu.reg[0xB], c.memory[0x50]);
    return 0;
}
