#include "directory.h"
#include "inode.h"
#include "diskimg.h"
#include "file.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

// Helper macro for error returns, can be adapted if specific negative error codes are needed
#define DIRECTORY_FAILURE -1 

/**
 * Looks up the specified name (name) in the specified directory (dirinumber).
 * If found, return the directory entry in space addressed by dirEnt.  Returns 0
 * on success and something negative on failure.
 */
int directory_findname(struct unixfilesystem *fs, const char *name,
                       int dirinumber, struct direntv6 *dirEnt) {
    struct inode dir_inode;

    // Preliminary check: if the name to find is too long, it can't exist
    if (strlen(name) > sizeof(dirEnt->d_name)) {
        // fprintf(stderr, "Error: Search name '%s' is longer than max filename length (%lu).\n", name, sizeof(dirEnt->d_name));
        return DIRECTORY_FAILURE;
    }

    // 1. Get the Directory's Inode
    if (inode_iget(fs, dirinumber, &dir_inode) < 0) {
        // inode_iget prints its own error
        return DIRECTORY_FAILURE;
    }

    // 2. Verify it's a Directory
    if ((dir_inode.i_mode & IFMT) != IFDIR) {
        fprintf(stderr, "Error: Inode %d is not a directory (i_mode: %04x).\n", dirinumber, dir_inode.i_mode);
        return DIRECTORY_FAILURE;
    }

    // 3. Get Directory Size
    int dir_size_bytes = inode_getsize(&dir_inode);
    if (dir_size_bytes == 0) {
        // fprintf(stderr, "Directory inode %d is empty.\n", dirinumber);
        return DIRECTORY_FAILURE; // Empty directory, name cannot be found
    }

    // Directory size should be a multiple of directory entry size
    if (dir_size_bytes % sizeof(struct direntv6) != 0) {
        fprintf(stderr, "Error: Directory inode %d has corrupted size %d.\n", dirinumber, dir_size_bytes);
        return DIRECTORY_FAILURE;
    }

    // 4. Iterate Through Directory Entries
    unsigned char block_buffer[DISKIMG_SECTOR_SIZE];
    int total_bytes_processed = 0;
    int current_block_num = 0;

    while (total_bytes_processed < dir_size_bytes) {
        int valid_bytes_in_block = file_getblock(fs, dirinumber, current_block_num, block_buffer);

        if (valid_bytes_in_block < 0) {
            // file_getblock should print its own error
            // fprintf(stderr, "Error reading data block %d for directory inode %d.\n", current_block_num, dirinumber);
            return DIRECTORY_FAILURE; // Error reading directory data
        }

        if (valid_bytes_in_block == 0) {
            // Reached end of directory data unexpectedly (should be covered by total_bytes_processed < dir_size_bytes)
            break;
        }

        // Directory data in a block should also be a multiple of entry size
        if (valid_bytes_in_block % sizeof(struct direntv6) != 0) {
             fprintf(stderr, "Error: Directory block %d for inode %d has corrupted content size %d.\n", current_block_num, dirinumber, valid_bytes_in_block);
             return DIRECTORY_FAILURE;
        }

        int num_entries_in_block = valid_bytes_in_block / sizeof(struct direntv6);

        for (int i = 0; i < num_entries_in_block; i++) {
            struct direntv6 *current_entry = (struct direntv6 *)(block_buffer + (i * sizeof(struct direntv6)));

            // Skip unused/deleted entries
            if (current_entry->d_inumber == 0) {
                continue;
            }

            // Compare names. d_name is fixed size (14 chars), might not be null-terminated if full.
            // 'name' is null-terminated. strncmp is suitable here.
            if (strncmp(name, current_entry->d_name, sizeof(current_entry->d_name)) == 0) {
                // Match found
                memcpy(dirEnt, current_entry, sizeof(struct direntv6));
                return 0; // Success
            }
        }
        total_bytes_processed += valid_bytes_in_block;
        current_block_num++;
    }

    // If loops complete, the name was not found
    // fprintf(stderr, "Name '%s' not found in directory inode %d.\n", name, dirinumber);
    return DIRECTORY_FAILURE;
}
