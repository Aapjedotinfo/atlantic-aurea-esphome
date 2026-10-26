# Functies M1.1 (automatisch uit CFG; namen/doel handmatig, zie RAPPORT.md)

| adres | naam | bytes | instr | aangeroepen door | roept aan | IO-registers (R/W) |
|---|---|---|---|---|---|---|
| 0x0002 | INT0-vector(stub) | 52 | 26 | vector | - | SREG(RW) |
| 0x0010 | T1OVF-vector | 252 | 126 | vector | - | SREG(RW), TCNT1H(W), TCNT1L(W) |
| 0x0016 | RXC-vector | 20 | 10 | vector | - | SREG(RW), UDR(R) |
| 0x002a | reset_main | 40 | 20 | reset | init_ram, init_timer1, init_ports, init_uart9600, init_wdt, wdr_kick, link_rx_frame, sw_cv_only_PC0, btn_test_PC1, rx666_parser, stand_regelaar, bewaking_65_lockout, seconden_watchdog_66, tx666_machine, link_tx_to_T11 | SPL(W), SPH(W) |
| 0x0052 | init_ram | 198 | 99 | reset_main | - |  |
| 0x0118 | init_timer1 | 12 | 6 | reset_main | - | TCCR1B(W), TIMSK(W) |
| 0x0124 | init_ports | 30 | 15 | reset_main | - | SPCR(W), DDRB(W), PORTB(W), DDRC(W), PORTC(W), DDRD(W), PORTD(W) |
| 0x0142 | init_uart9600 | 22 | 11 | reset_main | - | UBRRL(W), UBRRH/UCSRC(W), UCSRA(W), UCSRB(W), UDR(R) |
| 0x0164 | init_wdt | 14 | 7 | reset_main | - | WDTCR(RW) |
| 0x0172 | wdr_kick | 4 | 2 | reset_main | - |  |
| 0x0176 | link_rx_frame | 348 | 174 | reset_main | - | PORTB(W), PINC(R) |
| 0x02ae | sw_cv_only_PC0 | 36 | 18 | reset_main | - | PINC(R), PORTB(W) |
| 0x02d2 | btn_test_PC1 | 54 | 27 | reset_main | - | PINC(R), PORTB(W) |
| 0x0308 | rx666_parser | 438 | 219 | reset_main | - | PINC(R), PORTB(W) |
| 0x04be | stand_regelaar | 368 | 184 | reset_main | - | PINB(R), PORTB(W) |
| 0x062e | bewaking_65_lockout | 256 | 128 | reset_main | - | PORTB(RW) |
| 0x072e | seconden_watchdog_66 | 268 | 134 | reset_main | - | PORTB(W) |
| 0x083a | tx666_machine | 518 | 259 | reset_main | - | PINC(R), PORTC(W), PORTB(W) |
| 0x0a40 | link_tx_to_T11 | 34 | 17 | reset_main | uart_putc |  |
| 0x0a62 | uart_putc | 10 | 5 | link_tx_to_T11 | - | UDR(W), UCSRA(RW) |
