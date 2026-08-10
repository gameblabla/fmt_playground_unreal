/* Host stand-in for src/boot/io.h, used by the pacing test only.
 * Models the TOWNS 1us free-running timer at 0x26 and records DAC writes. */
#ifndef _ASM_IO_H
#define _ASM_IO_H
extern unsigned long host_now_us;      /* advanced by the test */
extern unsigned long host_dac_writes;
extern unsigned char host_dac_last;
extern unsigned long host_dac_times[];
extern unsigned long host_dac_count;
/* The interleave test drives a virtual clock from real host time; the pacing
 * test sets host_now_us directly and leaves this hook null. */
extern void host_clock_advance(void) __attribute__((weak));
/* The IC card reader pages the card into its window by writing a bank number
 * to 0x490; the card test implements this hook to move the corresponding
 * megabyte of its model card into the mapped window. */
extern void host_icm_bank_write(unsigned char bank) __attribute__((weak));
static inline unsigned short inw(unsigned short port) {
    (void)port;
    if (host_clock_advance) host_clock_advance();
    return (unsigned short)(host_now_us & 0xffff);
}
static inline unsigned char inb(unsigned short port) { (void)port; return 0; }
static inline void outb(unsigned char v, unsigned short port) {
    if (port == 0x490) {
        if (host_icm_bank_write) host_icm_bank_write(v);
        return;
    }
    if (port == 0x4DA) {
        host_dac_last = v;
        if (host_dac_count < 200000UL) host_dac_times[host_dac_count++] = host_now_us;
        host_dac_writes++;
    }
}
static inline void outw(unsigned short v, unsigned short port) { (void)v; (void)port; }
#endif
