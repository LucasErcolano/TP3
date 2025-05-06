#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "file.h"
#include "inode.h"
#include "diskimg.h"
#include "unixfilesystem.h"

/**
 * Fetches the specified file block from the specified inode.
 * Returns the number of valid bytes in the block, -1 on error.
 */
int file_getblock(struct unixfilesystem *fs, int inumber, int blockNum, void *buf) {
    struct inode inode_data;

    // 1. Fetch the Inode
    if (inode_iget(fs, inumber, &inode_data) < 0) {
        // inode_iget should print its own error
        return -1; // Error fetching inode
    }

    // 2. Get the File Size
    int file_size_bytes = inode_getsize(&inode_data);

    // 3. Handle empty file case
    if (file_size_bytes == 0) {
        // Any blockNum for an empty file results in 0 valid bytes.
        // No error, just no data.
        return 0;
    }

    // 4. Validate blockNum against File Size (for non-empty files)
    // Calculate total number of logical blocks in the file (0-indexed)
    int num_logical_blocks = (file_size_bytes + DISKIMG_SECTOR_SIZE - 1) / DISKIMG_SECTOR_SIZE;

    if (blockNum < 0 || blockNum >= num_logical_blocks) {
        fprintf(stderr, "Error: blockNum %d is out of bounds for file with %d blocks (size %d bytes).\n",
                blockNum, num_logical_blocks, file_size_bytes);
        return -1; // Requested block is out of the file's bounds
    }

    // 5. Find the Physical Disk Block Number
    int disk_sector_num = inode_indexlookup(fs, &inode_data, blockNum);
    if (disk_sector_num < 0) { // Handles errors from inode_indexlookup (e.g., -1)
        // inode_indexlookup should print its own error or the error is "block not found"
        // fprintf(stderr, "Error: Could not find disk sector for inumber %d, blockNum %d.\n", inumber, blockNum);
        return -1; // Error or block not allocated/found
    }
     if (disk_sector_num == 0) { // A disk sector number of 0 is invalid in Unix V6 for data
        fprintf(stderr, "Error: inode_indexlookup returned disk_sector_num 0 for inumber %d, blockNum %d.\n", inumber, blockNum);
        return -1;
    }


    // 6. Read the Disk Block
    if (diskimg_readsector(fs->dfd, disk_sector_num, buf) != DISKIMG_SECTOR_SIZE) {
        fprintf(stderr, "Error: Failed to read disk sector %d for inumber %d, blockNum %d.\n",
                disk_sector_num, inumber, blockNum);
        return -1; // Error reading block from disk
    }

    // 7. Determine Number of Valid Bytes
    // If this is the last block, it might not be full.
    // Otherwise, it's full (DISKIMG_SECTOR_SIZE bytes).
    
    // Calculate the byte offset of the start of the current logical block
    long start_byte_of_block = (long)blockNum * DISKIMG_SECTOR_SIZE;
    
    // Calculate how many bytes of the file are remaining from this block onwards
    int remaining_bytes_in_file_from_this_block = file_size_bytes - start_byte_of_block;

    if (remaining_bytes_in_file_from_this_block >= DISKIMG_SECTOR_SIZE) {
        return DISKIMG_SECTOR_SIZE; // Full block is valid
    } else {
        // This must be the last block, and it's partially filled.
        // remaining_bytes_in_file_from_this_block must be > 0 here because:
        // - file_size_bytes > 0 (checked at the beginning)
        // - blockNum < num_logical_blocks (checked)
        // This means we are not reading past EOF.
        return remaining_bytes_in_file_from_this_block;
    }
}

