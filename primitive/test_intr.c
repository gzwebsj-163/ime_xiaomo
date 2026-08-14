#include "primitive.h"
#include "interrupt.h"
#include <stdio.h>
#include <string.h>

int main() {
    PrimitiveComputer c;
    memset(&c, 0, sizeof(c));
    
    pic_init(&c.pic);
    pic_set_handler(&c.pic, IRQ_TIMER, 0x40, 0x03);
    pic_mask(&c.pic, IRQ_TIMER, false);
    
    pic_request(&c.pic, IRQ_TIMER);
    
    printf("PIC: irr=0x%02X imr=0x%02X isr=0x%02X\n", 
           c.pic.irr, c.pic.imr, c.pic.isr);
    printf("has_pending: %d\n", pic_has_pending(&c.pic));
    printf("get_highest: 0x%02X\n", pic_get_highest(&c.pic));
    printf("IVT[0]: addr=0x%02X flags=0x%02X\n", 
           c.pic.ivt[0].handler_addr, c.pic.ivt[0].flags);
    
    c.cpu.iflag = true;
    c.cpu.pc = 5;
    c.cpu.reg[0] = 42;
    
    bool handled = intr_handle(&c.pic, &c.cpu.pc, c.cpu.reg,
                               &c.cpu.zf, &c.cpu.cf,
                               &c.cpu.iflag, &c.intr_ctx);
    
    printf("intr_handle: %d\n", handled);
    printf("pc=0x%02X iflag=%d ctx.pc=0x%02X ctx.reg[0]=%d\n",
           c.cpu.pc, c.cpu.iflag, c.intr_ctx.pc, c.intr_ctx.reg[0]);
    
    return 0;
}
