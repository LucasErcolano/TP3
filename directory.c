#include "directory.h"
#include "inode.h"
#include "diskimg.h"
#include "file.h"
#include "unixfilesystem.h"
#include "direntv6.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

// Helper macro for error returns, can be adapted if specific negative error codes are needed
#define DIRECTORY_FAILURE -1 

/**
 * Looks up the specified name (name) in the specified directory (dirinumber).
 * If found, return the directory entry in space addressed by dirEnt.  Returns 0
 * on success and something negative on failure.
 * ESTA VERSIÓN ESTÁ OPTIMIZADA para evitar lecturas redundantes del inodo del directorio.
 */
int directory_findname(struct unixfilesystem *fs, const char *name,
                       int dirinumber, struct direntv6 *dirEnt) {
    struct inode dir_inode;

    // Preliminary check: if the name to find is too long, it can't exist
    if (strlen(name) > sizeof(dirEnt->d_name)) {
        // fprintf(stderr, "Error directory_findname: Search name '%s' is longer than max filename length (%lu).\n", name, sizeof(dirEnt->d_name));
        return DIRECTORY_FAILURE;
    }

    // 1. Get the Directory's Inode
    if (inode_iget(fs, dirinumber, &dir_inode) < 0) {
        // inode_iget prints its own error
        return DIRECTORY_FAILURE;
    }

    // 2. Verify it's a Directory
    if ((dir_inode.i_mode & IFMT) != IFDIR) {
        fprintf(stderr, "Error directory_findname: Inode %d is not a directory (i_mode: %04x).\n", dirinumber, dir_inode.i_mode);
        return DIRECTORY_FAILURE;
    }

    // 3. Get Directory Size
    int dir_size_bytes = inode_getsize(&dir_inode);
    if (dir_size_bytes == 0) {
        // fprintf(stderr, "directory_findname: Directory inode %d is empty.\n", dirinumber);
        return DIRECTORY_FAILURE; // Empty directory, name cannot be found
    }

    // Directory size should be a multiple of directory entry size
    if (dir_size_bytes % sizeof(struct direntv6) != 0) {
        fprintf(stderr, "Error directory_findname: Directory inode %d has corrupted size %d.\n", dirinumber, dir_size_bytes);
        return DIRECTORY_FAILURE;
    }

    // 4. Iterate Through Directory Entries
    unsigned char block_buffer[DISKIMG_SECTOR_SIZE];
    int total_bytes_processed = 0;
    int current_logical_block_num = 0;

    while (total_bytes_processed < dir_size_bytes) {
        // a. Obtener el número de bloque de disco físico para el bloque lógico actual del directorio.
        int disk_sector_num = inode_indexlookup(fs, &dir_inode, current_logical_block_num);
        
        if (disk_sector_num < 0) { // Error o bloque no asignado (ej. agujero en el archivo de directorio, aunque inusual)
            fprintf(stderr, "Error directory_findname: Could not find disk sector for directory inode %d, logical block %d.\n", dirinumber, current_logical_block_num);
            return DIRECTORY_FAILURE;
        }
         if (disk_sector_num == 0) { // Un sector de disco 0 es inválido para datos en Unix V6.
            fprintf(stderr, "Error directory_findname: inode_indexlookup returned disk_sector_num 0 for dir inode %d, block %d.\n", dirinumber, current_logical_block_num);
            return DIRECTORY_FAILURE;
        }

        // b. Leer el bloque de disco.
        if (diskimg_readsector(fs->dfd, disk_sector_num, block_buffer) != DISKIMG_SECTOR_SIZE) {
            fprintf(stderr, "Error directory_findname: Failed to read disk sector %d for directory inode %d, block %d.\n",
                    disk_sector_num, dirinumber, current_logical_block_num);
            return DIRECTORY_FAILURE;
        }

        // c. Calcular cuántos bytes son válidos en este bloque leído.
        int valid_bytes_in_block;
        long start_byte_of_this_block = (long)current_logical_block_num * DISKIMG_SECTOR_SIZE;
        int remaining_bytes_in_dir = dir_size_bytes - start_byte_of_this_block;

        if (remaining_bytes_in_dir >= DISKIMG_SECTOR_SIZE) {
            valid_bytes_in_block = DISKIMG_SECTOR_SIZE;
        } else {
            valid_bytes_in_block = remaining_bytes_in_dir;
        }
        
        if (valid_bytes_in_block <= 0) { // No debería ocurrir si total_bytes_processed < dir_size_bytes
             fprintf(stderr, "Warning directory_findname: Read 0 or negative valid bytes (%d) from dir inode %d, block %d, but expected more data.\n", valid_bytes_in_block, dirinumber, current_logical_block_num);
            break; 
        }

        // El contenido de un bloque de directorio también debe ser un múltiplo del tamaño de la entrada.
        if (valid_bytes_in_block % sizeof(struct direntv6) != 0) {
             fprintf(stderr, "Error directory_findname: Directory block %d (disk sector %d) for inode %d has corrupted content size %d.\n", current_logical_block_num, disk_sector_num, dirinumber, valid_bytes_in_block);
             return DIRECTORY_FAILURE;
        }

        // d. Iterar a través de las entradas direntv6 en el buffer.
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
        current_logical_block_num++;
    }

    // If loops complete, the name was not found
    // fprintf(stderr, "directory_findname: Name '%s' not found in directory inode %d.\n", name, dirinumber);
    return DIRECTORY_FAILURE;
}
