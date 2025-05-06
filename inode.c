#include <stdio.h>
#include <assert.h>
#include <stdlib.h> // For malloc, free
#include <string.h> // For memcpy
#include "inode.h"
#include "diskimg.h"
#include "unixfilesystem.h" // Provides INODE_START_SECTOR, struct filsys, etc.
#include "ino.h"            // Provides struct inode, IALLOC, ILARG, etc.

// Calculate the number of inodes that can fit in one disk block.
#define INODES_PER_BLOCK (DISKIMG_SECTOR_SIZE / sizeof(struct inode))
// Calculate the number of block addresses that can fit in one disk block.
#define ADDRESSES_PER_BLOCK (DISKIMG_SECTOR_SIZE / sizeof(uint16_t))

/**
 * Fetches the specified inode from the filesystem.
 * Returns 0 on success, -1 on error.
 */
int inode_iget(struct unixfilesystem *fs, int inumber, struct inode *inp) {
    if (inumber < ROOT_INUMBER) { // inumber is 1-indexed, ROOT_INUMBER is 1 [cite: 82, 95]
        fprintf(stderr, "Error: Invalid inumber %d (must be >= %d)\n", inumber, ROOT_INUMBER);
        return -1;
    }

    // Calculate total number of inodes possible with s_isize blocks
    // s_isize is the size in blocks of the I-list [cite: 62]
    int max_inumber = fs->superblock.s_isize * INODES_PER_BLOCK;
    if (inumber > max_inumber) {
        fprintf(stderr, "Error: Invalid inumber %d (max is %d)\n", inumber, max_inumber);
        return -1;
    }

    // Calculate the block number on disk that contains this inode
    // Inodes start at INODE_START_SECTOR [cite: 66]
    // inumber is 1-indexed, so subtract 1 for 0-indexed calculations
    int inode_block_offset = (inumber - 1) / INODES_PER_BLOCK;
    int disk_block_num = INODE_START_SECTOR + inode_block_offset;

    // Calculate the offset of the inode within that block
    int offset_in_block_bytes = ((inumber - 1) % INODES_PER_BLOCK) * sizeof(struct inode);

    // Buffer to read the disk block
    unsigned char block_buffer[DISKIMG_SECTOR_SIZE];

    // Read the block from disk
    if (diskimg_readsector(fs->dfd, disk_block_num, block_buffer) != DISKIMG_SECTOR_SIZE) {
        fprintf(stderr, "Error: Failed to read inode block %d for inumber %d\n", disk_block_num, inumber);
        return -1;
    }

    // Copy the inode data from the buffer to the output struct
    memcpy(inp, block_buffer + offset_in_block_bytes, sizeof(struct inode));

    // Check if the inode is allocated [cite: 87, 88]
    if ((inp->i_mode & IALLOC) == 0) {
        // This might not strictly be an error for all use cases,
        // but for typical "get me this file's inode", it implies it doesn't exist or is free.
        // Depending on expected behavior, this could return an error or a specific status.
        // For now, let's consider an unallocated inode as an error for iget.
        fprintf(stderr, "Error: Inode %d is not allocated (i_mode: %04x)\n", inumber, inp->i_mode);
        return -1;
    }

    return 0; // Success
}

/**
 * Given an index of a file block (logical block number within the file),
 * retrieves the file's actual disk block number from the given inode.
 *
 * Returns the disk block number on success, -1 on error.
 */
int inode_indexlookup(struct unixfilesystem *fs, struct inode *inp, int fileBlockNum) {
    if (fileBlockNum < 0) {
        fprintf(stderr, "Error: fileBlockNum %d cannot be negative.\n", fileBlockNum);
        return -1;
    }

    int file_size_bytes = inode_getsize(inp);
    if (file_size_bytes == 0 && fileBlockNum == 0) { // Empty file special case
        return -1; // No blocks for an empty file
    }
    // Allowing fileBlockNum == 0 for a zero-byte file could be valid if we were about to write to it,
    // but for lookup, it means no such block exists.

    // Maximum logical block number (0-indexed)
    // (size + blocksize -1 ) / blocksize effectively gives ceil(size/blocksize)
    int max_logical_block = (file_size_bytes + DISKIMG_SECTOR_SIZE - 1) / DISKIMG_SECTOR_SIZE;
    if (file_size_bytes > 0 && fileBlockNum >= max_logical_block) {
         // Requesting a block beyond the file's content
        // fprintf(stderr, "Error: fileBlockNum %d is out of bounds for file size %d bytes (max logical block %d).\n", fileBlockNum, file_size_bytes, max_logical_block -1);
        return -1; // Standard Unix behavior: Holes are zeroes, past EOF is error.
    }
    // If file_size_bytes is 0, max_logical_block will be 0.
    // If fileBlockNum is also 0, this condition is fileBlockNum >= 0, which isn't right for an empty file.
    // Handled by the file_size_bytes == 0 && fileBlockNum == 0 check above.

    unsigned char block_buffer[DISKIMG_SECTOR_SIZE]; // For reading indirect blocks
    uint16_t data_block_num;

    if ((inp->i_mode & ILARG) == 0) { // Small file [cite: 81, 90]
        // Direct blocks only. i_addr contains up to 8 direct block numbers.
        if (fileBlockNum >= (sizeof(inp->i_addr) / sizeof(inp->i_addr[0]))) {
            fprintf(stderr, "Error (Small File): fileBlockNum %d is out of direct block range.\n", fileBlockNum);
            return -1;
        }
        data_block_num = inp->i_addr[fileBlockNum];
        if (data_block_num == 0) {
            // This block is not allocated (hole in file or past EOF for allocated size but not written)
            // The problem asks for "disk block number on success, -1 on error".
            // A non-existent block within the file's theoretical span could be an error.
            return -1;
        }
        return data_block_num;

    } else { // Large file [cite: 81, 90]
        // i_addr[0]...i_addr[6] are single indirect, i_addr[7] is double indirect.

        int single_indirect_coverage = 7 * ADDRESSES_PER_BLOCK;

        if (fileBlockNum < single_indirect_coverage) { // Falls into one of the 7 single indirect blocks
            int indirect_block_index_in_i_addr = fileBlockNum / ADDRESSES_PER_BLOCK; // Which of i_addr[0]..[6]
            int offset_in_indirect_block = fileBlockNum % ADDRESSES_PER_BLOCK;

            uint16_t single_indirect_ptr = inp->i_addr[indirect_block_index_in_i_addr];
            if (single_indirect_ptr == 0) { // Single indirect block itself is not allocated
                return -1;
            }

            if (diskimg_readsector(fs->dfd, single_indirect_ptr, block_buffer) != DISKIMG_SECTOR_SIZE) {
                fprintf(stderr, "Error: Failed to read single indirect block %d\n", single_indirect_ptr);
                return -1;
            }
            
            data_block_num = ((uint16_t *)block_buffer)[offset_in_indirect_block];
            if (data_block_num == 0) { // Data block pointed to by indirect block is not allocated
                 return -1;
            }
            return data_block_num;

        } else { // Falls into the double indirect block (i_addr[7])
            uint16_t double_indirect_ptr = inp->i_addr[7];
            if (double_indirect_ptr == 0) { // Double indirect block itself is not allocated
                return -1;
            }

            if (diskimg_readsector(fs->dfd, double_indirect_ptr, block_buffer) != DISKIMG_SECTOR_SIZE) {
                fprintf(stderr, "Error: Failed to read double indirect block %d\n", double_indirect_ptr);
                return -1;
            }

            // Adjust fileBlockNum relative to the start of the double indirect region
            int block_num_in_double_region = fileBlockNum - single_indirect_coverage;
            
            int first_level_index = block_num_in_double_region / ADDRESSES_PER_BLOCK;
            if (first_level_index >= ADDRESSES_PER_BLOCK) { // Index out of bounds for the first level of indirection
                 fprintf(stderr, "Error (Large File - DI): first_level_index %d is out of bounds.\n", first_level_index);
                return -1;
            }

            uint16_t target_single_indirect_ptr = ((uint16_t *)block_buffer)[first_level_index];
            if (target_single_indirect_ptr == 0) { // Target single indirect block is not allocated
                return -1;
            }

            // Now read the target single indirect block
            // Can reuse block_buffer
            if (diskimg_readsector(fs->dfd, target_single_indirect_ptr, block_buffer) != DISKIMG_SECTOR_SIZE) {
                fprintf(stderr, "Error: Failed to read target single indirect block %d from double indirect path\n", target_single_indirect_ptr);
                return -1;
            }

            int second_level_index = block_num_in_double_region % ADDRESSES_PER_BLOCK;
            // No need to check second_level_index bounds as it's derived from % ADDRESSES_PER_BLOCK

            data_block_num = ((uint16_t *)block_buffer)[second_level_index];
            if (data_block_num == 0) { // Final data block is not allocated
                return -1;
            }
            return data_block_num;
        }
    }
    // Should not be reached if logic is correct
    return -1; 
}

/**
 * Computes the size in bytes of the file identified by the given inode
 */
int inode_getsize(struct inode *inp) {
  // This function is already provided in the skeleton
  return ((inp->i_size0 << 16) | inp->i_size1); 
}
