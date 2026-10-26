# Aanroepgraaf M1.1

```
reset (0x002a) -> init_* ; hoofdlus 0x003c ->
reset_main -> init_ram
reset_main -> init_timer1
reset_main -> init_ports
reset_main -> init_uart9600
reset_main -> init_wdt
reset_main -> wdr_kick
reset_main -> link_rx_frame
reset_main -> sw_cv_only_PC0
reset_main -> btn_test_PC1
reset_main -> rx666_parser
reset_main -> stand_regelaar
reset_main -> bewaking_65_lockout
reset_main -> seconden_watchdog_66
reset_main -> tx666_machine
reset_main -> link_tx_to_T11
link_tx_to_T11 -> uart_putc
```

Tail-sprongen (RJMP naar functie-entry buiten eigen functie):

- link_rx_frame@017a rjmp -> sw_cv_only_PC0
- link_rx_frame@0180 rjmp -> sw_cv_only_PC0
- link_rx_frame@0196 rjmp -> sw_cv_only_PC0
- link_rx_frame@01de rjmp -> sw_cv_only_PC0

Hoofdlus-volgorde (aanroepen tussen 0x003c en rjmp terug):

wdr_kick -> rx666_parser -> link_rx_frame -> sw_cv_only_PC0 -> btn_test_PC1 -> stand_regelaar -> bewaking_65_lockout -> seconden_watchdog_66 -> tx666_machine -> link_tx_to_T11
