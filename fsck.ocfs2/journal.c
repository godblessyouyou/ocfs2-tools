/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * Copyright (C) 2004 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License, version 2,  as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 *
 * --
 * This replays the jbd journals for each node.  First all the journals are
 * walked to detect inconsistencies.  Only journals with no problems will be
 * replayed.  IO errors during replay will just result in partial journal
 * replay, just like jbd does in the kernel.  Journals that don't pass
 * consistency checks, like having overlapping blocks or strange fields, are
 * ignored and left for later passes to clean up.  Overlap testing is done
 * using o2fsck_state's used block bitmap.  

 * XXX
 * 	future passes need to guarantee journals exist and are the same size 
 * 	pass fsck trigger back up, write dirty fs, always zap/write
 * 	revocation code is totally untested
 * 	some setup errors, like finding the dlm system inode, are fatal
 */

#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "fsck.h"
#include "journal.h"
/* jfs_compat.h defines these */
#undef cpu_to_be32
#undef be32_to_cpu
#include "jfs_user.h"
#include "ocfs2.h"
#include "ocfs2_disk_dlm.h"
#include "pass1.h"
#include "problem.h"
#include "util.h"

static char *whoami = "journal recovery";

struct journal_info {
	int			ji_node;
	unsigned		ji_replay:1;

	uint64_t		ji_ino;
	struct rb_root		ji_revoke;
	journal_superblock_t	*ji_jsb;
	uint64_t		ji_jsb_block;
	ocfs2_cached_inode	*ji_cinode;

	unsigned		ji_set_final_seq:1;
	uint32_t		ji_final_seq;

	/* we keep our own bitmap for detecting overlapping journal blocks */
	ocfs2_bitmap		*ji_used_blocks;
};

struct revoke_entry {
	struct rb_node	r_node;
	uint64_t	r_block;
	uint32_t	r_seq;
};

static int seq_gt(uint32_t x, uint32_t y)
{
	int32_t diff = x - y;
	return diff > 0;
}
static int seq_geq(uint32_t x, uint32_t y)
{
	int32_t diff = x - y;
	return diff >= 0;
}

static errcode_t revoke_insert(struct rb_root *root, uint64_t block,
				uint32_t seq)
{
	struct rb_node ** p = &root->rb_node;
	struct rb_node * parent = NULL;
	struct revoke_entry *re;

	while (*p)
	{
		parent = *p;
		re = rb_entry(parent, struct revoke_entry, r_node);

		if (block < re->r_block)
			p = &(*p)->rb_left;
		else if (block > re->r_block)
			p = &(*p)->rb_right;
		else {
			if (seq_gt(seq, re->r_seq))
				re->r_seq = seq;
			return 0;
		}
	}

	re = malloc(sizeof(struct revoke_entry));
	if (re == NULL)
		return OCFS2_ET_NO_MEMORY;

	re->r_block = block;
	re->r_seq = seq;

	rb_link_node(&re->r_node, parent, p);
	rb_insert_color(&re->r_node, root);

	return 0;
}

static int revoke_this_block(struct rb_root *root, uint64_t block, 
		 	     uint32_t seq)
{
	struct rb_node *node = root->rb_node;
	struct revoke_entry *re;

	while (node) {
		re = rb_entry(node, struct revoke_entry, r_node);

		if (block < re->r_block)
			node = node->rb_left;
		else if (block > re->r_block)
			node = node->rb_right;
		else {
			/* only revoke if we've recorded a revoke entry for
			 * this block that is <= the seq that we're interested
			 * in */
			if (re && !seq_gt(seq, re->r_seq)) {
				verbosef("%"PRIu64" is revoked\n", block);
				return 1;
			}
		}
	}

	return 0;
}

static void revoke_free_all(struct rb_root *root)
{
	struct revoke_entry *re;
	struct rb_node *node;

	while((node = rb_first(root)) != NULL) {
		re = rb_entry(node, struct revoke_entry, r_node);
		rb_erase(node, root);
		free(re);
	}
}

static errcode_t add_revoke_records(struct journal_info *ji, char *buf,
				    size_t max, uint32_t seq)
{
	journal_revoke_header_t jr;
	uint32_t *blkno;  /* XXX 640k ought to be enough for everybody */
	size_t i, num;
	errcode_t err = 0;

	memcpy(&jr, buf, sizeof(jr));
	jr.r_count = be32_to_cpu(jr.r_count);

	if (jr.r_count < sizeof(jr) || jr.r_count > max) {
		verbosef("corrupt r_count: %X", jr.r_count);
		return OCFS2_ET_BAD_JOURNAL_REVOKE;
	}

	num = (jr.r_count - sizeof(jr)) / sizeof(blkno);
	blkno = (uint32_t *)(buf + sizeof(jr));

	for (i = 0; i < num; i++, blkno++) {
		err = revoke_insert(&ji->ji_revoke, be32_to_cpu(*blkno), seq);
		if (err)
			break;
	}

	return err;
}

static uint64_t jwrap(journal_superblock_t *jsb, uint64_t block)
{
	uint64_t diff = jsb->s_maxlen - jsb->s_first;

	if (diff == 0) /* ugh */
		return 0;

	while (block > jsb->s_maxlen)
		block -= diff;

	return block;
}

static errcode_t count_tags(o2fsck_state *ost, char *buf, size_t size,
			    uint64_t *nr_ret)
{
	journal_block_tag_t *tag, *last;
	uint64_t nr = 0;

	if (size < sizeof(journal_header_t) + sizeof(*tag))
		return OCFS2_ET_BAD_JOURNAL_TAG;

       	tag = (journal_block_tag_t *)&buf[sizeof(journal_header_t)];
       	last = (journal_block_tag_t *)&buf[size - sizeof(*tag)];

	for(; tag <= last; tag++) {
		nr++;
		if (ocfs2_block_out_of_range(ost->ost_fs, 
					     be32_to_cpu(tag->t_blocknr)))
			return OCFS2_ET_BAD_JOURNAL_TAG;

		if (tag->t_flags & cpu_to_be32(JFS_FLAG_LAST_TAG))
			break;
		/* inline uuids are 16 bytes, tags are 8 */
		if (!(tag->t_flags & cpu_to_be32(JFS_FLAG_SAME_UUID)))
			tag += 2;
	}

	*nr_ret = nr;
	return 0;
}

static errcode_t lookup_journal_block(o2fsck_state *ost, 
				      struct journal_info *ji, 
				      uint64_t blkoff,
				      uint64_t *blkno,
				      int check_dup)
{
	errcode_t ret;
	int contig;
	int was_set;

	ret = ocfs2_extent_map_get_blocks(ji->ji_cinode, blkoff, 1, blkno,
					  &contig);
	if (ret) {
		com_err(whoami, ret, "while looking up logical block "
			"%"PRIu64" in node %d's journal", blkoff, ji->ji_node);
		goto out;
	}

	if (check_dup) {
		ocfs2_bitmap_set(ji->ji_used_blocks, *blkno, &was_set);
		if (was_set)  {
			printf("Logical block %"PRIu64" in node %d's journal "
			       "maps to block %"PRIu64" which has already "
			       "been used in another journal.\n", blkoff,
			       ji->ji_node, *blkno);
			ret = OCFS2_ET_DUPLICATE_BLOCK;
		}
	}

out:
	return ret;
}

static errcode_t read_journal_block(o2fsck_state *ost, 
				    struct journal_info *ji, 
				    uint64_t blkoff, 
				    char *buf,
				    int check_dup)
{
	errcode_t err;
	uint64_t	blkno;

	err = lookup_journal_block(ost, ji, blkoff, &blkno, check_dup);
	if (err)
		return err;

	err = io_read_block(ost->ost_fs->fs_io, blkno, 1, buf);
	if (err)
		com_err(whoami, err, "while reading block %"PRIu64" of node "
			"%d's journal", blkno, ji->ji_node);

	return err;
}

static errcode_t replay_blocks(o2fsck_state *ost, struct journal_info *ji,
			       char *buf, uint64_t seq, uint64_t *next_block)
{
	journal_block_tag_t tag, *tagp;
	size_t i, num;
	char *io_buf = NULL;
	errcode_t err, ret = 0;
		
	tagp = (journal_block_tag_t *)(buf + sizeof(journal_header_t));
	num = (ji->ji_jsb->s_blocksize - sizeof(journal_header_t)) / 
	      sizeof(tag);

	ret = ocfs2_malloc_blocks(ost->ost_fs->fs_io, 1, &io_buf);
	if (ret) {
		com_err(whoami, ret, "while allocating a block buffer");
		goto out;
	}

	for(i = 0; i < num; i++, tagp++, (*next_block)++) {
		memcpy(&tag, tagp, sizeof(tag));
		tag.t_flags = be32_to_cpu(tag.t_flags);
		tag.t_blocknr = be32_to_cpu(tag.t_blocknr);

		*next_block = jwrap(ji->ji_jsb, *next_block);

		verbosef("recovering journal block %"PRIu64" to disk block "
			 "%"PRIu32"\n", *next_block, tag.t_blocknr);	

		if (revoke_this_block(&ji->ji_revoke, tag.t_blocknr, seq))
			goto skip_io;

		err = read_journal_block(ost, ji, *next_block, io_buf, 1);
		if (err) {
			ret = err;
			goto skip_io;
		}

		if (tag.t_flags & JFS_FLAG_ESCAPE) {
			uint32_t magic = cpu_to_be32(JFS_MAGIC_NUMBER);
			memcpy(io_buf, &magic, sizeof(magic));
		}

		err = io_write_block(ost->ost_fs->fs_io, tag.t_blocknr, 1, 
				     io_buf);
		if (err)
			ret = err;

	skip_io:
		if (tag.t_flags & JFS_FLAG_LAST_TAG)
			i = num; /* be sure to increment next_block */
		/* inline uuids are 16 bytes, tags are 8 */
		if (!(tag.t_flags & JFS_FLAG_SAME_UUID))
			tagp += 2;
	}
	
out:
	if (io_buf)
		ocfs2_free(&io_buf);
	return ret;
}

static errcode_t walk_journal(o2fsck_state *ost, int node, 
			      struct journal_info *ji, char *buf, int recover)
{
	errcode_t err, ret = 0;
	uint32_t next_seq;
	uint64_t next_block, nr;
	journal_superblock_t *jsb = ji->ji_jsb;
	journal_header_t jh;

	next_seq = jsb->s_sequence;
	next_block = jsb->s_start;

	/* s_start == 0 when we have nothing to do */
	if (next_block == 0)
		return 0;

	/* ret is set when bad tags are seen in the first scan and when there
	 * are io errors in the recovery scan.  Only stop walking the journal
	 * when bad tags are seen in the first scan. */
	while(recover || !ret) {
		verbosef("next_seq %"PRIu32" final_seq %"PRIu32" next_block "
			 "%"PRIu64"\n", next_seq, ji->ji_final_seq,
			 next_block);

		if (recover && seq_geq(next_seq, ji->ji_final_seq))
			break;

		/* only mark the blocks used on the first pass */
		err = read_journal_block(ost, ji, next_block, buf, !recover);
		if (err) {
			ret = err;
			break;
		}

		next_block = jwrap(jsb, next_block + 1);

		memcpy(&jh, buf, sizeof(jh));
		jh.h_magic = be32_to_cpu(jh.h_magic);
		jh.h_blocktype = be32_to_cpu(jh.h_blocktype);
		jh.h_sequence = be32_to_cpu(jh.h_sequence);

		verbosef("jh magic %x\n", jh.h_magic);

		if (jh.h_magic != JFS_MAGIC_NUMBER)
			break;

		verbosef("jh block %x\n", jh.h_blocktype);
		verbosef("jh seq %"PRIu32"\n", jh.h_sequence);

		if (jh.h_sequence != next_seq)
			break;

		switch(jh.h_blocktype) {
		case JFS_DESCRIPTOR_BLOCK:
			verbosef("found a desc type %x\n", jh.h_blocktype);
			/* replay the blocks described in the desc block */
			if (recover) {
				err = replay_blocks(ost, ji, buf, next_seq, 
						    &next_block);
				if (err)
					ret = err;
				continue;
			}

			/* just record the blocks as used and carry on */ 
			err = count_tags(ost, buf, jsb->s_blocksize, &nr);
			if (err)
				ret = err;
			else
				next_block = jwrap(jsb, next_block + nr);
			break;

		case JFS_COMMIT_BLOCK:
			verbosef("found a commit type %x\n", jh.h_blocktype);
			next_seq++;
			break;

		case JFS_REVOKE_BLOCK:
			verbosef("found a revoke type %x\n", jh.h_blocktype);
			add_revoke_records(ji, buf, jsb->s_blocksize,
					   next_seq);
			break;

		default:
			verbosef("unknown type %x\n", jh.h_blocktype);
			break;
		}
	}

	verbosef("done scanning with seq %"PRIu32"\n", next_seq);

	if (!recover) {
		ji->ji_set_final_seq = 1;
		ji->ji_final_seq = next_seq;
	} else if (ji->ji_final_seq != next_seq) {
		printf("Replaying node %d's journal stopped at seq %"PRIu32" "
		       "but an initial scan indicated that it should have "
		       "stopped at seq %"PRIu32"\n", ji->ji_node, next_seq,
		       ji->ji_final_seq);
		if (ret == 0)
			err = OCFS2_ET_IO;
	}

	return ret;
}

static errcode_t prep_journal_info(o2fsck_state *ost, int node,
			           struct journal_info *ji)
{
	errcode_t err;

	ji->ji_revoke = RB_ROOT;
	ji->ji_node = node;

	err = ocfs2_malloc_blocks(ost->ost_fs->fs_io, 1, &ji->ji_jsb);
	if (err)
		com_err(whoami, err, "while allocating space for node %d's "
			    "journal superblock", node);

	err = ocfs2_lookup_system_inode(ost->ost_fs, JOURNAL_SYSTEM_INODE,
					node, &ji->ji_ino);
	if (err) {
		com_err(whoami, err, "while looking up the journal inode for "
			"node %d", node);
		goto out;
	}

	err = ocfs2_read_cached_inode(ost->ost_fs, ji->ji_ino, &ji->ji_cinode);
	if (err) {
		com_err(whoami, err, "while reading cached inode %"PRIu64" "
			"for node %d's journal", ji->ji_ino, node);
		goto out;
	}

	err = ocfs2_extent_map_init(ost->ost_fs, ji->ji_cinode);
	if (err) {
		com_err(whoami, err, "while initializing extent map");
		goto out;
	}

	err = lookup_journal_block(ost, ji, 0, &ji->ji_jsb_block, 1);
	if (err)
		goto out;

	/* XXX be smarter about reading in the whole super block if it
	 * spans multiple blocks */
	err = ocfs2_read_journal_superblock(ost->ost_fs, ji->ji_jsb_block, 
					    (char *)ji->ji_jsb);
	if (err) {
		com_err(whoami, err, "while reading block %"PRIu64" as node "
			"%d's journal super block", ji->ji_jsb_block,
			ji->ji_node);
		goto out;
	}

	verbosef("jsb start %u maxlen %u\n", ji->ji_jsb->s_start,
		 ji->ji_jsb->s_maxlen);
out:
	return err;
}

static int publish_mounted_set(ocfs2_filesys *fs, char *buf, int node, 
			       int max_nodes)
{
	int b_bits = OCFS2_RAW_SB(fs->fs_super)->s_blocksize_bits;
	/* magic! */
	ocfs_publish *pub = (ocfs_publish *)(buf + ((2 + 4 + max_nodes + node)
			       			<< b_bits));
	verbosef("node %d's publish: %u\n", node, le32_to_cpu(pub->mounted));

	return pub->mounted;
}

/* Try and replay the nodes journals if they're dirty.  This only returns
 * a non-zero error if the caller should not continue. */
errcode_t o2fsck_replay_journals(o2fsck_state *ost)
{
	errcode_t err = 0, ret = 0;
	struct journal_info *jis, *ji;
	journal_superblock_t *jsb;
	char *buf = NULL, *dlm_buf = NULL;
	int i, max_nodes, buflen, journal_trouble = 0;
	uint64_t dlm_ino;
	ocfs2_bitmap *used_blocks = NULL;

	max_nodes = OCFS2_RAW_SB(ost->ost_fs->fs_super)->s_max_nodes;

	ret = ocfs2_block_bitmap_new(ost->ost_fs, "journal blocks",
				     &used_blocks);
	if (ret) {
		com_err(whoami, ret, "while allocating journal block bitmap"); 
		goto out;
	}

	ret = ocfs2_malloc_blocks(ost->ost_fs->fs_io, 1, &buf);
	if (ret) {
		com_err(whoami, ret, "while allocating room to read journal "
			    "blocks");
		goto out;
	}

	ret = ocfs2_malloc0(sizeof(struct journal_info) * max_nodes, &jis);
	if (ret) {
		com_err(whoami, ret, "while allocating an array of block "
			 "numbers for journal replay");
		goto out;
	}

	ret = ocfs2_lookup_system_inode(ost->ost_fs, DLM_SYSTEM_INODE,
					0, &dlm_ino);
	if (ret) {
		com_err(whoami, ret, "while looking up the dlm system inode");
		goto out;
	}

	ret = ocfs2_read_whole_file(ost->ost_fs, dlm_ino, &dlm_buf, &buflen);
	if (ret) {
		com_err(whoami, ret, "while reading dlm file");
		goto out;
	}

	printf("Checking each node's journal.\n");

	for (i = 0, ji = jis; i < max_nodes; i++, ji++) {
		if (!publish_mounted_set(ost->ost_fs, dlm_buf, i, max_nodes)) {
			verbosef("node %d is clean\n", i);
			continue;
		}
		ji->ji_replay = 1;
		ji->ji_used_blocks = used_blocks;

		err = prep_journal_info(ost, i, ji);
		if (err == 0)
			err = walk_journal(ost, i, ji, buf, 0);

		if (err) {
			ji->ji_replay = 0;
			printf("Node %d's journal can not be replayed.\n", i);
			journal_trouble = 1;
		}
	}

	for (i = 0, ji = jis; i < max_nodes; i++, ji++) {
		if (!ji->ji_replay)
			continue;

		if (!prompt(ost, PY, "Node %d's journal needs to be replayed. "
			    "Do so?", i)) {
			journal_trouble = 1;
			continue;
		}

		err = walk_journal(ost, i, ji, buf, 1);
		if (err) {
			journal_trouble = 1;
			continue;
		} 

		jsb = ji->ji_jsb;
		/* reset the journal */
		jsb->s_start = 0;

		if (ji->ji_set_final_seq)
			jsb->s_sequence = ji->ji_final_seq + 1;

		/* we don't write back a clean 'mounted' bit here.  That would
		 * have to also include having recovered the orphan dir.  we
		 * updated s_start, though, so we won't replay the journal
		 * again. */
		err = ocfs2_write_journal_superblock(ost->ost_fs,
						     ji->ji_jsb_block,
						     (char *)ji->ji_jsb);
		if (err) {
			com_err(whoami, err, "while writing node %d's journal "
				"super block", i);
			journal_trouble = 1;
		}

		printf("Node %d's journal replayed successfully.\n", i);
	}

	/* this is awkward, but we want fsck -n to tell us as much as it
	 * can so we don't want to ask to proceed here.  */
	if (journal_trouble)
		printf("*** There were problems replaying journals.  Be "
		       "careful in telling fsck to make repairs to this "
		       "filesystem.\n");

	ret = 0;

out:
	if (jis) {
		for (i = 0, ji = jis; i < max_nodes; i++, ji++) {
			if (ji->ji_jsb)
				ocfs2_free(&ji->ji_jsb);
			if (ji->ji_cinode)
				ocfs2_free_cached_inode(ost->ost_fs, 
							ji->ji_cinode);
			revoke_free_all(&ji->ji_revoke);
		}
		ocfs2_free(&jis);
	}

	if (buf)
		ocfs2_free(&buf);
	if (dlm_buf)
		ocfs2_free(&dlm_buf);
	if (used_blocks)
		ocfs2_bitmap_free(used_blocks);

	return ret;
}
