/* included by pe_serial.c: the 8250/16550 console of crosvm (AVF), as a pe_serial_functions */
#ifdef VMAPPLE_UART
/*
 * 8250/16550 UART (device tree: uart0 with compatible "ns16550a"), the console
 * of crosvm, the VMM of Android's virtualization framework: ttyS0 at 0x3f8,
 * which AVF captures as the VM's console. Byte-wide registers, one byte
 * apart; crosvm ignores accesses of any other width.
 */
static vm_offset_t ns16550_base_vaddr = 0;

#define NS16550_REG(off)        (*(volatile uint8_t *)(ns16550_base_vaddr + (off)))
#define NS16550_RBR_THR         0x0
#define NS16550_IER             0x1
#define NS16550_FCR             0x2
#define NS16550_LCR             0x3
#define NS16550_MCR             0x4
#define NS16550_LSR             0x5
#define NS16550_LSR_DR          0x01u
#define NS16550_LSR_THRE        0x20u

static unsigned int
ns16550_transmit_ready(void)
{
	return (unsigned int) !!(NS16550_REG(NS16550_LSR) & NS16550_LSR_THRE);
}

static void
ns16550_transmit_data(uint8_t c)
{
	NS16550_REG(NS16550_RBR_THR) = c;
}

static unsigned int
ns16550_receive_ready(void)
{
	return (unsigned int) !!(NS16550_REG(NS16550_LSR) & NS16550_LSR_DR);
}

static uint8_t
ns16550_receive_data(void)
{
	return NS16550_REG(NS16550_RBR_THR);
}

static void
ns16550_init(void)
{
	NS16550_REG(NS16550_IER) = 0x00;        /* polled */
	NS16550_REG(NS16550_LCR) = 0x03;        /* 8N1, divisor latch off: keep the firmware's baud rate */
	NS16550_REG(NS16550_FCR) = 0x07;        /* FIFOs on and cleared */
	NS16550_REG(NS16550_MCR) = 0x03;        /* DTR, RTS */
}

SECURITY_READ_ONLY_LATE(static struct pe_serial_functions) ns16550_serial_functions =
{
	.init = ns16550_init,
	.transmit_ready = ns16550_transmit_ready,
	.transmit_data = ns16550_transmit_data,
	.receive_ready = ns16550_receive_ready,
	.receive_data = ns16550_receive_data,
	.device = SERIAL_VMAPPLE_UART
};
#endif /* VMAPPLE_UART */
