/*
 * BRIEF DESCRIPTION
 *
 * DAX file operations.
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/buffer_head.h>
#include <asm/cpufeature.h>
#include <asm/pgtable.h>
#include <linux/version.h>
#include "nova.h"

static ssize_t
do_dax_mapping_read(struct file *filp, char __user *buf,
	size_t len, loff_t *ppos)
{
	struct inode *inode = filp->f_mapping->host;
	struct super_block *sb = inode->i_sb;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_file_write_entry *entry;
	pgoff_t index, end_index;
	unsigned long offset;
	loff_t isize, pos;
	size_t copied = 0, error = 0;
	timing_t memcpy_time;

	pos = *ppos;
	index = pos >> PAGE_SHIFT;
	offset = pos & ~PAGE_MASK;

	if (!access_ok(VERIFY_WRITE, buf, len)) {
		error = -EFAULT;
		goto out;
	}

	isize = i_size_read(inode);
	if (!isize)
		goto out;

	nova_dbgv("%s: inode %lu, offset %lld, count %lu, size %lld\n",
		__func__, inode->i_ino,	pos, len, isize);

	if (len > isize - pos)
		len = isize - pos;

	if (len <= 0)
		goto out;

	end_index = (isize - 1) >> PAGE_SHIFT;
	do {
		unsigned long nr, left;
		unsigned long nvmm;
		unsigned long csum_blks;
		void *dax_mem = NULL;
		int zero = 0;

		/* nr is the maximum number of bytes to copy from this page */
		if (index >= end_index) {
			if (index > end_index)
				goto out;
			nr = ((isize - 1) & ~PAGE_MASK) + 1;
			if (nr <= offset) {
				goto out;
			}
		}

		entry = nova_get_write_entry(sb, si, index);
		if (unlikely(entry == NULL)) {
			nova_dbgv("Required extent not found: pgoff %lu, "
				"inode size %lld\n", index, isize);
			nr = PAGE_SIZE;
			zero = 1;
			goto memcpy;
		}

		/* Find contiguous blocks */
		if (index < entry->pgoff ||
			index - entry->pgoff >= entry->num_pages) {
			nova_err(sb, "%s ERROR: %lu, entry pgoff %llu, num %u, "
				"blocknr %llu\n", __func__, index, entry->pgoff,
				entry->num_pages, entry->block >> PAGE_SHIFT);
			return -EINVAL;
		}
		if (entry->reassigned == 0) {
			nr = (entry->num_pages - (index - entry->pgoff))
				* PAGE_SIZE;
		} else {
			nr = PAGE_SIZE;
		}

		nvmm = get_nvmm(sb, sih, entry, index);
		dax_mem = nova_get_block(sb, (nvmm << PAGE_SHIFT));

memcpy:
		nr = nr - offset;
		if (nr > len - copied)
			nr = len - copied;

		if ( (!zero) && (NOVA_SB(sb)->block_csum_base) ) {
			/* only whole blocks can be verified */
			csum_blks = ((offset + nr - 1) >> PAGE_SHIFT) + 1;
			if (!nova_verify_data_csum(inode, entry,
						index, csum_blks)) {
				nova_err(sb, "%s: nova data checksum fail! "
					"inode %lu entry pgoff %lu "
					"index %lu blocks %lu\n", __func__,
					inode->i_ino, entry->pgoff,
					index, csum_blks);
				error = -EIO;
				goto out;
			}
		}

		NOVA_START_TIMING(memcpy_r_nvmm_t, memcpy_time);

		if (!zero)
			left = __copy_to_user(buf + copied,
						dax_mem + offset, nr);
		else
			left = __clear_user(buf + copied, nr);

		NOVA_END_TIMING(memcpy_r_nvmm_t, memcpy_time);

		if (left) {
			nova_dbg("%s ERROR!: bytes %lu, left %lu\n",
				__func__, nr, left);
			error = -EFAULT;
			goto out;
		}

		copied += (nr - left);
		offset += (nr - left);
		index += offset >> PAGE_SHIFT;
		offset &= ~PAGE_MASK;
	} while (copied < len);

out:
	*ppos = pos + copied;
	if (filp)
		file_accessed(filp);

	NOVA_STATS_ADD(read_bytes, copied);

	nova_dbgv("%s returned %zu\n", __func__, copied);
	return (copied ? copied : error);
}

/*
 * Wrappers. We need to use the rcu read lock to avoid
 * concurrent truncate operation. No problem for write because we held
 * i_mutex.
 */
ssize_t nova_dax_file_read(struct file *filp, char __user *buf,
			    size_t len, loff_t *ppos)
{
	ssize_t res;
	timing_t dax_read_time;

	NOVA_START_TIMING(dax_read_t, dax_read_time);
//	rcu_read_lock();
	res = do_dax_mapping_read(filp, buf, len, ppos);
//	rcu_read_unlock();
	NOVA_END_TIMING(dax_read_t, dax_read_time);
	return res;
}

static inline int nova_copy_partial_block(struct super_block *sb,
	struct nova_inode_info_header *sih,
	struct nova_file_write_entry *entry, unsigned long index,
	size_t offset, void* kmem, bool is_end_blk)
{
	void *ptr;
	unsigned long nvmm;

	nvmm = get_nvmm(sb, sih, entry, index);
	ptr = nova_get_block(sb, (nvmm << PAGE_SHIFT));
	if (ptr != NULL) {
		if (is_end_blk)
			memcpy(kmem + offset, ptr + offset,
				sb->s_blocksize - offset);
		else 
			memcpy(kmem, ptr, offset);
	}

	return 0;
}

/* 
 * Fill the new start/end block from original blocks.
 * Do nothing if fully covered; copy if original blocks present;
 * Fill zero otherwise.
 */
static void nova_handle_head_tail_blocks(struct super_block *sb,
	struct nova_inode *pi, struct inode *inode, loff_t pos, size_t count,
	void *kmem)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	size_t offset, eblk_offset;
	unsigned long start_blk, end_blk, num_blocks;
	struct nova_file_write_entry *entry;
	timing_t partial_time;

	NOVA_START_TIMING(partial_block_t, partial_time);
	offset = pos & (sb->s_blocksize - 1);
	num_blocks = ((count + offset - 1) >> sb->s_blocksize_bits) + 1;
	/* offset in the actual block size block */
	offset = pos & (nova_inode_blk_size(pi) - 1);
	start_blk = pos >> sb->s_blocksize_bits;
	end_blk = start_blk + num_blocks - 1;

	nova_dbg_verbose("%s: %lu blocks\n", __func__, num_blocks);
	/* We avoid zeroing the alloc'd range, which is going to be overwritten
	 * by this system call anyway */
	nova_dbg_verbose("%s: start offset %lu start blk %lu %p\n", __func__,
				offset, start_blk, kmem);
	if (offset != 0) {
		entry = nova_get_write_entry(sb, si, start_blk);
		if (entry == NULL) {
			/* Fill zero */
		    	memset(kmem, 0, offset);
		} else {
			/* Copy from original block */
			nova_copy_partial_block(sb, sih, entry, start_blk,
					offset, kmem, false);
		}
		nova_flush_buffer(kmem, offset, 0);
	}

	kmem = (void *)((char *)kmem +
			((num_blocks - 1) << sb->s_blocksize_bits));
	eblk_offset = (pos + count) & (nova_inode_blk_size(pi) - 1);
	nova_dbg_verbose("%s: end offset %lu, end blk %lu %p\n", __func__,
				eblk_offset, end_blk, kmem);
	if (eblk_offset != 0) {
		entry = nova_get_write_entry(sb, si, end_blk);
		if (entry == NULL) {
			/* Fill zero */
		    	memset(kmem + eblk_offset, 0,
					sb->s_blocksize - eblk_offset);
		} else {
			/* Copy from original block */
			nova_copy_partial_block(sb, sih, entry, end_blk,
					eblk_offset, kmem, true);
		}
		nova_flush_buffer(kmem + eblk_offset,
					sb->s_blocksize - eblk_offset, 0);
	}

	NOVA_END_TIMING(partial_block_t, partial_time);
}

int nova_reassign_file_tree(struct super_block *sb,
	struct nova_inode *pi, struct nova_inode_info_header *sih,
	u64 begin_tail)
{
	struct nova_file_write_entry *entry_data;
	u64 curr_p = begin_tail;
	size_t entry_size = sizeof(struct nova_file_write_entry);

	while (curr_p != pi->log_tail) {
		if (is_last_entry(curr_p, entry_size))
			curr_p = next_log_page(sb, curr_p);

		if (curr_p == 0) {
			nova_err(sb, "%s: File inode %llu log is NULL!\n",
				__func__, pi->nova_ino);
			return -EINVAL;
		}

		entry_data = (struct nova_file_write_entry *)
					nova_get_block(sb, curr_p);

		if (nova_get_entry_type(entry_data) != FILE_WRITE) {
			nova_dbg("%s: entry type is not write? %d\n",
				__func__, nova_get_entry_type(entry_data));
			curr_p += entry_size;
			continue;
		}

		nova_assign_write_entry(sb, pi, sih, entry_data, true);
		curr_p += entry_size;
	}

	return 0;
}

static int nova_cleanup_incomplete_write(struct super_block *sb,
	struct nova_inode *pi, struct nova_inode_info_header *sih,
	unsigned long blocknr, int allocated, u64 begin_tail, u64 end_tail)
{
	struct nova_file_write_entry *entry;
	u64 curr_p = begin_tail;
	size_t entry_size = sizeof(struct nova_file_write_entry);

	if (blocknr > 0 && allocated > 0)
		nova_free_data_blocks(sb, pi, blocknr, allocated);

	if (begin_tail == 0 || end_tail == 0)
		return 0;

	while (curr_p != end_tail) {
		if (is_last_entry(curr_p, entry_size))
			curr_p = next_log_page(sb, curr_p);

		if (curr_p == 0) {
			nova_err(sb, "%s: File inode %llu log is NULL!\n",
				__func__, pi->nova_ino);
			return -EINVAL;
		}

		entry = (struct nova_file_write_entry *)
					nova_get_block(sb, curr_p);

		if (nova_get_entry_type(entry) != FILE_WRITE) {
			nova_dbg("%s: entry type is not write? %d\n",
				__func__, nova_get_entry_type(entry));
			curr_p += entry_size;
			continue;
		}

		blocknr = entry->block >> PAGE_SHIFT;
		nova_free_data_blocks(sb, pi, blocknr, entry->num_pages);
		curr_p += entry_size;
	}

	return 0;
}

ssize_t nova_cow_file_write(struct file *filp,
	const char __user *buf,	size_t len, loff_t *ppos, bool need_mutex)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode    *inode = mapping->host;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct super_block *sb = inode->i_sb;
	struct nova_inode *pi;
	struct nova_file_write_entry entry_data;
	ssize_t     written = 0;
	loff_t pos;
	size_t count, offset, copied, csummed, ret;
	unsigned long start_blk, num_blocks;
	unsigned long total_blocks;
	unsigned long blocknr = 0;
	unsigned int data_bits;
	int allocated = 0;
	void* kmem;
	u64 curr_entry;
	size_t bytes;
	long status = 0;
	timing_t cow_write_time, memcpy_time;
	unsigned long step = 0;
	u64 temp_tail = 0, begin_tail = 0;
	u64 trans_id;
	u32 time;

	if (len == 0)
		return 0;

	/*
	 * We disallow writing to a mmaped file,
	 * since write is copy-on-write while mmap is DAX (in-place).
	 */
	if (mapping_mapped(mapping))
		return -EACCES;

	NOVA_START_TIMING(cow_write_t, cow_write_time);

	sb_start_write(inode->i_sb);
	if (need_mutex)
		mutex_lock(&inode->i_mutex);

	if (!access_ok(VERIFY_READ, buf, len)) {
		ret = -EFAULT;
		goto out;
	}
	pos = *ppos;

	if (filp->f_flags & O_APPEND)
		pos = i_size_read(inode);

	count = len;

	pi = nova_get_inode(sb, inode);

	offset = pos & (sb->s_blocksize - 1);
	num_blocks = ((count + offset - 1) >> sb->s_blocksize_bits) + 1;
	total_blocks = num_blocks;

	/* offset in the actual block size block */

	ret = file_remove_privs(filp);
	if (ret) {
		goto out;
	}
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	time = CURRENT_TIME_SEC.tv_sec;

	nova_dbgv("%s: inode %lu, offset %lld, count %lu\n",
			__func__, inode->i_ino,	pos, count);

	trans_id = nova_get_trans_id(sb);
	temp_tail = pi->log_tail;
	while (num_blocks > 0) {
		offset = pos & (nova_inode_blk_size(pi) - 1);
		start_blk = pos >> sb->s_blocksize_bits;

		/* don't zero-out the allocated blocks */
		allocated = nova_new_data_blocks(sb, pi, &blocknr, num_blocks,
						start_blk, 0, 1);
		nova_dbg_verbose("%s: alloc %d blocks @ %lu\n", __func__,
						allocated, blocknr);

		if (allocated <= 0) {
			nova_dbg("%s alloc blocks failed %d\n", __func__,
								allocated);
			ret = allocated;
			goto out;
		}

		step++;
		bytes = sb->s_blocksize * allocated - offset;
		if (bytes > count)
			bytes = count;

		kmem = nova_get_block(inode->i_sb,
			nova_get_block_off(sb, blocknr,	pi->i_blk_type));

		if (offset || ((offset + bytes) & (PAGE_SIZE - 1)) != 0)
			nova_handle_head_tail_blocks(sb, pi, inode, pos, bytes,
								kmem);

		/* Now copy from user buf */
//		nova_dbg("Write: %p\n", kmem);
		NOVA_START_TIMING(memcpy_w_nvmm_t, memcpy_time);
		copied = bytes - memcpy_to_pmem_nocache(kmem + offset,
						buf, bytes);
		NOVA_END_TIMING(memcpy_w_nvmm_t, memcpy_time);

		entry_data.entry_type = FILE_WRITE;
		entry_data.reassigned = 0;
		entry_data.trans_id = trans_id;
		entry_data.pgoff = cpu_to_le64(start_blk);
		entry_data.num_pages = cpu_to_le32(allocated);
		entry_data.invalid_pages = 0;
		entry_data.block = cpu_to_le64(nova_get_block_off(sb, blocknr,
							pi->i_blk_type));
		entry_data.mtime = cpu_to_le32(time);

		if (pos + copied > inode->i_size)
			entry_data.size = cpu_to_le64(pos + copied);
		else
			entry_data.size = cpu_to_le64(inode->i_size);

		curr_entry = nova_append_file_write_entry(sb, pi, inode,
							&entry_data, temp_tail);
		if (curr_entry == 0) {
			nova_dbg("%s: append inode entry failed\n", __func__);
			ret = -ENOSPC;
			goto out;
		}


		if ( (copied > 0) && (NOVA_SB(sb)->block_csum_base) ) {
			csummed = copied - nova_update_cow_csum(inode, blocknr,
						(void *) buf, offset, copied);
			if (unlikely(csummed != copied)) {
				nova_dbg("%s: not all data bytes are "
					"checksummed! copied %zu, "
					"csummed %zu\n", __func__,
					copied, csummed);
			}
		}

		nova_dbgv("Write: %p, %lu\n", kmem, copied);
		if (copied > 0) {
			status = copied;
			written += copied;
			pos += copied;
			buf += copied;
			count -= copied;
			num_blocks -= allocated;
		}
		if (unlikely(copied != bytes)) {
			nova_dbg("%s ERROR!: %p, bytes %lu, copied %lu\n",
				__func__, kmem, bytes, copied);
			if (status >= 0)
				status = -EFAULT;
		}
		if (status < 0)
			break;

		if (begin_tail == 0)
			begin_tail = curr_entry;
		temp_tail = curr_entry + sizeof(struct nova_file_write_entry);
	}

	nova_memunlock_inode(sb, pi);
	data_bits = blk_type_to_shift[pi->i_blk_type];
	sih->i_blocks += (total_blocks << (data_bits - sb->s_blocksize_bits));
	nova_memlock_inode(sb, pi);

	nova_update_tail(pi, temp_tail);

	/* Free the overlap blocks after the write is committed */
	ret = nova_reassign_file_tree(sb, pi, sih, begin_tail);
	if (ret)
		goto out;

	inode->i_blocks = sih->i_blocks;

	ret = written;
	NOVA_STATS_ADD(write_breaks, step);
	nova_dbgv("blocks: %lu, %lu\n", inode->i_blocks, sih->i_blocks);

	*ppos = pos;
	if (pos > inode->i_size) {
		i_size_write(inode, pos);
		sih->i_size = pos;
	}
	nova_update_inode_checksum(pi);
	nova_update_alter_inode(sb, inode, pi);

out:
	if (ret < 0)
		nova_cleanup_incomplete_write(sb, pi, sih, blocknr, allocated,
						begin_tail, temp_tail);

	if (need_mutex)
		mutex_unlock(&inode->i_mutex);
	sb_end_write(inode->i_sb);
	NOVA_END_TIMING(cow_write_t, cow_write_time);
	NOVA_STATS_ADD(cow_write_bytes, written);
	return ret;
}

ssize_t nova_dax_file_write(struct file *filp, const char __user *buf,
	size_t len, loff_t *ppos)
{
	return nova_cow_file_write(filp, buf, len, ppos, true);
}

/*
 * return > 0, # of blocks mapped or allocated.
 * return = 0, if plain lookup failed.
 * return < 0, error case.
 */
static int nova_dax_get_blocks(struct inode *inode, sector_t iblock,
	unsigned long max_blocks, struct buffer_head *bh, int create)
{
	struct super_block *sb = inode->i_sb;
	struct nova_inode *pi;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_file_write_entry *entry = NULL;
	struct nova_file_write_entry entry_data;
	u64 temp_tail = 0;
	u64 curr_entry;
	u32 time;
	unsigned int data_bits;
	unsigned long nvmm = 0;
	unsigned long next_pgoff;
	unsigned long blocknr = 0;
	u64 trans_id;
	int num_blocks = 0;
	int allocated = 0;
	int ret = 0;

	if (max_blocks == 0)
		return 0;

	nova_dbgv("%s: pgoff %lu, num %lu, create %d\n",
				__func__, iblock, max_blocks, create);

	entry = nova_get_write_entry(sb, si, iblock);
	if (entry) {
		/* Find contiguous blocks */
		if (entry->invalid_pages == 0)
			num_blocks = entry->num_pages - (iblock - entry->pgoff);
		else
			num_blocks = 1;

		if (num_blocks > max_blocks)
			num_blocks = max_blocks;

		nvmm = get_nvmm(sb, sih, entry, iblock);
		clear_buffer_new(bh);
		nova_dbgv("%s: pgoff %lu, block %lu\n", __func__, iblock, nvmm);
		goto out;
	}

	if (create == 0)
		return 0;

	pi = nova_get_inode(sb, inode);
	num_blocks = max_blocks;
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	time = CURRENT_TIME_SEC.tv_sec;
	trans_id = nova_get_trans_id(sb);

	/* Fill the hole */
	entry = nova_find_next_entry(sb, sih, iblock);
	if (entry) {
		next_pgoff = entry->pgoff;
		if (next_pgoff <= iblock) {
			BUG();
			ret = -EINVAL;
			goto out;
		}

		num_blocks = next_pgoff - iblock;
		if (num_blocks > max_blocks)
			num_blocks = max_blocks;
	}

	/* Return initialized blocks to the user */
	allocated = nova_new_data_blocks(sb, pi, &blocknr, num_blocks,
						iblock, 1, 1);
	if (allocated <= 0) {
		nova_dbg("%s alloc blocks failed %d\n", __func__,
							allocated);
		ret = allocated;
		goto out;
	}

	num_blocks = allocated;
	entry_data.entry_type = FILE_WRITE;
	entry_data.reassigned = 0;
	entry_data.trans_id = trans_id;
	entry_data.pgoff = cpu_to_le64(iblock);
	entry_data.num_pages = cpu_to_le32(num_blocks);
	entry_data.invalid_pages = 0;
	entry_data.block = cpu_to_le64(nova_get_block_off(sb, blocknr,
							pi->i_blk_type));
	entry_data.mtime = cpu_to_le32(time);

	/* Do not extend file size */
	entry_data.size = cpu_to_le64(inode->i_size);

	curr_entry = nova_append_file_write_entry(sb, pi, inode,
						&entry_data, pi->log_tail);
	if (curr_entry == 0) {
		nova_dbg("%s: append inode entry failed\n", __func__);
		ret = -ENOSPC;
		goto out;
	}

	nvmm = blocknr;
	data_bits = blk_type_to_shift[pi->i_blk_type];
	sih->i_blocks += (num_blocks << (data_bits - sb->s_blocksize_bits));

	temp_tail = curr_entry + sizeof(struct nova_file_write_entry);
	nova_update_tail(pi, temp_tail);

	ret = nova_reassign_file_tree(sb, pi, sih, curr_entry);
	if (ret)
		goto out;

	inode->i_blocks = sih->i_blocks;

//	set_buffer_new(bh);
	nova_update_inode_checksum(pi);
	nova_update_alter_inode(sb, inode, pi);

out:
	if (ret < 0) {
		nova_cleanup_incomplete_write(sb, pi, sih, blocknr, allocated,
						0, temp_tail);
		return ret;
	}

	map_bh(bh, inode->i_sb, nvmm);
	if (num_blocks > 1)
		bh->b_size = sb->s_blocksize * num_blocks;

	return num_blocks;
}

int nova_dax_get_block(struct inode *inode, sector_t iblock,
	struct buffer_head *bh, int create)
{
	unsigned long max_blocks = bh->b_size >> inode->i_blkbits;
	int ret;
	timing_t gb_time;

	NOVA_START_TIMING(dax_get_block_t, gb_time);

	ret = nova_dax_get_blocks(inode, iblock, max_blocks, bh, create);
	if (ret > 0) {
		bh->b_size = ret << inode->i_blkbits;
		ret = 0;
	}
	NOVA_END_TIMING(dax_get_block_t, gb_time);
	return ret;
}

#if 0
static ssize_t nova_flush_mmap_to_nvmm(struct super_block *sb,
	struct inode *inode, struct nova_inode *pi, loff_t pos,
	size_t count, void *kmem, unsigned long blocknr)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	unsigned long start_blk;
	unsigned long cache_addr;
	u64 nvmm_block;
	void *nvmm_addr;
	loff_t offset;
	size_t bytes, copied, csummed;
	ssize_t written = 0;
	int status = 0;
	ssize_t ret;

	while (count) {
		start_blk = pos >> sb->s_blocksize_bits;
		offset = pos & (sb->s_blocksize - 1);
		bytes = sb->s_blocksize - offset;
		if (bytes > count)
			bytes = count;

		cache_addr = nova_get_cache_addr(sb, si, start_blk);
		if (cache_addr == 0) {
			nova_dbg("%s: ino %lu %lu mmap page %lu not found!\n",
					__func__, inode->i_ino, sih->ino, start_blk);
			nova_dbg("mmap pages %lu\n", sih->mmap_pages);
			ret = -EINVAL;
			goto out;
		}

		nvmm_block = MMAP_ADDR(cache_addr);
		nvmm_addr = nova_get_block(sb, nvmm_block);
		copied = bytes - memcpy_to_pmem_nocache(kmem + offset,
				nvmm_addr + offset, bytes);

		if ( (copied > 0) && (NOVA_SB(sb)->block_csum_base) ) {
			csummed = copied - nova_update_cow_csum(inode,
				blocknr, nvmm_addr + offset, offset, copied);
			if (unlikely(csummed != copied)) {
				nova_dbg("%s: not all data bytes are "
					"checksummed! copied %zu, "
					"csummed %zu\n", __func__,
					copied, csummed);
			}
		}

		if (copied > 0) {
			status = copied;
			written += copied;
			pos += copied;
			count -= copied;
			blocknr += (offset + copied) >> sb->s_blocksize_bits;
			kmem += offset + copied;
		}
		if (unlikely(copied != bytes)) {
			nova_dbg("%s ERROR!: %p, bytes %lu, copied %lu\n",
				__func__, kmem, bytes, copied);
			if (status >= 0)
				status = -EFAULT;
		}
		if (status < 0) {
			ret = status;
			goto out;
		}
	}
	ret = written;
out:
	return ret;
}

ssize_t nova_copy_to_nvmm(struct super_block *sb, struct inode *inode,
	struct nova_inode *pi, loff_t pos, size_t count, u64 *begin,
	u64 *end)
{
	struct nova_file_write_entry entry_data;
	unsigned long start_blk, num_blocks;
	unsigned long blocknr = 0;
	unsigned long total_blocks;
	unsigned int data_bits;
	int allocated = 0;
	u64 curr_entry;
	ssize_t written = 0;
	int ret;
	void *kmem;
	size_t bytes, copied;
	loff_t offset;
	int status = 0;
	u64 temp_tail = 0, begin_tail = 0;
	u64 trans_id;
	u32 time;
	timing_t memcpy_time, copy_to_nvmm_time;

	NOVA_START_TIMING(copy_to_nvmm_t, copy_to_nvmm_time);
	sb_start_write(inode->i_sb);

	offset = pos & (sb->s_blocksize - 1);
	num_blocks = ((count + offset - 1) >> sb->s_blocksize_bits) + 1;
	total_blocks = num_blocks;
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	time = CURRENT_TIME_SEC.tv_sec;

	nova_dbgv("%s: ino %lu, block %llu, offset %lu, count %lu\n",
		__func__, inode->i_ino, pos >> sb->s_blocksize_bits,
		(unsigned long)offset, count);

	trans_id = nova_get_trans_id(sb);
	temp_tail = *end;
	while (num_blocks > 0) {
		offset = pos & (nova_inode_blk_size(pi) - 1);
		start_blk = pos >> sb->s_blocksize_bits;
		allocated = nova_new_data_blocks(sb, pi, &blocknr, num_blocks,
						start_blk, 0, 0);
		if (allocated <= 0) {
			nova_dbg("%s alloc blocks failed %d\n", __func__,
								allocated);
			ret = allocated;
			goto out;
		}

		bytes = sb->s_blocksize * allocated - offset;
		if (bytes > count)
			bytes = count;

		kmem = nova_get_block(inode->i_sb,
			nova_get_block_off(sb, blocknr,	pi->i_blk_type));

		if (offset || ((offset + bytes) & (PAGE_SIZE - 1)))
			nova_handle_head_tail_blocks(sb, pi, inode, pos,
							bytes, kmem);

		NOVA_START_TIMING(memcpy_w_wb_t, memcpy_time);
		copied = nova_flush_mmap_to_nvmm(sb, inode, pi, pos, bytes,
							kmem, blocknr);
		NOVA_END_TIMING(memcpy_w_wb_t, memcpy_time);

		entry_data.entry_type = FILE_WRITE;
		entry_data.reassigned = 0;
		entry_data.trans_id = trans_id;
		entry_data.pgoff = cpu_to_le64(start_blk);
		entry_data.num_pages = cpu_to_le32(allocated);
		entry_data.invalid_pages = 0;
		entry_data.block = cpu_to_le64(nova_get_block_off(sb, blocknr,
							pi->i_blk_type));
		/* FIXME: should we use the page cache write time? */
		entry_data.mtime = cpu_to_le32(time);

		entry_data.size = cpu_to_le64(inode->i_size);

		curr_entry = nova_append_file_write_entry(sb, pi, inode,
						&entry_data, temp_tail);
		if (curr_entry == 0) {
			nova_dbg("%s: append inode entry failed\n", __func__);
			ret = -ENOSPC;
			goto out;
		}

		nova_dbgv("Write: %p, %ld\n", kmem, copied);
		if (copied > 0) {
			status = copied;
			written += copied;
			pos += copied;
			count -= copied;
			num_blocks -= allocated;
		}
		if (unlikely(copied != bytes)) {
			nova_dbg("%s ERROR!: %p, bytes %lu, copied %lu\n",
				__func__, kmem, bytes, copied);
			if (status >= 0)
				status = -EFAULT;
		}
		if (status < 0) {
			ret = status;
			goto out;
		}

		if (begin_tail == 0)
			begin_tail = curr_entry;
		temp_tail = curr_entry + sizeof(struct nova_file_write_entry);
	}

	nova_memunlock_inode(sb, pi);
	data_bits = blk_type_to_shift[pi->i_blk_type];
	sih->i_blocks += (total_blocks << (data_bits - sb->s_blocksize_bits));
	nova_memlock_inode(sb, pi);
	inode->i_blocks = sih->i_blocks;

	*begin = begin_tail;
	*end = temp_tail;

	ret = written;
out:
	if (ret < 0)
		nova_cleanup_incomplete_write(sb, pi, sih, blocknr, allocated,
						begin_tail, temp_tail);

	sb_end_write(inode->i_sb);
	NOVA_END_TIMING(copy_to_nvmm_t, copy_to_nvmm_time);
	return ret;
}

static int nova_get_nvmm_pfn(struct super_block *sb, struct nova_inode *pi,
	struct nova_inode_info *si, u64 nvmm, pgoff_t pgoff,
	vm_flags_t vm_flags, void **kmem, unsigned long *pfn)
{
	struct nova_inode_info_header *sih = &si->header;
	u64 mmap_block;
	unsigned long cache_addr = 0;
	unsigned long blocknr = 0;
	void *mmap_addr;
	void *nvmm_addr;
	int ret;

	cache_addr = nova_get_cache_addr(sb, si, pgoff);

	if (cache_addr) {
		mmap_block = MMAP_ADDR(cache_addr);
		mmap_addr = nova_get_block(sb, mmap_block);
	} else {
		ret = nova_new_data_blocks(sb, pi, &blocknr, 1,
						pgoff, 0, 1);

		if (ret <= 0) {
			nova_dbg("%s alloc blocks failed %d\n",
					__func__, ret);
			return ret;
		}

		mmap_block = blocknr << PAGE_SHIFT;
		mmap_addr = nova_get_block(sb, mmap_block);

		if (vm_flags & VM_WRITE)
			mmap_block |= MMAP_WRITE_BIT;

		nova_dbgv("%s: inode %lu, pgoff %lu, mmap block 0x%llx\n",
			__func__, sih->ino, pgoff, mmap_block);

		ret = radix_tree_insert(&sih->cache_tree, pgoff,
					(void *)mmap_block);
		if (ret) {
			nova_dbg("%s: ERROR %d\n", __func__, ret);
			return ret;
		}

		sih->mmap_pages++;
		if (nvmm) {
			/* Copy from NVMM to dram */
			nvmm_addr = nova_get_block(sb, nvmm);
			memcpy(mmap_addr, nvmm_addr, PAGE_SIZE);
		} else {
			memset(mmap_addr, 0, PAGE_SIZE);
		}
	}

	*kmem = mmap_addr;
	*pfn = nova_get_pfn(sb, mmap_block);

	return 0;
}

static int nova_get_mmap_addr(struct inode *inode, struct vm_area_struct *vma,
	pgoff_t pgoff, int create, void **kmem, unsigned long *pfn)
{
	struct super_block *sb = inode->i_sb;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_inode *pi;
	u64 nvmm;
	vm_flags_t vm_flags = vma->vm_flags;
	int ret;

	pi = nova_get_inode(sb, inode);

	nvmm = nova_find_nvmm_block(sb, si, NULL, pgoff);

	ret = nova_get_nvmm_pfn(sb, pi, si, nvmm, pgoff, vm_flags,
						kmem, pfn);

	if (vm_flags & VM_WRITE) {
		if (pgoff < sih->low_dirty)
			sih->low_dirty = pgoff;
		if (pgoff > sih->high_dirty)
			sih->high_dirty = pgoff;
	}

	return ret;
}

/* OOM err return with dax file fault handlers doesn't mean anything.
 * It would just cause the OS to go an unnecessary killing spree !
 */
static int __nova_dax_file_fault(struct vm_area_struct *vma,
				  struct vm_fault *vmf)
{
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;
	pgoff_t size;
	void *dax_mem;
	unsigned long dax_pfn = 0;
	int err;
	int ret = VM_FAULT_SIGBUS;

	mutex_lock(&inode->i_mutex);
	size = (i_size_read(inode) + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (vmf->pgoff >= size) {
		nova_dbg("[%s:%d] pgoff >= size(SIGBUS). vm_start(0x%lx),"
			" vm_end(0x%lx), pgoff(0x%lx), VA(%lx), size 0x%lx\n",
			__func__, __LINE__, vma->vm_start, vma->vm_end,
			vmf->pgoff, (unsigned long)vmf->virtual_address, size);
		goto out;
	}

	err = nova_get_mmap_addr(inode, vma, vmf->pgoff, 1,
						&dax_mem, &dax_pfn);
	if (unlikely(err)) {
		nova_dbg("[%s:%d] get_mmap_addr failed. vm_start(0x%lx),"
			" vm_end(0x%lx), pgoff(0x%lx), VA(%lx)\n",
			__func__, __LINE__, vma->vm_start, vma->vm_end,
			vmf->pgoff, (unsigned long)vmf->virtual_address);
		goto out;
	}

	nova_dbgv("%s flags: vma 0x%lx, vmf 0x%x\n",
			__func__, vma->vm_flags, vmf->flags);

	nova_dbgv("DAX mmap: inode %lu, vm_start(0x%lx), vm_end(0x%lx), "
			"pgoff(0x%lx), vma pgoff(0x%lx), "
			"VA(0x%lx)->PA(0x%lx)\n",
			inode->i_ino, vma->vm_start, vma->vm_end, vmf->pgoff,
			vma->vm_pgoff, (unsigned long)vmf->virtual_address,
			(unsigned long)dax_pfn << PAGE_SHIFT);

	if (dax_pfn == 0)
		goto out;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
	err = vm_insert_mixed(vma, (unsigned long)vmf->virtual_address,
		__pfn_to_pfn_t(dax_pfn, PFN_DEV));
#else
	err = vm_insert_mixed(vma, (unsigned long)vmf->virtual_address, dax_pfn);
#endif

	if (err == -ENOMEM)
		goto out;
	/*
	 * err == -EBUSY is fine, we've raced against another thread
	 * that faulted-in the same page
	 */
	if (err != -EBUSY)
		BUG_ON(err);

	ret = VM_FAULT_NOPAGE;

out:
	mutex_unlock(&inode->i_mutex);
	return ret;
}

static int nova_dax_file_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int ret = 0;
	timing_t fault_time;

	NOVA_START_TIMING(mmap_fault_t, fault_time);
	ret = __nova_dax_file_fault(vma, vmf);
	NOVA_END_TIMING(mmap_fault_t, fault_time);
	return ret;
}
#endif

static int nova_dax_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vma->vm_file);
	int ret = 0;
	timing_t fault_time;

	NOVA_START_TIMING(mmap_fault_t, fault_time);

	mutex_lock(&inode->i_mutex);
	ret = dax_fault(vma, vmf, nova_dax_get_block, NULL);
	mutex_unlock(&inode->i_mutex);

	NOVA_END_TIMING(mmap_fault_t, fault_time);
	return ret;
}

static int nova_dax_pmd_fault(struct vm_area_struct *vma, unsigned long addr,
	pmd_t *pmd, unsigned int flags)
{
	struct inode *inode = file_inode(vma->vm_file);
	int ret = 0;
	timing_t fault_time;

	NOVA_START_TIMING(mmap_fault_t, fault_time);

	mutex_lock(&inode->i_mutex);
	ret = dax_pmd_fault(vma, addr, pmd, flags, nova_dax_get_block, NULL);
	mutex_unlock(&inode->i_mutex);

	NOVA_END_TIMING(mmap_fault_t, fault_time);
	return ret;
}

static int nova_dax_pfn_mkwrite(struct vm_area_struct *vma,
	struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vma->vm_file);
	loff_t size;
	int ret = 0;
	timing_t fault_time;

	NOVA_START_TIMING(mmap_fault_t, fault_time);

	mutex_lock(&inode->i_mutex);
	size = (i_size_read(inode) + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (vmf->pgoff >= size)
		ret = VM_FAULT_SIGBUS;
	else
		ret = dax_pfn_mkwrite(vma, vmf);
	mutex_unlock(&inode->i_mutex);

	NOVA_END_TIMING(mmap_fault_t, fault_time);
	return ret;
}

static const struct vm_operations_struct nova_dax_vm_ops = {
	.fault	= nova_dax_fault,
	.pmd_fault = nova_dax_pmd_fault,
	.page_mkwrite = nova_dax_fault,
	.pfn_mkwrite = nova_dax_pfn_mkwrite,
};

int nova_dax_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	file_accessed(file);

	vma->vm_flags |= VM_MIXEDMAP | VM_HUGEPAGE;

	vma->vm_ops = &nova_dax_vm_ops;
	nova_dbg_mmap4k("[%s:%d] MMAP 4KPAGE vm_start(0x%lx),"
			" vm_end(0x%lx), vm_flags(0x%lx), "
			"vm_page_prot(0x%lx)\n", __func__,
			__LINE__, vma->vm_start, vma->vm_end,
			vma->vm_flags, pgprot_val(vma->vm_page_prot));

	return 0;
}

/* Calculate the data checksum. */
u32 nova_calc_data_csum(u32 init, void *buf, unsigned long size)
{
	u32 csum;

	/* TODO: Check if the function uses accelerated instructions for CRC. */
	csum = crc32c(init, buf, size);

	return csum;
}

/* Update copy-on-write data checksums.
 *
 * This function works on a sequence of contiguous data blocks that are just
 * created and the write buffer 'wrbuf' that causes this write transaction. The
 * data of 'wrbuf', and possible partial head and tail blocks are already copied
 * to NVMM data blocks.
 *
 * Logically the write buffer is in DRAM and it's checksummed before written to
 * NVMM, but if necessary 'wrbuf' can point to NVMM as well. Partial head and
 * and tail blocks are read from NVMM.
 *
 * Checksum is calculated over a whole block.
 *
 * blocknr: the physical block# of the first data block
 * wrbuf:   write buffer used to create the data blocks
 * offset:  byte offset of 'wrbuf' relative to the start the first block
 * bytes:   #bytes of 'wrbuf' written to the data blocks
 *
 * return: #bytes NOT checksummed (0 means a good exit)
 *
 * */
size_t nova_update_cow_csum(struct inode *inode, unsigned long blocknr,
		void *wrbuf, size_t offset, size_t bytes)
{
	struct super_block *sb = inode->i_sb;
	struct nova_inode  *pi = nova_get_inode(sb, inode);

	void *blockptr, *bufptr, *csum_addr;
	size_t blocksize = nova_inode_blk_size(pi);
	u32 csum;
	size_t csummed = 0;

	bufptr   = wrbuf;
	blockptr = nova_get_block(sb,
			nova_get_block_off(sb, blocknr,	pi->i_blk_type));

	/* in case file write entry is given instead of blocknr:
	 * blocknr  = get_nvmm(sb, sih, entry, entry->pgoff);
	 * blockptr = nova_get_block(sb, entry->block);
	 */

	if (offset) { // partial head block
		csum = nova_calc_data_csum(NOVA_INIT_CSUM, blockptr, offset);
		csummed = (blocksize - offset) < bytes ?
				blocksize - offset : bytes;
		csum = nova_calc_data_csum(csum, bufptr, csummed);

		if (offset + csummed < blocksize)
			csum = nova_calc_data_csum(csum,
						blockptr + offset + csummed,
						blocksize - offset - csummed);

		csum      = cpu_to_le32(csum);
		csum_addr = nova_get_block_csum_addr(sb, blocknr);
		memcpy_to_pmem_nocache(csum_addr, &csum, NOVA_DATA_CSUM_LEN);

		blocknr  += 1;
		bufptr   += csummed;
		blockptr += blocksize;
	}

	if (csummed < bytes) {
		while (csummed + blocksize < bytes) {
			csum = cpu_to_le32(nova_calc_data_csum(NOVA_INIT_CSUM,
						bufptr, blocksize));
			csum_addr = nova_get_block_csum_addr(sb, blocknr);
			memcpy_to_pmem_nocache(csum_addr, &csum,
						NOVA_DATA_CSUM_LEN);

			blocknr  += 1;
			bufptr   += blocksize;
			blockptr += blocksize;
			csummed  += blocksize;
		}

		if (csummed < bytes) { // partial tail block
			csum = nova_calc_data_csum(NOVA_INIT_CSUM, bufptr,
							bytes - csummed);
			csum = nova_calc_data_csum(csum,
						blockptr + bytes - csummed,
						blocksize - (bytes - csummed));

			csum      = cpu_to_le32(csum);
			csum_addr = nova_get_block_csum_addr(sb, blocknr);
			memcpy_to_pmem_nocache(csum_addr, &csum,
						NOVA_DATA_CSUM_LEN);

			csummed = bytes;
		}
	}

	return (bytes - csummed);
}

/* Verify checksums of requested data blocks of a file write entry.
 *
 * This function works on an existing file write 'entry' with its data in NVMM.
 *
 * Only a whole block can be checksum verified.
 *
 * index:  start block index of the file where data will be verified
 * blocks: #blocks to be verified starting from index
 *
 * return: true or false
 *
 * */
bool nova_verify_data_csum(struct inode *inode,
		struct nova_file_write_entry *entry, pgoff_t index,
		unsigned long blocks)
{
	struct super_block            *sb  = inode->i_sb;
	struct nova_inode             *pi  = nova_get_inode(sb, inode);
	struct nova_inode_info        *si  = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;

	void *blockptr;
	size_t blocksize = nova_inode_blk_size(pi);
	unsigned long block, blocknr;
	u32 csum_calc, csum_nvmm, *csum_addr;
	bool match;

	blocknr  = get_nvmm(sb, sih, entry, index);
	blockptr = nova_get_block(sb,
			nova_get_block_off(sb, blocknr,	pi->i_blk_type));

	match = true;
	for (block = 0; block < blocks; block++) {
		csum_calc = nova_calc_data_csum(NOVA_INIT_CSUM,
						blockptr, blocksize);
		csum_addr = nova_get_block_csum_addr(sb, blocknr);
		csum_nvmm = le32_to_cpu(*csum_addr);
		match     = (csum_calc == csum_nvmm);

		if (!match) {
			nova_dbg("%s: nova data block checksum fail! "
				"inode %lu block index %lu "
				"csum calc 0x%08x csum nvmm 0x%08x\n",
				__func__, inode->i_ino, index + block,
				csum_calc, csum_nvmm);
			break;
		}

		blocknr  += 1;
		blockptr += blocksize;
	}

	return match;
}
