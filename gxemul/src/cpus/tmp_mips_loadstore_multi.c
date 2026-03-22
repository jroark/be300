
/*  AUTOMATICALLY GENERATED! Do not edit.  */

#ifdef MODE32
X(multi_lw_2_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_load[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3)
	    || ((addr1 ^ addr0) & ~0xfff)) {
		mips32_loadstore[5](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	cpu->n_translated_instrs += 1;
	cpu->cd.mips.next_ic += 1;
}
#else
X(multi_lw_2_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_load[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3)
	    || ((addr1 ^ addr0) & ~0xfff)) {
		mips_loadstore[5](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	cpu->n_translated_instrs += 1;
	cpu->cd.mips.next_ic += 1;
}
#endif

#ifdef MODE32
X(multi_lw_3_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_load[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff)) {
		mips32_loadstore[5](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r2 = page[addr2];
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	r2 = LE32_TO_HOST(r2);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	reg(ic[2].arg[0]) = (MODE_int_t)(int32_t)r2;
	cpu->n_translated_instrs += 2;
	cpu->cd.mips.next_ic += 2;
}
#else
X(multi_lw_3_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_load[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff)) {
		mips_loadstore[5](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r2 = page[addr2];
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	r2 = LE32_TO_HOST(r2);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	reg(ic[2].arg[0]) = (MODE_int_t)(int32_t)r2;
	cpu->n_translated_instrs += 2;
	cpu->cd.mips.next_ic += 2;
}
#endif

#ifdef MODE32
X(multi_lw_4_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_load[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff)) {
		mips32_loadstore[5](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r2 = page[addr2];
	r3 = page[addr3];
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	r2 = LE32_TO_HOST(r2);
	r3 = LE32_TO_HOST(r3);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	reg(ic[2].arg[0]) = (MODE_int_t)(int32_t)r2;
	reg(ic[3].arg[0]) = (MODE_int_t)(int32_t)r3;
	cpu->n_translated_instrs += 3;
	cpu->cd.mips.next_ic += 3;
}
#else
X(multi_lw_4_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_load[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff)) {
		mips_loadstore[5](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r2 = page[addr2];
	r3 = page[addr3];
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	r2 = LE32_TO_HOST(r2);
	r3 = LE32_TO_HOST(r3);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	reg(ic[2].arg[0]) = (MODE_int_t)(int32_t)r2;
	reg(ic[3].arg[0]) = (MODE_int_t)(int32_t)r3;
	cpu->n_translated_instrs += 3;
	cpu->cd.mips.next_ic += 3;
}
#endif

#ifdef MODE32
X(multi_lw_5_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3, r4;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	MODE_uint_t addr4 = rX + (int32_t)ic[4].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_load[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3) || (addr4 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff) || ((addr4 ^ addr0) & ~0xfff)) {
		mips32_loadstore[5](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	addr4 = (addr4 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r2 = page[addr2];
	r3 = page[addr3];
	r4 = page[addr4];
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	r2 = LE32_TO_HOST(r2);
	r3 = LE32_TO_HOST(r3);
	r4 = LE32_TO_HOST(r4);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	reg(ic[2].arg[0]) = (MODE_int_t)(int32_t)r2;
	reg(ic[3].arg[0]) = (MODE_int_t)(int32_t)r3;
	reg(ic[4].arg[0]) = (MODE_int_t)(int32_t)r4;
	cpu->n_translated_instrs += 4;
	cpu->cd.mips.next_ic += 4;
}
#else
X(multi_lw_5_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3, r4;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	MODE_uint_t addr4 = rX + (int32_t)ic[4].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_load[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3) || (addr4 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff) || ((addr4 ^ addr0) & ~0xfff)) {
		mips_loadstore[5](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	addr4 = (addr4 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r2 = page[addr2];
	r3 = page[addr3];
	r4 = page[addr4];
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	r2 = LE32_TO_HOST(r2);
	r3 = LE32_TO_HOST(r3);
	r4 = LE32_TO_HOST(r4);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	reg(ic[2].arg[0]) = (MODE_int_t)(int32_t)r2;
	reg(ic[3].arg[0]) = (MODE_int_t)(int32_t)r3;
	reg(ic[4].arg[0]) = (MODE_int_t)(int32_t)r4;
	cpu->n_translated_instrs += 4;
	cpu->cd.mips.next_ic += 4;
}
#endif

#ifdef MODE32
X(multi_sw_2_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_store[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3)
	    || ((addr1 ^ addr0) & ~0xfff)) {
		mips32_loadstore[12](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	page[addr0] = r0;
	page[addr1] = r1;
	cpu->n_translated_instrs += 1;
	cpu->cd.mips.next_ic += 1;
}
#else
X(multi_sw_2_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_store[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3)
	    || ((addr1 ^ addr0) & ~0xfff)) {
		mips_loadstore[12](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	page[addr0] = r0;
	page[addr1] = r1;
	cpu->n_translated_instrs += 1;
	cpu->cd.mips.next_ic += 1;
}
#endif

#ifdef MODE32
X(multi_sw_3_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_store[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff)) {
		mips32_loadstore[12](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r2 = reg(ic[2].arg[0]);
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	r2 = LE32_TO_HOST(r2);
	page[addr0] = r0;
	page[addr1] = r1;
	page[addr2] = r2;
	cpu->n_translated_instrs += 2;
	cpu->cd.mips.next_ic += 2;
}
#else
X(multi_sw_3_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_store[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff)) {
		mips_loadstore[12](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r2 = reg(ic[2].arg[0]);
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	r2 = LE32_TO_HOST(r2);
	page[addr0] = r0;
	page[addr1] = r1;
	page[addr2] = r2;
	cpu->n_translated_instrs += 2;
	cpu->cd.mips.next_ic += 2;
}
#endif

#ifdef MODE32
X(multi_sw_4_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_store[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff)) {
		mips32_loadstore[12](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r2 = reg(ic[2].arg[0]);
	r3 = reg(ic[3].arg[0]);
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	r2 = LE32_TO_HOST(r2);
	r3 = LE32_TO_HOST(r3);
	page[addr0] = r0;
	page[addr1] = r1;
	page[addr2] = r2;
	page[addr3] = r3;
	cpu->n_translated_instrs += 3;
	cpu->cd.mips.next_ic += 3;
}
#else
X(multi_sw_4_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_store[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff)) {
		mips_loadstore[12](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r2 = reg(ic[2].arg[0]);
	r3 = reg(ic[3].arg[0]);
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	r2 = LE32_TO_HOST(r2);
	r3 = LE32_TO_HOST(r3);
	page[addr0] = r0;
	page[addr1] = r1;
	page[addr2] = r2;
	page[addr3] = r3;
	cpu->n_translated_instrs += 3;
	cpu->cd.mips.next_ic += 3;
}
#endif

#ifdef MODE32
X(multi_sw_5_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3, r4;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	MODE_uint_t addr4 = rX + (int32_t)ic[4].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_store[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3) || (addr4 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff) || ((addr4 ^ addr0) & ~0xfff)) {
		mips32_loadstore[12](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	addr4 = (addr4 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r2 = reg(ic[2].arg[0]);
	r3 = reg(ic[3].arg[0]);
	r4 = reg(ic[4].arg[0]);
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	r2 = LE32_TO_HOST(r2);
	r3 = LE32_TO_HOST(r3);
	r4 = LE32_TO_HOST(r4);
	page[addr0] = r0;
	page[addr1] = r1;
	page[addr2] = r2;
	page[addr3] = r3;
	page[addr4] = r4;
	cpu->n_translated_instrs += 4;
	cpu->cd.mips.next_ic += 4;
}
#else
X(multi_sw_5_le)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3, r4;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	MODE_uint_t addr4 = rX + (int32_t)ic[4].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_store[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3) || (addr4 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff) || ((addr4 ^ addr0) & ~0xfff)) {
		mips_loadstore[12](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	addr4 = (addr4 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r2 = reg(ic[2].arg[0]);
	r3 = reg(ic[3].arg[0]);
	r4 = reg(ic[4].arg[0]);
	r0 = LE32_TO_HOST(r0);
	r1 = LE32_TO_HOST(r1);
	r2 = LE32_TO_HOST(r2);
	r3 = LE32_TO_HOST(r3);
	r4 = LE32_TO_HOST(r4);
	page[addr0] = r0;
	page[addr1] = r1;
	page[addr2] = r2;
	page[addr3] = r3;
	page[addr4] = r4;
	cpu->n_translated_instrs += 4;
	cpu->cd.mips.next_ic += 4;
}
#endif

#ifdef MODE32
X(multi_lw_2_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_load[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3)
	    || ((addr1 ^ addr0) & ~0xfff)) {
		mips32_loadstore[21](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	cpu->n_translated_instrs += 1;
	cpu->cd.mips.next_ic += 1;
}
#else
X(multi_lw_2_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_load[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3)
	    || ((addr1 ^ addr0) & ~0xfff)) {
		mips_loadstore[21](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	cpu->n_translated_instrs += 1;
	cpu->cd.mips.next_ic += 1;
}
#endif

#ifdef MODE32
X(multi_lw_3_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_load[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff)) {
		mips32_loadstore[21](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r2 = page[addr2];
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	r2 = BE32_TO_HOST(r2);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	reg(ic[2].arg[0]) = (MODE_int_t)(int32_t)r2;
	cpu->n_translated_instrs += 2;
	cpu->cd.mips.next_ic += 2;
}
#else
X(multi_lw_3_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_load[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff)) {
		mips_loadstore[21](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r2 = page[addr2];
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	r2 = BE32_TO_HOST(r2);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	reg(ic[2].arg[0]) = (MODE_int_t)(int32_t)r2;
	cpu->n_translated_instrs += 2;
	cpu->cd.mips.next_ic += 2;
}
#endif

#ifdef MODE32
X(multi_lw_4_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_load[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff)) {
		mips32_loadstore[21](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r2 = page[addr2];
	r3 = page[addr3];
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	r2 = BE32_TO_HOST(r2);
	r3 = BE32_TO_HOST(r3);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	reg(ic[2].arg[0]) = (MODE_int_t)(int32_t)r2;
	reg(ic[3].arg[0]) = (MODE_int_t)(int32_t)r3;
	cpu->n_translated_instrs += 3;
	cpu->cd.mips.next_ic += 3;
}
#else
X(multi_lw_4_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_load[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff)) {
		mips_loadstore[21](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r2 = page[addr2];
	r3 = page[addr3];
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	r2 = BE32_TO_HOST(r2);
	r3 = BE32_TO_HOST(r3);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	reg(ic[2].arg[0]) = (MODE_int_t)(int32_t)r2;
	reg(ic[3].arg[0]) = (MODE_int_t)(int32_t)r3;
	cpu->n_translated_instrs += 3;
	cpu->cd.mips.next_ic += 3;
}
#endif

#ifdef MODE32
X(multi_lw_5_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3, r4;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	MODE_uint_t addr4 = rX + (int32_t)ic[4].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_load[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3) || (addr4 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff) || ((addr4 ^ addr0) & ~0xfff)) {
		mips32_loadstore[21](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	addr4 = (addr4 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r2 = page[addr2];
	r3 = page[addr3];
	r4 = page[addr4];
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	r2 = BE32_TO_HOST(r2);
	r3 = BE32_TO_HOST(r3);
	r4 = BE32_TO_HOST(r4);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	reg(ic[2].arg[0]) = (MODE_int_t)(int32_t)r2;
	reg(ic[3].arg[0]) = (MODE_int_t)(int32_t)r3;
	reg(ic[4].arg[0]) = (MODE_int_t)(int32_t)r4;
	cpu->n_translated_instrs += 4;
	cpu->cd.mips.next_ic += 4;
}
#else
X(multi_lw_5_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3, r4;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	MODE_uint_t addr4 = rX + (int32_t)ic[4].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_load[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3) || (addr4 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff) || ((addr4 ^ addr0) & ~0xfff)) {
		mips_loadstore[21](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	addr4 = (addr4 >> 2) & 0x3ff;
	r0 = page[addr0];
	r1 = page[addr1];
	r2 = page[addr2];
	r3 = page[addr3];
	r4 = page[addr4];
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	r2 = BE32_TO_HOST(r2);
	r3 = BE32_TO_HOST(r3);
	r4 = BE32_TO_HOST(r4);
	reg(ic[0].arg[0]) = (MODE_int_t)(int32_t)r0;
	reg(ic[1].arg[0]) = (MODE_int_t)(int32_t)r1;
	reg(ic[2].arg[0]) = (MODE_int_t)(int32_t)r2;
	reg(ic[3].arg[0]) = (MODE_int_t)(int32_t)r3;
	reg(ic[4].arg[0]) = (MODE_int_t)(int32_t)r4;
	cpu->n_translated_instrs += 4;
	cpu->cd.mips.next_ic += 4;
}
#endif

#ifdef MODE32
X(multi_sw_2_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_store[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3)
	    || ((addr1 ^ addr0) & ~0xfff)) {
		mips32_loadstore[28](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	page[addr0] = r0;
	page[addr1] = r1;
	cpu->n_translated_instrs += 1;
	cpu->cd.mips.next_ic += 1;
}
#else
X(multi_sw_2_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_store[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3)
	    || ((addr1 ^ addr0) & ~0xfff)) {
		mips_loadstore[28](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	page[addr0] = r0;
	page[addr1] = r1;
	cpu->n_translated_instrs += 1;
	cpu->cd.mips.next_ic += 1;
}
#endif

#ifdef MODE32
X(multi_sw_3_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_store[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff)) {
		mips32_loadstore[28](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r2 = reg(ic[2].arg[0]);
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	r2 = BE32_TO_HOST(r2);
	page[addr0] = r0;
	page[addr1] = r1;
	page[addr2] = r2;
	cpu->n_translated_instrs += 2;
	cpu->cd.mips.next_ic += 2;
}
#else
X(multi_sw_3_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_store[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff)) {
		mips_loadstore[28](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r2 = reg(ic[2].arg[0]);
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	r2 = BE32_TO_HOST(r2);
	page[addr0] = r0;
	page[addr1] = r1;
	page[addr2] = r2;
	cpu->n_translated_instrs += 2;
	cpu->cd.mips.next_ic += 2;
}
#endif

#ifdef MODE32
X(multi_sw_4_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_store[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff)) {
		mips32_loadstore[28](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r2 = reg(ic[2].arg[0]);
	r3 = reg(ic[3].arg[0]);
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	r2 = BE32_TO_HOST(r2);
	r3 = BE32_TO_HOST(r3);
	page[addr0] = r0;
	page[addr1] = r1;
	page[addr2] = r2;
	page[addr3] = r3;
	cpu->n_translated_instrs += 3;
	cpu->cd.mips.next_ic += 3;
}
#else
X(multi_sw_4_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_store[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff)) {
		mips_loadstore[28](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r2 = reg(ic[2].arg[0]);
	r3 = reg(ic[3].arg[0]);
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	r2 = BE32_TO_HOST(r2);
	r3 = BE32_TO_HOST(r3);
	page[addr0] = r0;
	page[addr1] = r1;
	page[addr2] = r2;
	page[addr3] = r3;
	cpu->n_translated_instrs += 3;
	cpu->cd.mips.next_ic += 3;
}
#endif

#ifdef MODE32
X(multi_sw_5_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3, r4;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	MODE_uint_t addr4 = rX + (int32_t)ic[4].arg[2];
	uint32_t index0 = addr0 >> 12;
	page = (uint32_t *) cpu->cd.mips.host_store[index0];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3) || (addr4 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff) || ((addr4 ^ addr0) & ~0xfff)) {
		mips32_loadstore[28](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	addr4 = (addr4 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r2 = reg(ic[2].arg[0]);
	r3 = reg(ic[3].arg[0]);
	r4 = reg(ic[4].arg[0]);
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	r2 = BE32_TO_HOST(r2);
	r3 = BE32_TO_HOST(r3);
	r4 = BE32_TO_HOST(r4);
	page[addr0] = r0;
	page[addr1] = r1;
	page[addr2] = r2;
	page[addr3] = r3;
	page[addr4] = r4;
	cpu->n_translated_instrs += 4;
	cpu->cd.mips.next_ic += 4;
}
#else
X(multi_sw_5_be)
{
	uint32_t *page;
	MODE_uint_t rX = reg(ic[0].arg[1]), r0, r1, r2, r3, r4;
	MODE_uint_t addr0 = rX + (int32_t)ic[0].arg[2];
	MODE_uint_t addr1 = rX + (int32_t)ic[1].arg[2];
	MODE_uint_t addr2 = rX + (int32_t)ic[2].arg[2];
	MODE_uint_t addr3 = rX + (int32_t)ic[3].arg[2];
	MODE_uint_t addr4 = rX + (int32_t)ic[4].arg[2];
	const uint32_t mask1 = (1 << DYNTRANS_L1N) - 1;
	const uint32_t mask2 = (1 << DYNTRANS_L2N) - 1;
	const uint32_t mask3 = (1 << DYNTRANS_L3N) - 1;
	uint32_t x1, x2, x3;
	struct DYNTRANS_L2_64_TABLE *l2;
	struct DYNTRANS_L3_64_TABLE *l3;
	x1 = (addr0 >> (64-DYNTRANS_L1N)) & mask1;
	x2 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N)) & mask2;
	x3 = (addr0 >> (64-DYNTRANS_L1N-DYNTRANS_L2N-DYNTRANS_L3N)) & mask3;
	l2 = cpu->cd.DYNTRANS_ARCH.l1_64[x1];
	l3 = l2->l3[x2];
	page = (uint32_t *) l3->host_store[x3];
	if (cpu->delay_slot ||
	    page == NULL || (addr0 & 3) || (addr1 & 3) || (addr2 & 3) || (addr3 & 3) || (addr4 & 3)
	    || ((addr1 ^ addr0) & ~0xfff) || ((addr2 ^ addr0) & ~0xfff) || ((addr3 ^ addr0) & ~0xfff) || ((addr4 ^ addr0) & ~0xfff)) {
		mips_loadstore[28](cpu, ic);
		return;
	}
	addr0 = (addr0 >> 2) & 0x3ff;
	addr1 = (addr1 >> 2) & 0x3ff;
	addr2 = (addr2 >> 2) & 0x3ff;
	addr3 = (addr3 >> 2) & 0x3ff;
	addr4 = (addr4 >> 2) & 0x3ff;
	r0 = reg(ic[0].arg[0]);
	r1 = reg(ic[1].arg[0]);
	r2 = reg(ic[2].arg[0]);
	r3 = reg(ic[3].arg[0]);
	r4 = reg(ic[4].arg[0]);
	r0 = BE32_TO_HOST(r0);
	r1 = BE32_TO_HOST(r1);
	r2 = BE32_TO_HOST(r2);
	r3 = BE32_TO_HOST(r3);
	r4 = BE32_TO_HOST(r4);
	page[addr0] = r0;
	page[addr1] = r1;
	page[addr2] = r2;
	page[addr3] = r3;
	page[addr4] = r4;
	cpu->n_translated_instrs += 4;
	cpu->cd.mips.next_ic += 4;
}
#endif

