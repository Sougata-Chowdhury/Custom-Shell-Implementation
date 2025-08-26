#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define BLOCK_SIZE 4096
#define TOTAL_BLOCKS 64
#define INODE_SIZE 256
#define INODE_COUNT 80
#define MAGIC_NUMBER 0xD34D

#define SUPERBLOCK_OFFSET 0
#define INODE_BITMAP_OFFSET 1
#define DATA_BITMAP_OFFSET 2
#define INODE_TABLE_START 3
#define DATA_BLOCK_START 8

typedef struct {
    uint16_t magic_number;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_bitmap_block;
    uint32_t data_bitmap_block;
    uint32_t inode_table_start_block;
    uint32_t first_data_block_number;
    uint32_t inode_size;
    uint32_t inode_count;
    uint8_t reserved[4058];
} Superblock;

typedef struct {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t file_size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint32_t hard_links;
    uint32_t blocks;
    uint32_t direct_block;
    uint32_t single_indirect_block;
    uint32_t double_indirect_block;
    uint32_t triple_indirect_block;
    uint8_t reserved[156];
} Inode;

int log_fd;
int fix_errors = 1;
int inodes_modified = 0;

void log_message(const char *message) {
    printf("%s", message);
    write(log_fd, message, strlen(message));
}

int is_valid_block(uint32_t block_num) {
    return (block_num >= DATA_BLOCK_START && block_num < TOTAL_BLOCKS);
}

void read_superblock(int fs, Superblock *sb) {
    lseek(fs, SUPERBLOCK_OFFSET * BLOCK_SIZE, SEEK_SET);
    read(fs, sb, sizeof(Superblock));
}

int validate_superblock(Superblock *sb) {
    log_message("Validating Superblock...\n");
    int errors = 0;
    if (sb->magic_number != MAGIC_NUMBER) {
        log_message("Error: Invalid magic number.\n");
        errors++;
    }
    if (sb->block_size != BLOCK_SIZE) {
        log_message("Error: Invalid block size.\n");
        errors++;
    }
    if (sb->total_blocks != TOTAL_BLOCKS) {
        log_message("Error: Invalid total number of blocks.\n");
        errors++;
    }
    if (sb->inode_bitmap_block != INODE_BITMAP_OFFSET) {
        log_message("Error: Invalid inode bitmap block.\n");
        errors++;
    }
    if (sb->data_bitmap_block != DATA_BITMAP_OFFSET) {
        log_message("Error: Invalid data bitmap block.\n");
        errors++;
    }
    if (sb->inode_table_start_block != INODE_TABLE_START) {
        log_message("Error: Invalid inode table start block.\n");
        errors++;
    }
    if (sb->first_data_block_number != DATA_BLOCK_START) {
        log_message("Error: Invalid first data block number.\n");
        errors++;
    }
    if (sb->inode_size != INODE_SIZE) {
        log_message("Error: Invalid inode size.\n");
        errors++;
    }
    if (sb->inode_count > (BLOCK_SIZE * 5) / INODE_SIZE) {
        log_message("Error: Inode count exceeds limit.\n");
        errors++;
    }
    if (errors == 0) {
        log_message("Superblock is valid.\n");
    } else {
        char msg[100];
        sprintf(msg, "Superblock has %d errors.\n", errors);
        log_message(msg);
    }
    log_message("Superblock validation complete.\n");
    return errors;
}

void read_bitmap(int fs, int block, uint8_t *bitmap, size_t size) {
    lseek(fs, block * BLOCK_SIZE, SEEK_SET);
    read(fs, bitmap, size);
}

void write_bitmap(int fs, int block, uint8_t *bitmap, size_t size) {
    if (fix_errors) {
        lseek(fs, block * BLOCK_SIZE, SEEK_SET);
        write(fs, bitmap, size);
        log_message("Bitmap updated on disk.\n");
    }
}

void read_inode_table(int fs, Inode *inode_table, size_t count) {
    lseek(fs, INODE_TABLE_START * BLOCK_SIZE, SEEK_SET);
    read(fs, inode_table, sizeof(Inode) * count);
}

void write_inode_table(int fs, Inode *inode_table, size_t count) {
    if (fix_errors && inodes_modified) {
        lseek(fs, INODE_TABLE_START * BLOCK_SIZE, SEEK_SET);
        write(fs, inode_table, sizeof(Inode) * count);
        log_message("Inode table updated on disk.\n");
        inodes_modified = 0;
    }
}

void read_block(int fs, uint32_t block_num, uint32_t *block_data) {
    lseek(fs, block_num * BLOCK_SIZE, SEEK_SET);
    read(fs, block_data, BLOCK_SIZE);
}

void write_block(int fs, uint32_t block_num, uint32_t *block_data) {
    if (fix_errors) {
        lseek(fs, block_num * BLOCK_SIZE, SEEK_SET);
        write(fs, block_data, BLOCK_SIZE);
    }
}

int is_valid_inode(Inode *inode) {
    return (inode->hard_links > 0 && inode->dtime == 0);
}

int check_fix_block_ptr(int fs, uint32_t *block_ptr, uint8_t *block_usage,
                         int *bad_blocks, int inode_num, const char *block_type) {
    if (*block_ptr == 0) return 0;
    if (!is_valid_block(*block_ptr)) {
        (*bad_blocks)++;
        char msg[150];
        sprintf(msg, "Error: Bad %s block %u (out of valid range) in inode %d.\n",
               block_type, *block_ptr, inode_num);
        log_message(msg);
        if (fix_errors) {
            sprintf(msg, "Fixing: Setting %s block pointer in inode %d to 0.\n",
                   block_type, inode_num);
            log_message(msg);
            *block_ptr = 0;
            return 1;
        }
    } else {
        block_usage[*block_ptr]++;
    }
    return 0;
}

int process_single_indirect(int fs, uint32_t block_num, uint8_t *block_usage,
                            int *bad_blocks, int inode_num, Inode *inode) {
    uint32_t block_pointers[BLOCK_SIZE / 4];
    int fixed_count = 0;
    read_block(fs, block_num, block_pointers);
    for (int i = 0; i < BLOCK_SIZE / 4; i++) {
        if (block_pointers[i] != 0) {
            if (!is_valid_block(block_pointers[i])) {
                (*bad_blocks)++;
                char msg[150];
                sprintf(msg, "Error: Bad block %u from single indirect in inode %d (out of valid range).\n",
                       block_pointers[i], inode_num);
                log_message(msg);
                if (fix_errors) {
                    sprintf(msg, "Fixing: Clearing invalid block pointer %d in single indirect block.\n", i);
                    log_message(msg);
                    block_pointers[i] = 0;
                    fixed_count++;
                    if (inode->blocks > 0) inode->blocks--;
                }
            } else {
                block_usage[block_pointers[i]]++;
            }
        }
    }
    if (fixed_count > 0 && fix_errors) write_block(fs, block_num, block_pointers);
    return fixed_count;
}

int process_double_indirect(int fs, uint32_t block_num, uint8_t *block_usage,
                           int *bad_blocks, int inode_num, Inode *inode) {
    uint32_t primary_pointers[BLOCK_SIZE / 4];
    int fixed_count = 0;
    read_block(fs, block_num, primary_pointers);
    for (int i = 0; i < BLOCK_SIZE / 4; i++) {
        if (primary_pointers[i] != 0) {
            if (!is_valid_block(primary_pointers[i])) {
                (*bad_blocks)++;
                char msg[150];
                sprintf(msg, "Error: Bad secondary pointer %u in double indirect block for inode %d.\n",
                       primary_pointers[i], inode_num);
                log_message(msg);
                if (fix_errors) {
                    sprintf(msg, "Fixing: Clearing invalid secondary pointer %d in double indirect block.\n", i);
                    log_message(msg);
                    primary_pointers[i] = 0;
                    fixed_count++;
                    if (inode->blocks > 0) inode->blocks--;
                }
            } else {
                block_usage[primary_pointers[i]]++;
                fixed_count += process_single_indirect(fs, primary_pointers[i], block_usage,
                                                     bad_blocks, inode_num, inode);
            }
        }
    }
    if (fixed_count > 0 && fix_errors) write_block(fs, block_num, primary_pointers);
    return fixed_count;
}

int process_triple_indirect(int fs, uint32_t block_num, uint8_t *block_usage,
                           int *bad_blocks, int inode_num, Inode *inode) {
    uint32_t primary_pointers[BLOCK_SIZE / 4];
    int fixed_count = 0;
    read_block(fs, block_num, primary_pointers);
    for (int i = 0; i < BLOCK_SIZE / 4; i++) {
        if (primary_pointers[i] != 0) {
            if (!is_valid_block(primary_pointers[i])) {
                (*bad_blocks)++;
                char msg[150];
                sprintf(msg, "Error: Bad tertiary pointer %u in triple indirect block for inode %d.\n",
                       primary_pointers[i], inode_num);
                log_message(msg);
                if (fix_errors) {
                    sprintf(msg, "Fixing: Clearing invalid tertiary pointer %d in triple indirect block.\n", i);
                    log_message(msg);
                    primary_pointers[i] = 0;
                    fixed_count++;
                    if (inode->blocks > 0) inode->blocks--;
                }
            } else {
                block_usage[primary_pointers[i]]++;
                fixed_count += process_double_indirect(fs, primary_pointers[i], block_usage,
                                                    bad_blocks, inode_num, inode);
            }
        }
    }
    if (fixed_count > 0 && fix_errors) write_block(fs, block_num, primary_pointers);
    return fixed_count;
}

int process_inode_blocks(int fs, Inode *inode, uint8_t *block_usage, int *bad_blocks, int inode_num) {
    int inode_modified = 0;
    int fixed_count = 0;
    inode_modified |= check_fix_block_ptr(fs, &inode->direct_block, block_usage,
                                         bad_blocks, inode_num, "direct");
    if (check_fix_block_ptr(fs, &inode->single_indirect_block, block_usage,
                          bad_blocks, inode_num, "single indirect")) {
        inode_modified = 1;
    } else if (inode->single_indirect_block != 0) {
        fixed_count = process_single_indirect(fs, inode->single_indirect_block, block_usage,
                                           bad_blocks, inode_num, inode);
        if (fixed_count > 0) inode_modified = 1;
    }
    if (check_fix_block_ptr(fs, &inode->double_indirect_block, block_usage,
                          bad_blocks, inode_num, "double indirect")) {
        inode_modified = 1;
    } else if (inode->double_indirect_block != 0) {
        fixed_count = process_double_indirect(fs, inode->double_indirect_block, block_usage,
                                           bad_blocks, inode_num, inode);
        if (fixed_count > 0) inode_modified = 1;
    }
    if (check_fix_block_ptr(fs, &inode->triple_indirect_block, block_usage,
                          bad_blocks, inode_num, "triple indirect")) {
        inode_modified = 1;
    } else if (inode->triple_indirect_block != 0) {
        fixed_count = process_triple_indirect(fs, inode->triple_indirect_block, block_usage,
                                           bad_blocks, inode_num, inode);
        if (fixed_count > 0) inode_modified = 1;
    }
    return inode_modified;
}

int check_and_fix_inode_bitmap(int fs, uint8_t *inode_bitmap, Inode *inode_table) {
    log_message("Checking Inode Bitmap...\n");
    int errors = 0;
    for (int i = 0; i < INODE_COUNT; i++) {
        int valid_inode = is_valid_inode(&inode_table[i]);
        int bitmap_set = inode_bitmap[i / 8] & (1 << (i % 8));
        if (valid_inode && !bitmap_set) {
            char msg[100];
            sprintf(msg, "Error: Inode %d is valid but not marked in bitmap.\n", i);
            log_message(msg);
            errors++;
            if (fix_errors) {
                sprintf(msg, "Fixing: Marking inode %d as used in bitmap.\n", i);
                log_message(msg);
                inode_bitmap[i / 8] |= (1 << (i % 8));
            }
        }
        if (!valid_inode && bitmap_set) {
            char msg[100];
            sprintf(msg, "Error: Inode %d is marked in bitmap but not valid.\n", i);
            log_message(msg);
            errors++;
            if (fix_errors) {
                sprintf(msg, "Fixing: Marking inode %d as unused in bitmap.\n", i);
                log_message(msg);
                inode_bitmap[i / 8] &= ~(1 << (i % 8));
            }
        }
    }
    if (errors == 0) {
        log_message("Inode bitmap is consistent.\n");
    } else {
        char msg[100];
        sprintf(msg, "Inode bitmap has %d inconsistencies.\n", errors);
        log_message(msg);
        if (fix_errors) write_bitmap(fs, INODE_BITMAP_OFFSET, inode_bitmap, BLOCK_SIZE);
    }
    log_message("Inode Bitmap check complete.\n");
    return errors;
}

int check_and_fix_data_bitmap(int fs, uint8_t *data_bitmap, Inode *inode_table) {
    log_message("Checking Data Bitmap...\n");
    uint8_t block_usage[TOTAL_BLOCKS] = {0};
    int bad_blocks = 0;
    int bitmap_errors = 0;
    inodes_modified = 0;
    for (int i = 0; i < INODE_COUNT; i++) {
        if (is_valid_inode(&inode_table[i])) {
            if (process_inode_blocks(fs, &inode_table[i], block_usage, &bad_blocks, i)) inodes_modified = 1;
        }
    }
    if (inodes_modified && fix_errors) write_inode_table(fs, inode_table, INODE_COUNT);
    for (int i = 0; i < DATA_BLOCK_START; i++) {
        int bitmap_set = data_bitmap[i / 8] & (1 << (i % 8));
        if (bitmap_set) {
            bitmap_errors++;
            if (fix_errors) data_bitmap[i / 8] &= ~(1 << (i % 8));
        }
    }
    for (int i = DATA_BLOCK_START; i < TOTAL_BLOCKS; i++) {
        int bitmap_set = data_bitmap[i / 8] & (1 << (i % 8));
        if (block_usage[i] > 0 && !bitmap_set) {
            char msg[100];
            sprintf(msg, "Error: Data block %d is used but not marked in bitmap.\n", i);
            log_message(msg);
            bitmap_errors++;
            if (fix_errors) {
                sprintf(msg, "Fixing: Marking data block %d as used in bitmap.\n", i);
                log_message(msg);
                data_bitmap[i / 8] |= (1 << (i % 8));
            }
        }
        if (block_usage[i] == 0 && bitmap_set) {
            char msg[100];
            sprintf(msg, "Error: Data block %d is marked in bitmap but not used.\n", i);
            log_message(msg);
            bitmap_errors++;
            if (fix_errors) {
                sprintf(msg, "Fixing: Marking data block %d as unused in bitmap.\n", i);
                log_message(msg);
                data_bitmap[i / 8] &= ~(1 << (i % 8));
            }
        }
    }
    if (bad_blocks > 0) {
        char msg[100];
        sprintf(msg, "Total bad blocks found: %d\n", bad_blocks);
        log_message(msg);
    } else {
        log_message("No bad blocks found.\n");
    }
    if (bitmap_errors == 0) {
        log_message("Data bitmap is consistent.\n");
    } else {
        char msg[100];
        sprintf(msg, "Data bitmap has %d inconsistencies.\n", bitmap_errors);
        log_message(msg);
        if (fix_errors) write_bitmap(fs, DATA_BITMAP_OFFSET, data_bitmap, BLOCK_SIZE);
    }
    log_message("Data Bitmap check complete.\n");
    return bitmap_errors + bad_blocks;
}

int fix_pointer_duplicate(uint32_t *ptr, uint8_t *block_seen, int *duplicates, Inode *inode) {
    if (*ptr == 0) return 0;
    if (!is_valid_block(*ptr)) return 0;
    if (block_seen[*ptr] == 0) {
        block_seen[*ptr] = 1;
        return 0;
    }
    (*duplicates)++;
    if (fix_errors) {
        *ptr = 0;
        if (inode->blocks > 0) inode->blocks--;
        return 1;
    }
    return 0;
}

int single_indirect_duplicate(int fs, uint32_t block_num, uint8_t *block_seen, int *duplicates, Inode *inode) {
    uint32_t ptrs[BLOCK_SIZE / 4];
    int modified = 0;
    if (block_num == 0) return 0;
    read_block(fs, block_num, ptrs);
    for (int i = 0; i < BLOCK_SIZE / 4; i++) {
        if (fix_pointer_duplicate(&ptrs[i], block_seen, duplicates, inode)) modified = 1;
    }
    if (modified && fix_errors) write_block(fs, block_num, ptrs);
    return modified;
}

int double_indirect_duplicate(int fs, uint32_t block_num, uint8_t *block_seen, int *duplicates, Inode *inode) {
    uint32_t primary[BLOCK_SIZE / 4];
    int modified = 0;
    if (block_num == 0) return 0;
    read_block(fs, block_num, primary);
    for (int i = 0; i < BLOCK_SIZE / 4; i++) {
        if (primary[i] != 0) {
            if (fix_pointer_duplicate(&primary[i], block_seen, duplicates, inode)) modified = 1;
            else if (single_indirect_duplicate(fs, primary[i], block_seen, duplicates, inode)) modified = 1;
        }
    }
    if (modified && fix_errors) write_block(fs, block_num, primary);
    return modified;
}

int triple_indirect_duplicate(int fs, uint32_t block_num, uint8_t *block_seen, int *duplicates, Inode *inode) {
    uint32_t primary[BLOCK_SIZE / 4];
    int modified = 0;
    if (block_num == 0) return 0;
    read_block(fs, block_num, primary);
    for (int i = 0; i < BLOCK_SIZE / 4; i++) {
        if (primary[i] != 0) {
            if (fix_pointer_duplicate(&primary[i], block_seen, duplicates, inode)) modified = 1;
            else if (double_indirect_duplicate(fs, primary[i], block_seen, duplicates, inode)) modified = 1;
        }
    }
    if (modified && fix_errors) write_block(fs, block_num, primary);
    return modified;
}

int inode_duplicate_pass(int fs, Inode *inode, uint8_t *block_seen, int *duplicates) {
    int modified = 0;
    if (fix_pointer_duplicate(&inode->direct_block, block_seen, duplicates, inode)) modified = 1;
    if (fix_pointer_duplicate(&inode->single_indirect_block, block_seen, duplicates, inode)) modified = 1;
    else if (single_indirect_duplicate(fs, inode->single_indirect_block, block_seen, duplicates, inode)) modified = 1;
    if (fix_pointer_duplicate(&inode->double_indirect_block, block_seen, duplicates, inode)) modified = 1;
    else if (double_indirect_duplicate(fs, inode->double_indirect_block, block_seen, duplicates, inode)) modified = 1;
    if (fix_pointer_duplicate(&inode->triple_indirect_block, block_seen, duplicates, inode)) modified = 1;
    else if (triple_indirect_duplicate(fs, inode->triple_indirect_block, block_seen, duplicates, inode)) modified = 1;
    return modified;
}

int check_duplicate_blocks(int fs, Inode *inode_table) {
    log_message("Checking for duplicate blocks...\n");
    uint8_t block_seen[TOTAL_BLOCKS] = {0};
    int duplicates = 0;
    inodes_modified = 0;
    for (int i = 0; i < INODE_COUNT; i++) {
        if (is_valid_inode(&inode_table[i])) {
            if (inode_duplicate_pass(fs, &inode_table[i], block_seen, &duplicates)) inodes_modified = 1;
        }
    }
    if (inodes_modified && fix_errors) write_inode_table(fs, inode_table, INODE_COUNT);
    if (duplicates == 0) {
        log_message("No duplicate blocks found.\n");
    } else {
        char msg[100];
        sprintf(msg, "Total duplicate blocks fixed: %d\n", duplicates);
        log_message(msg);
    }
    log_message("Duplicate block check complete.\n");
    return duplicates;
}

int check_and_fix_filesystem(int fs) {
    int total_errors = 0;
    Superblock sb;
    read_superblock(fs, &sb);
    total_errors += validate_superblock(&sb);
    uint8_t inode_bitmap[BLOCK_SIZE];
    uint8_t data_bitmap[BLOCK_SIZE];
    read_bitmap(fs, INODE_BITMAP_OFFSET, inode_bitmap, BLOCK_SIZE);
    read_bitmap(fs, DATA_BITMAP_OFFSET, data_bitmap, BLOCK_SIZE);
    Inode inode_table[INODE_COUNT];
    read_inode_table(fs, inode_table, INODE_COUNT);
    total_errors += check_and_fix_inode_bitmap(fs, inode_bitmap, inode_table);
    total_errors += check_and_fix_data_bitmap(fs, data_bitmap, inode_table);
    total_errors += check_duplicate_blocks(fs, inode_table);
    return total_errors;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <file_system_image>\n", argv[0]);
        return 1;
    }
    int fs = open(argv[1], O_RDWR);
    if (fs == -1) {
        perror("Error opening file system");
        return 1;
    }
    log_fd = open("vsfsck_log.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd == -1) {
        perror("Error opening log file");
        close(fs);
        return 1;
    }
    log_message("PASS 1: Checking and fixing file system errors\n");
    fix_errors = 1;
    int initial_errors = check_and_fix_filesystem(fs);
    if (initial_errors > 0) {
        char msg[100];
        sprintf(msg, "PASS 1: Found and fixed %d errors\n", initial_errors);
        log_message(msg);
        log_message("\nPASS 2: Verifying file system integrity after fixes\n");
        fix_errors = 0;
        int remaining_errors = check_and_fix_filesystem(fs);
        if (remaining_errors > 0) {
            sprintf(msg, "PASS 2: %d errors remain after fixes\n", remaining_errors);
            log_message(msg);
        } else {
            log_message("PASS 2: File system is now consistent\n");
        }
    } else {
        log_message("PASS 1: No errors found, file system is consistent\n");
    }
    log_message("---- VSFSck completed ----\n\n");
    close(fs);
    close(log_fd);
    return 0;
}
