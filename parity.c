/*
 * BRIEF DESCRIPTION
 *
 * Parity related methods.
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include "nova.h"

static int nova_calculate_block_parity(struct super_block *sb, u8 *parity,
	u8 *block)
{
	unsigned int strp, num_strps, i, j;
	size_t strp_size = NOVA_STRIPE_SIZE;
	unsigned int strp_shift = NOVA_STRIPE_SHIFT;
	u64 xor;

	num_strps = sb->s_blocksize >> strp_shift;
	if ( static_cpu_has(X86_FEATURE_XMM2) ) { // sse2 128b
		for (i = 0; i < strp_size; i += 16) {
			asm volatile("movdqa %0, %%xmm0" : : "m" (block[i]));
			for (strp = 1; strp < num_strps; strp++) {
				j = (strp << strp_shift) + i;
				asm volatile(
					"movdqa     %0, %%xmm1\n"
					"pxor   %%xmm1, %%xmm0\n"
					: : "m" (block[j])
				);
			}
			asm volatile("movntdq %%xmm0, %0" : "=m" (parity[i]));
		}
	} else { // common 64b
		for (i = 0; i < strp_size; i += 8) {
			xor = *((u64 *) &block[i]);
			for (strp = 1; strp < num_strps; strp++) {
				j = (strp << strp_shift) + i;
				xor ^= *((u64 *) &block[j]);
			}
			*((u64 *) &parity[i]) = xor;
		}
	}

	return 0;
}

/* Compute parity for a whole data block and write the parity stripe to nvmm
 *
 * The block buffer to compute checksums should reside in dram (more trusted),
 * not in nvmm (less trusted).
 *
 * block:   block buffer with user data and possibly partial head-tail block
 *          - should be in kernel memory (dram) to avoid page faults
 * blocknr: destination nvmm block number where the block is written to
 *          - used to derive the parity stripe address

 * If the modified content is less than a stripe size (small writes), it's
 * possible to re-compute the parity only using the difference of the modified
 * stripe, without re-computing for the whole block.

static int nova_update_block_parity(struct super_block *sb,
	struct nova_inode_info_header *sih, void *block, unsigned long blocknr,
	size_t offset, size_t bytes, int zero)

 */
static int nova_update_block_parity(struct super_block *sb, u8 *block,
	unsigned long blocknr, int zero)
{
	size_t strp_size = NOVA_STRIPE_SIZE;
	void *parity, *nvmmptr;
	int ret = 0;
	timing_t block_parity_time;

	NOVA_START_TIMING(block_parity_t, block_parity_time);

	parity = kmalloc(strp_size, GFP_KERNEL);
	if (parity == NULL) {
		nova_err(sb, "%s: parity buffer allocation error\n", __func__);
		ret = -ENOMEM;
		goto out;
	}

	if (block == NULL) {
		nova_dbg("%s: block pointer error\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	if (unlikely(zero))
		memset(parity, 0, strp_size);
	else
		nova_calculate_block_parity(sb, parity, block);

	nvmmptr = nova_get_parity_addr(sb, blocknr);

	nova_memunlock_range(sb, nvmmptr, strp_size);
	memcpy_to_pmem_nocache(nvmmptr, parity, strp_size);
	nova_memlock_range(sb, nvmmptr, strp_size);

	// TODO: The parity stripe is better checksummed for higher reliability.
out:
	if (parity != NULL) kfree(parity);

	NOVA_END_TIMING(block_parity_t, block_parity_time);

	return 0;
}

int nova_update_pgoff_parity(struct super_block *sb,
	struct nova_inode_info_header *sih, struct nova_file_write_entry *entry,
	unsigned long pgoff, int zero)
{
	unsigned long blocknr;
	void *dax_mem = NULL;
	u64 blockoff;

	blockoff = nova_find_nvmm_block(sb, sih, entry, pgoff);
	/* Truncated? */
	if (blockoff == 0)
		return 0;

	dax_mem = nova_get_block(sb, blockoff);

	blocknr = nova_get_blocknr(sb, blockoff, sih->i_blk_type);
	nova_update_block_parity(sb, dax_mem, blocknr, zero);

	return 0;
}

/* Update block checksums and/or parity.
 *
 * Since this part of computing is along the critical path, unroll by 8 to gain
 * performance if possible. This unrolling applies to stripe width of 8 and
 * whole block writes.
 */
#define CSUM0 NOVA_INIT_CSUM
int nova_update_block_csum_parity(struct super_block *sb,
	struct nova_inode_info_header *sih, u8 *block, unsigned long blocknr,
	size_t offset, size_t bytes)
{
	unsigned int i, strp_offset, num_strps;
	size_t csum_size = NOVA_DATA_CSUM_LEN;
	size_t strp_size = NOVA_STRIPE_SIZE;
	unsigned int strp_shift = NOVA_STRIPE_SHIFT;
	unsigned long strp_nr, blockoff, blocksize = sb->s_blocksize;
	void *nvmmptr, *nvmmptr1;
	u32 crc[8];
	u64 qwd[8], *parity = NULL;
	u64 acc[8] = {CSUM0, CSUM0, CSUM0, CSUM0, CSUM0, CSUM0, CSUM0, CSUM0};
	bool unroll_csum = false, unroll_parity = false;
	int ret = 0;
	timing_t block_csum_parity_time;

	NOVA_START_TIMING(block_csum_parity_t, block_csum_parity_time);

	blockoff = nova_get_block_off(sb, blocknr, sih->i_blk_type);
	strp_nr = blockoff >> strp_shift;

	strp_offset = offset & (strp_size - 1);
	num_strps = ((strp_offset + bytes - 1) >> strp_shift) + 1;

	unroll_parity = (blocksize / strp_size == 8) && (num_strps == 8);
	unroll_csum = unroll_parity && static_cpu_has(X86_FEATURE_XMM4_2);

	/* unrolled-by-8 implementation */
	if (unroll_csum || unroll_parity) {
		if (data_parity > 0) {
			parity = (u64 *) kmalloc(strp_size, GFP_KERNEL);
			if (parity == NULL) {
				nova_err(sb, "%s: buffer allocation error\n",
								__func__);
				ret = -ENOMEM;
				goto out;
			}
		}
		for (i = 0; i < strp_size / 8; i++) {
			qwd[0] = *((u64 *) (block));
			qwd[1] = *((u64 *) (block + 1 * strp_size));
			qwd[2] = *((u64 *) (block + 2 * strp_size));
			qwd[3] = *((u64 *) (block + 3 * strp_size));
			qwd[4] = *((u64 *) (block + 4 * strp_size));
			qwd[5] = *((u64 *) (block + 5 * strp_size));
			qwd[6] = *((u64 *) (block + 6 * strp_size));
			qwd[7] = *((u64 *) (block + 7 * strp_size));

			if (data_csum > 0 && unroll_csum) {
				nova_crc32c_qword(qwd[0], acc[0]);
				nova_crc32c_qword(qwd[1], acc[1]);
				nova_crc32c_qword(qwd[2], acc[2]);
				nova_crc32c_qword(qwd[3], acc[3]);
				nova_crc32c_qword(qwd[4], acc[4]);
				nova_crc32c_qword(qwd[5], acc[5]);
				nova_crc32c_qword(qwd[6], acc[6]);
				nova_crc32c_qword(qwd[7], acc[7]);
			}

			if (data_parity > 0) {
				parity[i] =	qwd[0] ^ qwd[1] ^ qwd[2] ^ \
						qwd[3] ^ qwd[4] ^ qwd[5] ^ \
						qwd[6] ^ qwd[7];
			}

			block += 8;
		}
		if (data_csum > 0 && unroll_csum) {
			crc[0] = cpu_to_le32( (u32) acc[0] );
			crc[1] = cpu_to_le32( (u32) acc[1] );
			crc[2] = cpu_to_le32( (u32) acc[2] );
			crc[3] = cpu_to_le32( (u32) acc[3] );
			crc[4] = cpu_to_le32( (u32) acc[4] );
			crc[5] = cpu_to_le32( (u32) acc[5] );
			crc[6] = cpu_to_le32( (u32) acc[6] );
			crc[7] = cpu_to_le32( (u32) acc[7] );

			nvmmptr = nova_get_data_csum_addr(sb, strp_nr, 0);
			nvmmptr1 = nova_get_data_csum_addr(sb, strp_nr, 1);
			nova_memunlock_range(sb, nvmmptr, csum_size * 8);
			memcpy_to_pmem_nocache(nvmmptr, crc, csum_size * 8);
			memcpy_to_pmem_nocache(nvmmptr1, crc, csum_size * 8);
			nova_memlock_range(sb, nvmmptr, csum_size * 8);
		}

		nvmmptr = nova_get_parity_addr(sb, blocknr);
		nova_memunlock_range(sb, nvmmptr, strp_size);
		memcpy_to_pmem_nocache(nvmmptr, parity, strp_size);
		nova_memlock_range(sb, nvmmptr, strp_size);
	}

	if (data_csum > 0 && !unroll_csum)
		nova_update_block_csum(sb, sih, block, blocknr, offset, bytes, 0);
	if (data_parity > 0 && !unroll_parity)
		nova_update_block_parity(sb, block, blocknr, 0);

out:
	if (parity != NULL) kfree(parity);

	NOVA_END_TIMING(block_csum_parity_t, block_csum_parity_time);

	return 0;
}

/* Restore a stripe of data. */
int nova_restore_data(struct super_block *sb, unsigned long blocknr,
        unsigned int bad_strp_id)
{
	unsigned int i, num_strps;
	size_t strp_size = NOVA_STRIPE_SIZE;
	unsigned int strp_shift = NOVA_STRIPE_SHIFT;
	size_t blockoff, offset;
	unsigned long bad_strp_nr;
	u8 *blockptr, *bad_strp, *blockbuf, *stripe, *parity;
	u32 csum_calc, csum_nvmm, *csum_addr;
	u32 csum_nvmm1, *csum_addr1;
	bool match;
	timing_t restore_time;
	int ret = 0;

	NOVA_START_TIMING(restore_data_t, restore_time);
	blockoff = nova_get_block_off(sb, blocknr, NOVA_BLOCK_TYPE_4K);
	blockptr = nova_get_block(sb, blockoff);
	bad_strp = blockptr + (bad_strp_id << strp_shift);
	bad_strp_nr = (blockoff + (bad_strp_id << strp_shift)) >> strp_shift;

	stripe = kmalloc(strp_size, GFP_KERNEL);
	blockbuf = kmalloc(sb->s_blocksize, GFP_KERNEL);
	if (stripe == NULL || blockbuf == NULL) {
		nova_err(sb, "%s: buffer allocation error\n", __func__);
		ret = -ENOMEM;
		goto out;
	}

	parity = nova_get_parity_addr(sb, blocknr);
	if (parity == NULL) {
		nova_err(sb, "%s: parity address error\n", __func__);
		ret = -EIO;
		goto out;
	}

	num_strps = sb->s_blocksize >> strp_shift;
	for (i = 0; i < num_strps; i++) {
		offset = i << strp_shift;
		if (i == bad_strp_id)
			ret = memcpy_from_pmem(blockbuf + offset,
							parity, strp_size);
		else
			ret = memcpy_from_pmem(blockbuf + offset,
						blockptr + offset, strp_size);
		if (ret < 0) {
			nova_err(sb, "%s: unrecoverable media error\n",
							__func__);
			goto out;
		}
	}

	nova_calculate_block_parity(sb, stripe, blockbuf);

	csum_calc = nova_crc32c(NOVA_INIT_CSUM, stripe, strp_size);
	csum_addr = nova_get_data_csum_addr(sb, bad_strp_nr, 0);
	csum_nvmm = le32_to_cpu(*csum_addr);
	csum_addr1 = nova_get_data_csum_addr(sb, bad_strp_nr, 1);
	csum_nvmm1 = le32_to_cpu(*csum_addr1);
	match     = (csum_calc == csum_nvmm) || (csum_calc == csum_nvmm1);

	if (match) {
		nova_memunlock_range(sb, bad_strp, strp_size);
	        memcpy_to_pmem_nocache(bad_strp, stripe, strp_size);
		nova_memlock_range(sb, bad_strp, strp_size);
	}

	if (!match) ret = -EIO;

out:
	if (stripe != NULL) kfree(stripe);
	if (blockbuf != NULL) kfree(blockbuf);

	NOVA_END_TIMING(restore_data_t, restore_time);
	return ret;
}

int nova_update_truncated_block_parity(struct super_block *sb,
	struct inode *inode, loff_t newsize)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	unsigned long pgoff, blocknr;
	u64 nvmm;
	char *nvmm_addr, *block;
	u8 btype = sih->i_blk_type;

	pgoff = newsize >> sb->s_blocksize_bits;

	nvmm = nova_find_nvmm_block(sb, sih, NULL, pgoff);
	if (nvmm == 0)
		return -EFAULT;

	nvmm_addr = (char *)nova_get_block(sb, nvmm);

	blocknr = nova_get_blocknr(sb, nvmm, btype);

	/* Copy to DRAM to catch MCE.
	block = kmalloc(blocksize, GFP_KERNEL);
	if (block == NULL) {
		nova_err(sb, "%s: buffer allocation error\n", __func__);
		return -ENOMEM;
	}
	*/

//	memcpy_from_pmem(block, nvmm_addr, blocksize);
	block = nvmm_addr;

	nova_update_block_parity(sb, block, blocknr, 0);

//	kfree(blkbuf);

	return 0;
}

int nova_data_parity_init_free_list(struct super_block *sb,
	struct free_list *free_list)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	unsigned long blocksize, total_blocks, parity_blocks;
	size_t strp_size = NOVA_STRIPE_SIZE;

	/* Allocate blocks to store data block checksums.
	 * Always reserve in case user turns it off at init mount but later
	 * turns it on. */
	blocksize = sb->s_blocksize;
	total_blocks = sbi->initsize / blocksize;
	parity_blocks = total_blocks / (blocksize / strp_size + 1);
	if (total_blocks % (blocksize / strp_size + 1))
		parity_blocks++;

	free_list->parity_start = free_list->block_start;
	free_list->block_start += parity_blocks / sbi->cpus;
	if (parity_blocks % sbi->cpus)
		free_list->block_start++;

	free_list->num_parity_blocks =
		free_list->block_start - free_list->parity_start;

	return 0;
}

