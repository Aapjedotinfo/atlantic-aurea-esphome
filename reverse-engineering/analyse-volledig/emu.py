#!/usr/bin/env python3
"""emu.py - cyclus-nauwkeurige ATmega8-emulator (eigen werk, alleen de voor deze firmware nodige
randapparatuur: poorten, Timer1 (normaal, deler 1/8/64/256/1024), USART (8N1/8N2, U2X),
analoge comparator-ingang, SP/SREG, WDT-teller). Instructieset volledig (avr4, geen JMP/CALL).

Doel: de originele M1.1- en T1.1-firmware echt laten lopen en de uitgaande bitstroom,
poortschakelingen en UART-bytes loggen.  Zie test_emu_vs_gdb.py voor de kruiscontrole van de
kern tegen de AVR-simulator in avr-gdb (onafhankelijke implementatie).
"""
import heapq, os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import avrlib as A

RAMEND = 0x45F
# IO-adressen (0x00-0x3F, dataadres = +0x20)
SREG_, SPH_, SPL_ = 0x3F, 0x3E, 0x3D
GICR, GIFR, TIMSK, TIFR = 0x3B, 0x3A, 0x39, 0x38
TCCR1A, TCCR1B, TCNT1H, TCNT1L = 0x2F, 0x2E, 0x2D, 0x2C
PORTB, DDRB, PINB, PORTC, DDRC, PINC, PORTD, DDRD, PIND = 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10
UDR, UCSRA, UCSRB, UBRRL, UCSRC = 0x0C, 0x0B, 0x0A, 0x09, 0x20
ACSR, WDTCR = 0x08, 0x21
EECR = 0x1C

VEC_T1OVF, VEC_RXC, VEC_INT0 = 8, 11, 1


class CPU:
    def __init__(self, flash_bytes, name="cpu"):
        self.name = name
        self.words = [flash_bytes[i] | (flash_bytes[i + 1] << 8) for i in range(0, 8192, 2)]
        self.r = [0] * 32
        self.mem = bytearray(RAMEND + 1)      # dataruimte 0x00-0x45F (0x20-0x5F: niet gebruikt, IO apart)
        self.io = [0] * 64
        self.sreg = 0
        self.sp = RAMEND
        self.pc = 0
        self.cyc = 0
        self.int_block = False
        self.halted = False
        self.dec = [None] * 4096
        self._decode_all()
        # perifere toestand
        self.ext_pin = {PINB: None, PINC: None, PIND: None}   # (drivenmask, value)
        self.ext_drv = {PINB: 0, PINC: 0, PIND: 0}
        self.ext_val = {PINB: 0, PINC: 0, PIND: 0}
        self.aco_fn = lambda cyc: 0
        self.tcnt_base = 0
        self.tcnt_t0 = 0
        self.tc_div = 0
        self.tcnt_temp = 0
        self.tifr = 0
        self.ov_time = None
        self.uart_tx_end = None
        self.uart_tx_byte = None
        self.rx_queue = []            # heap (cycle, seq, byte)
        self.rx_seq = 0
        self.rxc = False
        self.udr_rx = 0
        self.txc = False
        self.events = []              # extra callbacks (cycle, seq, fn)
        self.irq = False
        self.next_ev = 1 << 62
        self.portlog = []             # (cyc, ioaddr, nieuwe PORT/DDR-waarde)
        self.uart_tx_log = []         # (cyc_start, byte)
        self.lpm_log = {}             # Z-adres -> aantal
        self.exec_pcs = set()
        self.trace_pcs = False
        self.wdt_last = 0
        self.wdt_resets = 0
        self.isr_log = []
        self.log_isr = False
        self.stop_at_pc = None
        self.io_read_log = {}
        self.io_write_log = {}
        self.track_io = False
        self.raw_io = False

    # ------------------------------------------------------------ decode
    def _decode_all(self):
        w = self.words
        for pc in range(4096):
            ins = A.decode(w[pc], w[pc + 1] if pc + 1 < 4096 else 0, pc)
            self.dec[pc] = self._compile(ins, pc) if ins else self._compile_bad(w[pc], pc)

    def _compile_bad(self, wd, pc):
        def f(c):
            c.pc = (pc + 1) & 0xFFF
            c.cyc += 1
        return f

    # ------------------------------------------------------------ data space
    def rd(self, addr):
        if addr < 0x20:
            return self.r[addr]
        if addr < 0x60:
            return self.io_read(addr - 0x20)
        return self.mem[addr] if addr <= RAMEND else 0

    def wr(self, addr, v):
        if addr < 0x20:
            self.r[addr] = v
        elif addr < 0x60:
            self.io_write(addr - 0x20, v)
        elif addr <= RAMEND:
            self.mem[addr] = v

    def push(self, v):
        self.mem[self.sp] = v
        self.sp = (self.sp - 1) & 0xFFFF

    def pop(self):
        self.sp = (self.sp + 1) & 0xFFFF
        return self.mem[self.sp] if self.sp <= RAMEND else 0

    # ------------------------------------------------------------ timer-hulpfuncties
    def tcnt_now(self):
        if not self.tc_div:
            return self.tcnt_base
        return (self.tcnt_base + (self.cyc - self.tcnt_t0) // self.tc_div) & 0xFFFF

    def _set_tcnt(self, val, t):
        self.tcnt_base = val & 0xFFFF
        self.tcnt_t0 = t
        self._plan_ov()

    def _plan_ov(self):
        if self.tc_div:
            self.ov_time = self.tcnt_t0 + (0x10000 - self.tcnt_base) * self.tc_div
        else:
            self.ov_time = None
        self._recalc()

    # ------------------------------------------------------------ IO
    def pin_value(self, pinaddr):
        port, ddr = pinaddr + 2, pinaddr + 1
        p, d = self.io[port], self.io[ddr]
        drv, val = self.ext_drv[pinaddr], self.ext_val[pinaddr]
        # uitgangen: eigen PORT-waarde; ingangen: extern gedreven, anders pull-up (PORT) of zwevend=0
        v = (p & d) | (val & drv & ~d) | (p & ~d & ~drv)
        return v & 0xFF

    def io_read(self, a):
        if self.raw_io and a not in (SREG_, SPL_, SPH_):
            return self.io[a]
        if self.track_io:
            self.io_read_log[a] = self.io_read_log.get(a, 0) + 1
        if a == SREG_:
            return self.sreg
        if a == SPL_:
            return self.sp & 0xFF
        if a == SPH_:
            return self.sp >> 8
        if a in (PINB, PINC, PIND):
            return self.pin_value(a)
        if a == TCNT1L:
            t = self.tcnt_now()
            self.tcnt_temp = t >> 8
            return t & 0xFF
        if a == TCNT1H:
            return self.tcnt_temp
        if a == TIFR:
            return self.tifr
        if a == UDR:
            v = self.udr_rx
            self.rxc = False
            self._recalc()
            return v
        if a == UCSRA:
            return (0x80 if self.rxc else 0) | (0x40 if self.txc else 0) | 0x20 | (self.io[UCSRA] & 0x03)
        if a == ACSR:
            return (self.io[ACSR] & ~0x20) | (0x20 if self.aco_fn(self.cyc) else 0)
        return self.io[a]

    def io_write(self, a, v):
        if self.raw_io and a not in (SREG_, SPL_, SPH_):
            self.io[a] = v
            return
        if self.track_io:
            self.io_write_log[a] = self.io_write_log.get(a, 0) + 1
        if a == SREG_:
            self.sreg = v
            return
        if a == SPL_:
            self.sp = (self.sp & 0xFF00) | v
            return
        if a == SPH_:
            self.sp = (self.sp & 0x00FF) | (v << 8)
            return
        if a in (PORTB, PORTC, PORTD, DDRB, DDRC, DDRD):
            old = self.io[a]
            self.io[a] = v
            if old != v:
                self.portlog.append((self.cyc, a, v))
            return
        if a in (PINB, PINC, PIND):       # schrijven toggelt PORT (mega8: toggle niet; negeren)
            return
        if a == TCNT1H:
            self.tcnt_temp = v
            return
        if a == TCNT1L:
            self._set_tcnt((self.tcnt_temp << 8) | v, self.cyc + 1)
            return
        if a == TCCR1B:
            self.io[a] = v
            div = {0: 0, 1: 1, 2: 8, 3: 64, 4: 256, 5: 1024}.get(v & 7, 0)
            if div != self.tc_div:
                cur = self.tcnt_now()
                self.tc_div = div
                self._set_tcnt(cur, self.cyc + 1)
            return
        if a == TIMSK:
            self.io[a] = v
            self._recalc()
            return
        if a == TIFR:
            self.tifr &= ~v
            self._recalc()
            return
        if a == UDR:
            self._uart_tx(v)
            return
        if a == UCSRA:
            if v & 0x40:
                self.txc = False
            self.io[a] = (self.io[a] & ~0x03) | (v & 0x03)       # U2X (bit1), MPCM(bit0)
            return
        if a == UCSRB:
            self.io[a] = v
            self._recalc()
            return
        if a == WDTCR:
            self.io[a] = v
            return
        self.io[a] = v

    # ------------------------------------------------------------ UART
    def bit_cycles(self):
        ubrr = self.io[UBRRL] | ((self.io[UCSRC] & 0x0F) << 8 if False else 0)
        u2x = (self.io[UCSRA] >> 1) & 1
        return (8 if u2x else 16) * (ubrr + 1)

    def frame_bits(self):
        c = self.io[UCSRC]
        stop = 2 if (c & 0x08) else 1
        # UCSZ=3 -> 8 bit
        return 1 + 8 + stop

    def _uart_tx(self, v):
        if not (self.io[UCSRB] & 0x08):
            return
        start = self.cyc + 1
        if self.uart_tx_end is not None and start < self.uart_tx_end:
            start = self.uart_tx_end       # (schuifregister bezet)
        self.uart_tx_log.append((start, v))
        self.uart_tx_end = start + self.frame_bits() * self.bit_cycles()
        self.txc = False
        self._recalc()

    def inject_rx(self, cycle, byte):
        self.rx_seq += 1
        heapq.heappush(self.rx_queue, (cycle, self.rx_seq, byte))
        self._recalc()

    def schedule(self, cycle, fn):
        self.rx_seq += 1
        heapq.heappush(self.events, (cycle, self.rx_seq, fn))
        self._recalc()

    # ------------------------------------------------------------ events / irq
    def _recalc(self):
        t = []
        if self.ov_time is not None:
            t.append(self.ov_time)
        if self.uart_tx_end is not None:
            t.append(self.uart_tx_end)
        if self.rx_queue:
            t.append(self.rx_queue[0][0])
        if self.events:
            t.append(self.events[0][0])
        self.next_ev = min(t) if t else (1 << 62)
        self.irq = bool(((self.tifr & 4) and (self.io[TIMSK] & 4)) or (self.rxc and (self.io[UCSRB] & 0x80)))

    def _handle_events(self):
        c = self.cyc
        if self.io[WDTCR] & 0x08 and c - self.wdt_last > 2_100_000 * 8 // 1:   # WDE aan, WDP=7: ~2,1 s bij 8 MHz-nominaal 16 ms*128
            self.wdt_resets += 1
            self.wdt_last = c
        while self.ov_time is not None and c >= self.ov_time:
            self.tifr |= 4
            self.ov_time += 0x10000 * self.tc_div
        if self.uart_tx_end is not None and c >= self.uart_tx_end:
            self.txc = True
            self.uart_tx_end = None
        while self.rx_queue and self.rx_queue[0][0] <= c:
            _, _, b = heapq.heappop(self.rx_queue)
            self.udr_rx = b
            self.rxc = True
        while self.events and self.events[0][0] <= c:
            _, _, fn = heapq.heappop(self.events)
            fn(self)
        self._recalc()

    def _service_irq(self):
        vec = None
        if (self.tifr & 4) and (self.io[TIMSK] & 4):
            vec = VEC_T1OVF
            self.tifr &= ~4
        elif self.rxc and (self.io[UCSRB] & 0x80):
            vec = VEC_RXC
        if vec is None:
            return
        ret = self.pc
        self.push(ret & 0xFF)
        self.push(ret >> 8)
        self.sreg &= 0x7F
        if self.log_isr:
            self.isr_log.append((self.cyc, vec))
        self.pc = vec
        self.cyc += 4
        self._recalc()

    def step_raw(self):
        self.dec[self.pc](self)

    # ------------------------------------------------------------ run
    def run(self, until):
        dec = self.dec
        while self.cyc < until:
            if self.cyc >= self.next_ev:
                self._handle_events()
            if self.irq and (self.sreg & 0x80) and not self.int_block:
                self._service_irq()
                continue
            self.int_block = False
            if self.trace_pcs:
                self.exec_pcs.add(self.pc)
            if self.stop_at_pc is not None and self.pc == self.stop_at_pc:
                return "stop"
            dec[self.pc](self)
        return "ok"

    # ------------------------------------------------------------ instructies
    def _compile(self, ins, pc):
        n, o = ins["name"], ins["ops"]
        npc = (pc + ins["size"]) & 0xFFF
        w = self.words
        nxt_size = 1
        if pc + ins["size"] < 4096:
            ni = A.decode(w[pc + ins["size"]], w[pc + ins["size"] + 1] if pc + ins["size"] + 1 < 4096 else 0)
            nxt_size = ni["size"] if ni else 1
        d = o.get("d")
        rr = o.get("r")
        K = o.get("K")

        def setf(c, mask, val):
            pass

        # ---- aritmetiek
        if n in ("add", "adc"):
            carry = (n == "adc")

            def f(c):
                r = c.r
                a, b = r[d], r[rr]
                cin = (c.sreg & 1) if carry else 0
                s = a + b + cin
                res = s & 0xFF
                h = ((a & 0xF) + (b & 0xF) + cin) > 0xF
                v = ((a ^ res) & (b ^ res) & 0x80) != 0
                nf = res >> 7
                sr = c.sreg & 0xC0
                sr |= (1 if s > 0xFF else 0) | (2 if res == 0 else 0) | (nf << 2) | (8 if v else 0) | \
                      (((nf ^ (1 if v else 0)) & 1) << 4) | (0x20 if h else 0)
                c.sreg = sr
                r[d] = res
                c.pc = npc
                c.cyc += 1
            return f
        if n in ("sub", "sbc", "cp", "cpc", "subi", "sbci", "cpi"):
            use_c = n in ("sbc", "cpc", "sbci")
            wb = n in ("sub", "sbc", "subi", "sbci")
            imm = n in ("subi", "sbci", "cpi")
            keepz = n in ("sbc", "cpc", "sbci")

            def f(c):
                r = c.r
                a = r[d]
                b = K if imm else r[rr]
                cin = (c.sreg & 1) if use_c else 0
                s = a - b - cin
                res = s & 0xFF
                h = ((a & 0xF) - (b & 0xF) - cin) < 0
                v = ((a ^ b) & (a ^ res) & 0x80) != 0
                nf = res >> 7
                z = (res == 0) and ((c.sreg & 2) != 0 if keepz else True)
                sr = c.sreg & 0xC0
                sr |= (1 if s < 0 else 0) | (2 if z else 0) | (nf << 2) | (8 if v else 0) | \
                      (((nf ^ (1 if v else 0)) & 1) << 4) | (0x20 if h else 0)
                c.sreg = sr
                if wb:
                    r[d] = res
                c.pc = npc
                c.cyc += 1
            return f
        if n in ("and", "andi", "or", "ori", "eor"):
            op = {"and": 0, "andi": 0, "or": 1, "ori": 1, "eor": 2}[n]
            imm = n in ("andi", "ori")

            def f(c):
                r = c.r
                b = K if imm else r[rr]
                res = (r[d] & b) if op == 0 else ((r[d] | b) if op == 1 else (r[d] ^ b))
                nf = res >> 7
                c.sreg = (c.sreg & 0xE1) | (2 if res == 0 else 0) | (nf << 2) | (nf << 4)
                r[d] = res
                c.pc = npc
                c.cyc += 1
            return f
        if n == "com":
            def f(c):
                r = c.r
                res = (~r[d]) & 0xFF
                nf = res >> 7
                c.sreg = (c.sreg & 0xE0) | 1 | (2 if res == 0 else 0) | (nf << 2) | (nf << 4)
                r[d] = res
                c.pc = npc
                c.cyc += 1
            return f
        if n == "neg":
            def f(c):
                r = c.r
                a = r[d]
                res = (-a) & 0xFF
                nf = res >> 7
                v = (res == 0x80)
                h = ((res | a) & 0x08) != 0
                c.sreg = (c.sreg & 0xC0) | (1 if res != 0 else 0) | (2 if res == 0 else 0) | (nf << 2) | \
                         (8 if v else 0) | (((nf ^ (1 if v else 0)) & 1) << 4) | (0x20 if h else 0)
                r[d] = res
                c.pc = npc
                c.cyc += 1
            return f
        if n in ("inc", "dec"):
            inc = n == "inc"

            def f(c):
                r = c.r
                a = r[d]
                res = (a + 1) & 0xFF if inc else (a - 1) & 0xFF
                v = (a == 0x7F) if inc else (a == 0x80)
                nf = res >> 7
                c.sreg = (c.sreg & 0xE1) | (2 if res == 0 else 0) | (nf << 2) | (8 if v else 0) | \
                         (((nf ^ (1 if v else 0)) & 1) << 4)
                r[d] = res
                c.pc = npc
                c.cyc += 1
            return f
        if n in ("lsr", "asr", "ror"):
            def f(c):
                r = c.r
                a = r[d]
                cin = c.sreg & 1
                if n == "lsr":
                    res = a >> 1
                elif n == "asr":
                    res = (a >> 1) | (a & 0x80)
                else:
                    res = (a >> 1) | (cin << 7)
                cf = a & 1
                nf = res >> 7
                v = nf ^ cf
                c.sreg = (c.sreg & 0xC0) | cf | (2 if res == 0 else 0) | (nf << 2) | (v << 3) | ((nf ^ v) << 4) | (c.sreg & 0x20)
                r[d] = res
                c.pc = npc
                c.cyc += 1
            return f
        if n == "swap":
            def f(c):
                a = c.r[d]
                c.r[d] = ((a << 4) | (a >> 4)) & 0xFF
                c.pc = npc
                c.cyc += 1
            return f
        if n in ("adiw", "sbiw"):
            add = n == "adiw"

            def f(c):
                r = c.r
                a = r[d] | (r[d + 1] << 8)
                res = (a + K) & 0xFFFF if add else (a - K) & 0xFFFF
                ah, rh = a >> 15, res >> 15
                if add:
                    v = (rh and not ah)
                    cf = (ah and not rh)
                else:
                    v = (ah and not rh)
                    cf = (rh and not ah)
                nf = rh
                c.sreg = (c.sreg & 0xE0) | (1 if cf else 0) | (2 if res == 0 else 0) | (nf << 2) | \
                         (8 if v else 0) | (((nf ^ (1 if v else 0)) & 1) << 4)
                r[d] = res & 0xFF
                r[d + 1] = res >> 8
                c.pc = npc
                c.cyc += 2
            return f
        if n in ("mul", "muls", "mulsu"):
            def f(c):
                r = c.r
                a, b = r[d], r[rr]
                if n != "mul":
                    a = a - 256 if a & 0x80 else a
                if n == "muls":
                    b = b - 256 if b & 0x80 else b
                res = (a * b) & 0xFFFF
                r[0], r[1] = res & 0xFF, res >> 8
                c.sreg = (c.sreg & 0xFC) | (res >> 15) | (2 if res == 0 else 0)
                c.pc = npc
                c.cyc += 2
            return f
        if n in ("fmul", "fmuls", "fmulsu"):
            def f(c):
                r = c.r
                a, b = r[d], r[rr]
                if n != "fmul":
                    a = a - 256 if a & 0x80 else a
                if n == "fmuls":
                    b = b - 256 if b & 0x80 else b
                p = (a * b) & 0xFFFF
                res = (p << 1) & 0xFFFF
                r[0], r[1] = res & 0xFF, res >> 8
                c.sreg = (c.sreg & 0xFC) | (p >> 15) | (2 if res == 0 else 0)
                c.pc = npc
                c.cyc += 2
            return f
        # ---- data
        if n == "mov":
            def f(c):
                c.r[d] = c.r[rr]
                c.pc = npc
                c.cyc += 1
            return f
        if n == "movw":
            def f(c):
                c.r[d], c.r[d + 1] = c.r[rr], c.r[rr + 1]
                c.pc = npc
                c.cyc += 1
            return f
        if n == "ldi":
            def f(c):
                c.r[d] = K
                c.pc = npc
                c.cyc += 1
            return f
        if n == "in":
            A_ = o["A"]

            def f(c):
                c.r[d] = c.io_read(A_)
                c.pc = npc
                c.cyc += 1
            return f
        if n == "out":
            A_ = o["A"]

            def f(c):
                c.io_write(A_, c.r[d])
                c.pc = npc
                c.cyc += 1
            return f
        if n in ("sbi", "cbi"):
            A_, b = o["A"], o["b"]
            setb = n == "sbi"

            def f(c):
                if A_ in (PINB, PINC, PIND):
                    pass
                v = c.io[A_] if A_ not in (SREG_, SPL_, SPH_) else c.io_read(A_)
                v = (v | (1 << b)) if setb else (v & ~(1 << b))
                c.io_write(A_, v & 0xFF)
                c.pc = npc
                c.cyc += 2
            return f
        if n in ("sbic", "sbis"):
            A_, b = o["A"], o["b"]
            want = 1 if n == "sbis" else 0

            def f(c):
                bit = (c.io_read(A_) >> b) & 1
                if bit == want:
                    c.pc = (npc + nxt_size) & 0xFFF
                    c.cyc += 1 + nxt_size
                else:
                    c.pc = npc
                    c.cyc += 1
            return f
        if n in ("sbrc", "sbrs"):
            b = o["b"]
            want = 1 if n == "sbrs" else 0

            def f(c):
                if ((c.r[d] >> b) & 1) == want:
                    c.pc = (npc + nxt_size) & 0xFFF
                    c.cyc += 1 + nxt_size
                else:
                    c.pc = npc
                    c.cyc += 1
            return f
        if n == "cpse":
            def f(c):
                if c.r[d] == c.r[rr]:
                    c.pc = (npc + nxt_size) & 0xFFF
                    c.cyc += 1 + nxt_size
                else:
                    c.pc = npc
                    c.cyc += 1
            return f
        if n in ("bst", "bld"):
            b = o["b"]
            if n == "bst":
                def f(c):
                    c.sreg = (c.sreg & ~0x40) | (((c.r[d] >> b) & 1) << 6)
                    c.pc = npc
                    c.cyc += 1
            else:
                def f(c):
                    if c.sreg & 0x40:
                        c.r[d] |= (1 << b)
                    else:
                        c.r[d] &= ~(1 << b) & 0xFF
                    c.pc = npc
                    c.cyc += 1
            return f
        if n in ("bset", "bclr"):
            s = o["s"]
            setb = n == "bset"

            def f(c):
                if setb:
                    c.sreg |= (1 << s)
                    if s == 7:
                        c.int_block = True
                else:
                    c.sreg &= ~(1 << s) & 0xFF
                c.pc = npc
                c.cyc += 1
            return f
        # ---- geheugen
        if n in ("ldd_y", "ldd_z", "std_y", "std_z"):
            q = o["q"]
            ptr = 28 if n.endswith("y") else 30
            ld = n.startswith("ld")

            def f(c):
                r = c.r
                addr = ((r[ptr + 1] << 8) | r[ptr]) + q
                if ld:
                    r[d] = c.rd(addr & 0xFFFF)
                else:
                    c.wr(addr & 0xFFFF, r[d])
                c.pc = npc
                c.cyc += 2
            return f
        if n in ("ld_x", "ld_xp", "ld_mx", "st_x", "st_xp", "st_mx", "ld_yp", "ld_my", "st_yp", "st_my",
                 "ld_zp", "ld_mz", "st_zp", "st_mz"):
            suf = n.split("_")[1]
            ptr = {"x": 26, "y": 28, "z": 30}[[ch for ch in suf if ch in "xyz"][0]]
            ldst = n.startswith("ld")
            post = "p" in suf
            pre = "m" in suf

            def f(c):
                r = c.r
                a = (r[ptr + 1] << 8) | r[ptr]
                if pre:
                    a = (a - 1) & 0xFFFF
                if ldst:
                    r[d] = c.rd(a)
                else:
                    c.wr(a, r[d])
                if post:
                    a = (a + 1) & 0xFFFF
                if pre or post:
                    r[ptr], r[ptr + 1] = a & 0xFF, a >> 8
                c.pc = npc
                c.cyc += 2
            return f
        if n == "lds":
            k = o["k"]

            def f(c):
                c.r[d] = c.rd(k)
                c.pc = npc
                c.cyc += 2
            return f
        if n == "sts":
            k = o["k"]

            def f(c):
                c.wr(k, c.r[d])
                c.pc = npc
                c.cyc += 2
            return f
        if n == "push":
            def f(c):
                c.push(c.r[d])
                c.pc = npc
                c.cyc += 2
            return f
        if n == "pop":
            def f(c):
                c.r[d] = c.pop()
                c.pc = npc
                c.cyc += 2
            return f
        if n in ("lpm", "lpm_z", "lpm_zp"):
            dst = 0 if n == "lpm" else d
            post = n == "lpm_zp"

            def f(c):
                r = c.r
                z = (r[31] << 8) | r[30]
                wd = c.words[(z >> 1) & 0xFFF]
                r[dst] = (wd >> 8) & 0xFF if z & 1 else wd & 0xFF
                c.lpm_log[z] = c.lpm_log.get(z, 0) + 1
                if post:
                    z = (z + 1) & 0xFFFF
                    r[30], r[31] = z & 0xFF, z >> 8
                c.pc = npc
                c.cyc += 3
            return f
        # ---- vertakkingen
        if n == "rjmp":
            t = ins["target"] & 0xFFF

            def f(c):
                c.pc = t
                c.cyc += 2
            return f
        if n == "rcall":
            t = ins["target"] & 0xFFF

            def f(c):
                c.push(npc & 0xFF)
                c.push(npc >> 8)
                c.pc = t
                c.cyc += 3
            return f
        if n == "ret" or n == "reti":
            isreti = n == "reti"

            def f(c):
                hi = c.pop()
                lo = c.pop()
                c.pc = ((hi << 8) | lo) & 0xFFF
                if isreti:
                    c.sreg |= 0x80
                    c.int_block = True
                c.cyc += 4
            return f
        if n in ("ijmp", "icall"):
            call = n == "icall"

            def f(c):
                z = (c.r[31] << 8) | c.r[30]
                if call:
                    c.push(npc & 0xFF)
                    c.push(npc >> 8)
                c.pc = z & 0xFFF
                c.cyc += 3 if call else 2
            return f
        if n in ("brbs", "brbc"):
            b = o["b"]
            t = ins["target"] & 0xFFF
            want = 1 if n == "brbs" else 0

            def f(c):
                if ((c.sreg >> b) & 1) == want:
                    c.pc = t
                    c.cyc += 2
                else:
                    c.pc = npc
                    c.cyc += 1
            return f
        if n in ("nop", "break", "spm"):
            def f(c):
                c.pc = npc
                c.cyc += 1
            return f
        if n == "wdr":
            def f(c):
                c.wdt_last = c.cyc
                c.pc = npc
                c.cyc += 1
            return f
        if n == "sleep":
            def f(c):
                c.pc = npc
                c.cyc += 1
            return f
        raise NotImplementedError(n)
