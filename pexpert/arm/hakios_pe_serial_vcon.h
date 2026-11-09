/* included by pe_serial.c: the virtio console as a pe_serial_functions */
/*
 * A virtio console (port 0 of the first virtio-console device, over virtio-pci or
 * virtio-mmio): bsd/dev/hakios.virtio_console.c. For virtual machines without a UART.
 */
extern bool hakios_vcon_probe(bool other);
extern void hakios_vcon_init(void);
extern unsigned int hakios_vcon_transmit_ready(void);
extern void hakios_vcon_transmit_data(uint8_t c);
extern unsigned int hakios_vcon_receive_ready(void);
extern uint8_t hakios_vcon_receive_data(void);

SECURITY_READ_ONLY_LATE(static struct pe_serial_functions) virtio_console_serial_functions =
{
	.init = hakios_vcon_init,
	.transmit_ready = hakios_vcon_transmit_ready,
	.transmit_data = hakios_vcon_transmit_data,
	.receive_ready = hakios_vcon_receive_ready,
	.receive_data = hakios_vcon_receive_data,
	.device = SERIAL_VIRTIO_CONSOLE
};
