# Functies T1.1 (automatisch uit CFG; namen/doel handmatig, zie RAPPORT.md)

| adres | naam | bytes | instr | aangeroepen door | roept aan | IO-registers (R/W) |
|---|---|---|---|---|---|---|
| 0x0002 | INT0-vector(stub) | 48 | 24 | vector | - | SREG(RW) |
| 0x0010 | T1OVF-vector | 240 | 120 | vector | - | SREG(RW), TCNT1H(W), TCNT1L(W) |
| 0x0016 | RXC-vector | 20 | 10 | vector | - | SREG(RW), UDR(R) |
| 0x002a | reset_main | 44 | 22 | reset | init_ram, init_timer1, init_ports, init_uart9600, init_comparator, init_wdt, wdr_kick, link_rx_frame, bouw_link_frame, ot_rx_thermostaat_ACO, ot_rx_ketel_PD5, ot_verwerk, ot_tx_ketel_PD3, ot_tx_thermostaat_PD4, link_timeout_noodbrug, led_knippercode, link_tx_naar_M11 | SPL(W), SPH(W) |
| 0x0056 | init_ram | 202 | 101 | reset_main | - |  |
| 0x0120 | init_timer1 | 12 | 6 | reset_main | - | TCCR1B(W), TIMSK(W) |
| 0x012c | init_ports | 26 | 13 | reset_main | - | DDRB(W), PORTB(W), DDRC(W), PORTC(W), DDRD(W), PORTD(W) |
| 0x0146 | init_uart9600 | 22 | 11 | reset_main | - | UBRRL(W), UBRRH/UCSRC(W), UCSRA(W), UCSRB(W), UDR(R) |
| 0x015c | init_comparator | 6 | 3 | reset_main | - | ACSR(W) |
| 0x0162 | init_wdt | 14 | 7 | reset_main | - | WDTCR(RW) |
| 0x0170 | wdr_kick | 4 | 2 | reset_main | - |  |
| 0x0174 | link_rx_frame | 274 | 137 | reset_main | - | PORTB(W) |
| 0x0232 | bouw_link_frame | 84 | 42 | reset_main, ot_tx_thermostaat_PD4 | - |  |
| 0x0286 | ot_rx_thermostaat_ACO | 274 | 137 | reset_main | - | ACSR(R), TCNT1H(W), TCNT1L(W), PINC(R) |
| 0x0398 | ot_rx_ketel_PD5 | 234 | 117 | reset_main | - | PIND(R), TCNT1H(W), TCNT1L(W) |
| 0x0482 | ot_verwerk | 890 | 445 | reset_main | ot_parity8, ot_parity_tail | PINC(R) |
| 0x0870 | ot_parity8 | 34 | 17 | ot_verwerk | - |  |
| 0x0872 | ot_parity_tail | 32 | 16 | ot_verwerk | - |  |
| 0x0892 | ot_tx_ketel_PD3 | 198 | 99 | reset_main | - | PINC(R), TCNT1H(W), TCNT1L(W), PORTD(W), PIND(R), PORTC(W) |
| 0x0958 | ot_tx_thermostaat_PD4 | 204 | 102 | reset_main | bouw_link_frame | TCNT1H(W), TCNT1L(W), PORTD(W), PIND(R) |
| 0x0a24 | link_timeout_noodbrug | 124 | 62 | reset_main | - | PINC(R), PORTB(W), PORTC(W), PINB(R) |
| 0x0aa0 | led_knippercode | 222 | 111 | reset_main | - | PORTC(W) |
| 0x0b7e | link_tx_naar_M11 | 34 | 17 | reset_main | uart_putc |  |
| 0x0ba0 | uart_putc | 10 | 5 | link_tx_naar_M11 | - | UDR(W), UCSRA(RW) |
