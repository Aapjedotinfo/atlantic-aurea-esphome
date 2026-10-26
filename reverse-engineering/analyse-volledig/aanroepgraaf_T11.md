# Aanroepgraaf T1.1

```
reset (0x002a) -> init_* ; hoofdlus 0x0040 ->
reset_main -> init_ram
reset_main -> init_timer1
reset_main -> init_ports
reset_main -> init_uart9600
reset_main -> init_comparator
reset_main -> init_wdt
reset_main -> wdr_kick
reset_main -> link_rx_frame
reset_main -> bouw_link_frame
reset_main -> ot_rx_thermostaat_ACO
reset_main -> ot_rx_ketel_PD5
reset_main -> ot_verwerk
reset_main -> ot_tx_ketel_PD3
reset_main -> ot_tx_thermostaat_PD4
reset_main -> link_timeout_noodbrug
reset_main -> led_knippercode
reset_main -> link_tx_naar_M11
ot_verwerk -> ot_parity8
ot_verwerk -> ot_parity_tail
ot_tx_thermostaat_PD4 -> bouw_link_frame
link_tx_naar_M11 -> uart_putc
```

Tail-sprongen (RJMP naar functie-entry buiten eigen functie):


Hoofdlus-volgorde (aanroepen tussen 0x0040 en rjmp terug):

wdr_kick -> link_rx_frame -> ot_rx_thermostaat_ACO -> ot_rx_ketel_PD5 -> ot_verwerk -> ot_tx_ketel_PD3 -> ot_tx_thermostaat_PD4 -> link_timeout_noodbrug -> led_knippercode -> link_tx_naar_M11
