#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "simplefs.h"
#include "string.h"

#define FCB_SIZE 128
#define MAX_FILE_COUNT 128
#define MAX_FILENAME 110
#define MAX_OPEN_FILES 16
#define MAX_FCB_COUNT 128
#define MAX_BITMAP_SIZE 32768 // Bitmap size for each block: 32768 Bits -> 4KB's
#define MAX_ENTRY 32 // Max entry for root directory and FCB blocks.

#define BITMAP_BLOCK_COUNT 4 // 1-4
#define ROOT_BLOCK_COUNT 4 // 5-8
#define FCB_BLOCK_COUNT 4 // 9-12

// *********** Function Prototypes: ***********
int read_block (void *block, int k);
int write_block (void *block, int k);
int create_format_vdisk (char *vdiskname, unsigned int m);
int sfs_mount (char *vdiskname);
int sfs_umount ();
int sfs_create(char *filename);
int sfs_open(char *file, int mode);
int sfs_close(int fd);
int sfs_getsize(int fd);
int sfs_read(int fd, void *buf, int n);
int sfs_append(int fd, void *buf, int n);
int sfs_delete(char *filename);
int find_free_block();
void update_bitmap(int index, int set);
int search_file(char filename[MAX_FILENAME]);
// *********** End of Function Prototypes ***********

// Global Variables =======================================
int vdisk_fd; // Global virtual disk file descriptor. Global within the library.
              // Will be assigned with the vsfs_mount call.
              // Any function in this file can use this.
              // Applications will not use this directly.
// ========================================================


// *********************************************** //
// ***** STRUCT DEFINITIONS AND CONSTRUCTORS ***** //
// *********************************************** //

// -- Bitmap implementation -- //
typedef unsigned char* t_bitmap;

t_bitmap create_bitmap(int n) {
    return malloc((n + 7) / 8);
}

// Sets the bit to on at index n
void bm_set_one( t_bitmap bm, int n ) {
    bm[n / 8] |= 1 << (n & 7);
}

// Sets the bit to zero at index n
void bm_set_zero( t_bitmap bm, int n ) {
    bm[n / 8] &= ~(1 << (n & 7));
}

// Returns the state of the bit at index n
int get_bm_value( t_bitmap bm, int n ) {
    return bm[n / 8] & (1 << (n & 7)) ? 1 : 0;
}
// -- End of Bitmap implementation -- //

struct OpenTable {
    char * names[MAX_OPEN_FILES];
    int entry_indexes[MAX_OPEN_FILES];
};

struct Superblock { // For block 0
    int total_block_amt;
    int curr_file_amt;
    int curr_open; // Currently opened file amt
    struct OpenTable open_table;
};

// All data block numbers for a file will be included in the index node
// One index node per file -> 4MB max file size (4KB * (4 KB / 4 bytes))
struct IndexBlock {
    // Disk pointer size (block number) will be 4 bytes, therefore there will be 1024 disk pointers
    unsigned int ptr[BLOCKSIZE/4];
};

// Directory Entry: Mapping of file to its inode
struct DirectoryEntry { // Size is 128 bytes. Each disk block can hold 32 directory entries.
    char name[MAX_FILENAME]; // size = MAX_FILENAME
    int file_size;
    int fcb_index;
    int mode;
};

// Root Directory
struct Directory { // For blocks 5, 6, 7, 8
    struct DirectoryEntry entries[MAX_ENTRY]; // At most 128 files in disk (MAX_ENTRY * 4).
};

// File Control Block
struct FCB { // Size = 128 bytes
    int used;
    int used_block_count;
    int iblock_index; // Index of index block
    int last_item_offset; // Index of the last inserted item
    int last_read_offset; // Index of the last readed item
};

struct FCBTable {
    int curr_table_size;
    struct FCB fcbs[MAX_ENTRY];
};


// ------------- Constructors ------------- //

// Initialize all four bitmap blocks. (16 KB's, 4KB EACH)
void init_bitmap_blocks() {
    t_bitmap first_bm = create_bitmap(MAX_BITMAP_SIZE);
    t_bitmap bm = create_bitmap(MAX_BITMAP_SIZE);

    for( int i = 0; i < MAX_BITMAP_SIZE; i++ ) {
        if( i <= 12 ) {
            bm_set_one(first_bm, i);
        }
    }

    // Write into blocks
    for( int i = 1; i < BITMAP_BLOCK_COUNT; i++ ) {
        if( i == 1 ) {
            write_block(first_bm, i);
        } else {
            write_block(bm, i);
        }

    }

    free(first_bm);
    free(bm);
}

// Initialize the superblock.
void init_superblock(int disk_size) {
    struct Superblock * sb = (struct Superblock *) malloc(BLOCKSIZE);

    sb->total_block_amt = disk_size / BLOCKSIZE;
    printf("Block count = %d\n", disk_size / BLOCKSIZE);
    sb->curr_file_amt = 0;
    sb->curr_open = 0;
    for( int i = 0; i < MAX_OPEN_FILES; i++ ){
        sb->open_table.entry_indexes[i] = -1;
        sb->open_table.names[i] = "";
    }

    write_block (sb, 0);
    free(sb);
}

// Initialize all four root directory blocks
void init_directory_blocks() {
    struct Directory* dir = (struct Directory *) malloc(BLOCKSIZE);
    for( int i = 0; i < MAX_ENTRY; i++ ){
        dir->entries[i].name[0] = '\0';
        dir->entries[i].file_size = -1;
        dir->entries[i].fcb_index = -1;
        dir->entries[i].mode = -1;
    }

    for( int i = 5; i < 5 + ROOT_BLOCK_COUNT; i++ ){
        write_block(dir, i);
    }

    free( dir );
}


// Initialize all four blocks that contain FCBs (Total of 32*4 FCBs)
void init_fcb_blocks() {
    struct FCBTable * fcb_table;
    fcb_table = (struct FCBTable *) malloc(BLOCKSIZE);
    //unsigned int inode[BLOCKSIZE/4] = {0};

    for( int i = 0; i < MAX_ENTRY; i++ ){
        fcb_table->fcbs[i].used = 0;
        fcb_table->fcbs[i].used_block_count = 0;
        fcb_table->fcbs[i].iblock_index = -1;
        fcb_table->fcbs[i].last_item_offset = 0;
        fcb_table->fcbs[i].last_read_offset = -1;
    }

    for( int i = 9; i < 9 + FCB_BLOCK_COUNT; i++ ){
        write_block(fcb_table, i);
    }

    free( fcb_table );
}


// ------------- End of Constructors ------------- //

// *********************************************** //
// ** END OF STRUCT DEFINITIONS AND CONSTRUCTORS * //
// *********************************************** //


// read block k from disk (virtual disk) into buffer block.
// size of the block is BLOCKSIZE.
// space for block must be allocated outside of this function.
// block numbers start from 0 in the virtual disk. 
int read_block (void *block, int k) {
    int n;
    int offset;

    offset = k * BLOCKSIZE;
    lseek(vdisk_fd, (off_t) offset, SEEK_SET);
    n = read (vdisk_fd, block, BLOCKSIZE);
    if (n != BLOCKSIZE) {
	    printf ("read error\n");
	    return -1;
    }
    return (0); 
}

// write block k into the virtual disk. 
int write_block (void *block, int k) {
    int n;
    int offset;

    offset = k * BLOCKSIZE;
    lseek(vdisk_fd, (off_t) offset, SEEK_SET);
    n = write (vdisk_fd, block, BLOCKSIZE);
    if (n != BLOCKSIZE) {
	printf ("write error\n");
	return (-1);
    }
    return 0; 
}

/**********************************************************************
   The following functions are to be called by applications directly. 
***********************************************************************/

int create_format_vdisk (char *vdiskname, unsigned int m) {
    char command[1000];
    int size;
    int num = 1;
    int count;
    size  = num << m;
    count = size / BLOCKSIZE;
    //    printf ("%d %d", m, size);
    sprintf (command, "dd if=/dev/zero of=%s bs=%d count=%d",
             vdiskname, BLOCKSIZE, count);
    //printf ("executing command = %s\n", command);
    system (command);

    // now write the code to format the disk below.
    // .. your code...
    sfs_mount(vdiskname);

    init_superblock(size);
    init_bitmap_blocks();
    init_directory_blocks();
    init_fcb_blocks();

    t_bitmap bm = create_bitmap(MAX_BITMAP_SIZE);
    read_block(bm, 1);

    //printf("Initial Bitmap:\n");
    //for( int i = 0; i < size / BLOCKSIZE; i++ ) {
    //    printf("(%d: %d, )", i, get_bm_value(bm, i));
    //}

    free(bm);
    return (0); 
}


// already implemented
int sfs_mount (char *vdiskname) {
    // simply open the Linux file vdiskname and in this
    // way make it ready to be used for other operations.
    // vdisk_fd is global; hence other functions can use it.
    vdisk_fd = open(vdiskname, O_RDWR);
    return(0);
}


// already implemented
int sfs_umount () {
    fsync (vdisk_fd); // copy everything in memory to disk
    close (vdisk_fd);
    return (0); 
}

// Find a free directory entry location to insert and do the insertion
int insert_dir_entry_into_free(char filename[MAX_FILENAME]) {
    struct Directory* dir = (struct Directory *) malloc(BLOCKSIZE);

    for( int i = 5; i < ROOT_BLOCK_COUNT + 5; i++ ) { // In each root directory block
        read_block(dir, i);
        for( int j = 0; j < MAX_ENTRY; j++ ) {
            if( dir->entries[j].file_size == -1 ) {
                printf( "Found directory entry at block %d, entry %d! Inserting file \"%s\"...\n", i, j, filename );
                dir->entries[j].file_size = 0; // -1 to 0

                strcpy( dir->entries[j].name, filename );

                write_block(dir, i);
                free(dir);
                return 0;

            }
        }
    }

    printf("Error: Cannot have more than 128 files!\n");
    free(dir);
    return -1;
}

void print_root_dirs() {
    printf("Available files in root directory:\n");
    struct Directory* dir = (struct Directory *) malloc(BLOCKSIZE);
    for( int i = 0; i < ROOT_BLOCK_COUNT; i++ ) {
        read_block(dir, i);
        for( int j = 0; j < MAX_ENTRY; j++ ) {
            if( dir->entries[j].file_size == -1 ) {
                printf( "Entry at block %d, entry %d: FS=%d, FNAME=%s\n", i, j, dir->entries[j].file_size, dir->entries[j].name );
            }
        }
    }

    free(dir);
}


// Find a free fcb location to insert and do the insertion
// Create an index block for the file
int insert_fcb_into_free(char filename[MAX_FILENAME]) {
    struct FCBTable * fcb_table = (struct FCBTable *) malloc(BLOCKSIZE);

    int fcb_index = -1;

    for( int i = 9; i < FCB_BLOCK_COUNT + 9; i++ ) { // In each root directory block
        read_block(fcb_table, i); // Read the FCB table of each block
        for( int j = 0; j < MAX_ENTRY; j++ ) {
            if( fcb_table->fcbs[j].used == 0 ) { // Found unused FCB
                printf( "\nFound free FCB at block %d, index %d! (fcb_index=%d, filename=%s) Inserting...\n", i, j, (i*32+j), filename );

                fcb_table->fcbs[j].used = 1;
                fcb_table->fcbs[j].last_item_offset = 0;
                fcb_index = ((i - 9) * 32) + j;

                // Create an index block for the file and save it inside FCB
                struct IndexBlock * index_block = (struct IndexBlock *) malloc(BLOCKSIZE);
                int block_index = find_free_block();
                printf("Inserting index table of file \"%s\" into block %d.\n", filename, block_index);


                for( int i = 0; i < BLOCKSIZE / 4; i++ ) {
                    index_block->ptr[i] = 0;
                }

                fcb_table->fcbs[j].iblock_index = block_index;

                // Update bitmap
                update_bitmap( block_index, 1);

                // Save changes
                write_block(index_block, block_index);
                write_block(fcb_table, i);

                free(fcb_table);
                free(index_block);
                break;
            }
        }

        if(fcb_index != -1) { // Already found
            break;
        }
    }

    if( fcb_index == -1 ) {
        printf("Error: Cannot have more than 128 FCBs!\n");

        free(fcb_table);
        return -1;
    }

    // Find directory entry with name 'filename' and update its value
    struct Directory* dir = (struct Directory *) malloc(BLOCKSIZE);

    for( int i = 5; i < ROOT_BLOCK_COUNT + 5; i++ ) { // In each root directory block
        read_block(dir, i);
        for( int j = 0; j < MAX_ENTRY; j++ ) {
            if( strcmp(dir->entries[j].name, filename) == 0 ) {
                printf( "Registering FCB with index %d into directory entry with name=\"%s\" (Block=%d, Entry=%d)\n", fcb_index, dir->entries[j].name, i, j );

                dir->entries[j].fcb_index = fcb_index;

                write_block(dir, i);
                free(dir);
                return ((i - 5) * 32 + j); // Return entry index

            }
        }
    }

    free(dir);
    printf("Error: Could not find directory entry!\n");
    return -1;
}

int sfs_create(char *filename) {
    int exists = search_file(filename);

    if( exists != -1 ) { printf("Error: This file already exists\n"); return -1; }

    // Use an entry in the root directory to store information about the created file, like its name, size
    // Read the superblock
    struct Superblock* sb;
    sb = (struct Superblock *) malloc( BLOCKSIZE );
    read_block( sb, 0 );

    if( sb->curr_file_amt >= MAX_FILE_COUNT ) {
        printf("Error: Came to maximum file amount. Terminating...\n");
        exit(1);
    }

    //printf("Current file amount: %d\n", sb->curr_file_amt);
    sb->curr_file_amt++;
    write_block (sb, 0);
    free(sb);

    insert_dir_entry_into_free(filename);
    insert_fcb_into_free(filename);
    return (0);
}

// Find a free block from the bitmap
// Returns -1 if not found and block index if found
int find_free_block() {
    struct Superblock* sb = (struct Superblock *) malloc( BLOCKSIZE );
    read_block(sb, 0);

    t_bitmap bitmap = create_bitmap(MAX_BITMAP_SIZE);

    for( int i = 1; i < BITMAP_BLOCK_COUNT + 1; i++ ) { // Iterate through all bitmap blocks
        read_block(bitmap, i);
        for( int j = 0; j < MAX_BITMAP_SIZE; j++ ) { // Iterate through all bits inside each block
            if( get_bm_value(bitmap, j) == 0 ) { // Found an empty one
                //printf("Found empty block at index: %d\n", (((i-1) * MAX_BITMAP_SIZE) + j));
                bm_set_one(bitmap, j);
                write_block(bitmap, i);
                free(bitmap);
                return ((i-1) * MAX_BITMAP_SIZE + j);
            }
        }
    }

    free(bitmap);
    return -1;
}

void update_bitmap(int index, int set) {
    t_bitmap bm = create_bitmap(MAX_BITMAP_SIZE);

    read_block(bm, index / MAX_BITMAP_SIZE); // Bitmap block

    // Update bit inside bitmap
    if(set == 1) {
        bm_set_one(bm, index % MAX_BITMAP_SIZE);
    } else {
        bm_set_zero(bm, index % MAX_BITMAP_SIZE);
    }

    // Save
    write_block(bm, index / MAX_BITMAP_SIZE);
    free(bm);
}


// Search for a file and return its index ((Block Index * 32) + (place inside block))
int search_file(char filename[MAX_FILENAME]) {
    struct Directory * d = (struct Directory *) malloc(BLOCKSIZE);

    // Find the file with the same name:
    for( int i = 5; i < 5 + ROOT_BLOCK_COUNT; i++ ) {
        read_block(d, i);
        for( int j = 0; j < MAX_ENTRY; j++ ) {
            if( strcmp(d->entries[j].name, filename) == 0 ) {
                int index = ((i - 5) * 32) + j;
                free(d);

                return index;
            }
        }
    }

    free(d);
    return -1;
}


int set_mode(char filename[MAX_FILENAME], int mode) {
    struct Directory * d = (struct Directory *) malloc(BLOCKSIZE);
    int dir_entry_index = search_file(filename);

    if(dir_entry_index == -1) {
        printf("Error: File not found!\n");
        return -1;
    }

    read_block(d, 1 + BITMAP_BLOCK_COUNT + (dir_entry_index / MAX_ENTRY));
    d->entries[dir_entry_index % MAX_ENTRY].mode = mode;

    write_block(d, 1 + BITMAP_BLOCK_COUNT + (dir_entry_index / MAX_ENTRY));
    free(d);
    return dir_entry_index;
}

// mode: MODE_READ or MODE_APPEND
int sfs_open( char *file, int mode ) {
    printf("Opening file \"%s\"...\n", file);

    // Set the open files in superblock
    struct Superblock * sb = malloc(BLOCKSIZE);
    read_block(sb, 0);

    if(sb->curr_open >= 16) { printf("Error: Cannot have more than 16 open files!\n"); return -1; }

    int file_entry_index = set_mode(file, mode);

    if( file_entry_index == -1 ) { return -1; }

    // Check if file is created before
    struct Directory* dir = (struct Directory *) malloc(BLOCKSIZE);
    int file_exists = 0;
    for( int i = 5; i < ROOT_BLOCK_COUNT + 5; i++ ) { // In each root directory block
        read_block(dir, i);
        for( int j = 0; j < MAX_ENTRY; j++ ) {
            if( strcmp(dir->entries[j].name, file) == 0 ) {
                file_exists = 1;
                break;
            }
        }
        if( file_exists == 1 ) { break; }
    }

    free(dir);

    if( file_exists == 0 ) { printf("Error: Cannot open file. This file does not exist. You should create the file first.\n"); return -1; }


    int open_index = -1;
    for( int i = 0; i <= sb->curr_open; i++ ) {
        if( sb->open_table.entry_indexes[i] == -1 ) { // Have an open place
            open_index = i;
            sb->open_table.names[i] = file;
            sb->open_table.entry_indexes[i] = file_entry_index;
        }
    }

    if( open_index == -1 ) { printf("Error: Something wrong with the open table!\n"); return -1; } // Should not happen

    sb->curr_open++;

    printf("Open files table (Curr Open File Amt=%d): { ", sb->curr_open);
    for(int i = 0; i < MAX_OPEN_FILES; i++) {
        if( sb->open_table.entry_indexes[i] != -1 ) { // Print only filled ones
            printf("[Name=%s, Dir Entry Index=%d], ", sb->open_table.names[i], sb->open_table.entry_indexes[i]);
        }
    }
    printf(" }\n");

    write_block(sb, 0);

    free(sb);
    return open_index;
}

int sfs_close(int fd) {

    // Set the open files in superblock
    struct Superblock * sb = malloc(BLOCKSIZE);
    read_block(sb, 0);

    if(fd > 15) { printf("Error: Something wrong with the fd!\n"); return -1; } // should not happen

    int dir_entry_index = sb->open_table.entry_indexes[fd];

    if( dir_entry_index == -1 ) { printf("This file is already closed!\n"); return -1; }

    struct Directory * dir = (struct Directory *) malloc(BLOCKSIZE);
    read_block(dir, 1 + BITMAP_BLOCK_COUNT + (dir_entry_index / MAX_ENTRY));
    struct DirectoryEntry entry = dir->entries[dir_entry_index % 32]; // Get file's directory entry
    int fcb_index = entry.fcb_index;
    char * filename = entry.name;

    printf("Closing file \"%s\"...\n", filename);

    struct FCBTable * fcb_table = (struct FCBTable *) malloc(BLOCKSIZE);
    read_block(fcb_table, 1 + BITMAP_BLOCK_COUNT + FCB_BLOCK_COUNT + (fcb_index / MAX_ENTRY));
    fcb_table->fcbs[fcb_index % MAX_ENTRY].last_read_offset = 0; // Set last read attribute in FCB to zero

    sb->open_table.entry_indexes[fd] = -1; // Closed = -1
    sb->open_table.names[fd] = "";
    sb->curr_open--;

    write_block(sb, 0);
    write_block(fcb_table, 1 + BITMAP_BLOCK_COUNT + FCB_BLOCK_COUNT + (fcb_index / MAX_ENTRY));
    free(sb);
    free(fcb_table);

    return (0); 
}

int sfs_getsize(int fd) {
    struct Directory* dir = (struct Directory *) malloc(BLOCKSIZE);

    int entry_block = fd / MAX_ENTRY; // Get block number
    int index = fd % MAX_ENTRY; // Get offset inside block (index)

    read_block(dir, 1 + BITMAP_BLOCK_COUNT + entry_block);
    int size = dir->entries[index].file_size;
    free(dir);

    return size;
}

int sfs_read(int fd, void *buf, int n){

    // Get the index block of the file
    struct Superblock * sb = (struct Superblock *) malloc(BLOCKSIZE);
    read_block(sb, 0);
    int dir_entry_index = sb->open_table.entry_indexes[fd];

    if( dir_entry_index == -1 ) {
        printf("Error: This file is not open or does not exist!\n");
        return -1;
    }

    struct Directory * dir = (struct Directory *) malloc(BLOCKSIZE);
    read_block(dir, 1 + BITMAP_BLOCK_COUNT + (dir_entry_index / MAX_ENTRY));
    struct DirectoryEntry entry = dir->entries[dir_entry_index % 32]; // Get file's directory entry
    int fcb_index = entry.fcb_index;

    if( entry.mode == MODE_APPEND ) {
        printf("Error: Cannot read. This file is in APPEND mode.\n");
        return -1;
    }

    struct FCBTable * fcb_table = (struct FCBTable *) malloc(BLOCKSIZE);
    read_block(fcb_table, 1 + BITMAP_BLOCK_COUNT + FCB_BLOCK_COUNT + (fcb_index / MAX_ENTRY));
    struct FCB fcb = fcb_table->fcbs[fcb_index % MAX_ENTRY]; // Get file's FCB
    int iblock_index = fcb.iblock_index; // Block number of index block

    if( fcb.iblock_index == -1 ) { // Index block DNE!
        printf("Error: No index block!\n"); return -1; // Should not happen
    }

    struct IndexBlock * index_block = (struct IndexBlock *) malloc(BLOCKSIZE);
    read_block(index_block, iblock_index);

    //printf("---READ:--- Reading the file with fd=(%d) name=\"%s\". Block Number of Index Block=(%d). Used block count=(%d)\n", fd, filename, iblock_index, fcb.used_block_count);

    int curr_block_index = fcb.last_read_offset / BLOCKSIZE;

    // Get the current block
    char * curr_block = (char *) malloc(BLOCKSIZE);
    read_block(curr_block, (int)index_block->ptr[curr_block_index]);

    int count = 0;
    int in_block_index = fcb.last_read_offset % BLOCKSIZE;
    // Iterate through all used blocks:
    while( count < n ) {
        if( in_block_index == BLOCKSIZE ) { // Go into next block
            curr_block_index++;
            in_block_index = 0;

            if(curr_block_index > fcb.used_block_count) {
                printf("Warning: Exceeded read limit. You cannot read more that you write\n");
                return count;
            }

            read_block(curr_block, (int)index_block->ptr[curr_block_index]);
        }

        //printf("Char read: %c\n", curr_block[in_block_index]);
        ((char *)buf)[count] = curr_block[in_block_index];

        count++;
        in_block_index++;
    }

    fcb_table->fcbs[fcb_index % MAX_ENTRY].last_read_offset += n;
    write_block(fcb_table, 1 + BITMAP_BLOCK_COUNT + FCB_BLOCK_COUNT + (fcb_index / MAX_ENTRY));
    free(fcb_table);
    free(index_block);
    return n;
}

int sfs_append(int fd, void *buf, int n) {

    // Get the index block of the file
    struct Superblock * sb = (struct Superblock *) malloc(BLOCKSIZE);
    read_block(sb, 0);
    int dir_entry_index = sb->open_table.entry_indexes[fd];

    if( dir_entry_index == -1  ) {
        printf("Error: This file is not open or does not exist!\n");
        return -1;
    }

    struct Directory * dir = (struct Directory *) malloc(BLOCKSIZE);
    read_block(dir, 1 + BITMAP_BLOCK_COUNT + (dir_entry_index / MAX_ENTRY));
    struct DirectoryEntry entry = dir->entries[dir_entry_index % 32]; // Get file's directory entry
    int fcb_index = entry.fcb_index;
    char * filename = entry.name;

    if( entry.mode == MODE_READ ) {
        printf("Error: Cannot append. This file is in READ mode.\n");
        return -1;
    }

    struct FCBTable * fcb_table = (struct FCBTable *) malloc(BLOCKSIZE);
    read_block(fcb_table, 1 + BITMAP_BLOCK_COUNT + FCB_BLOCK_COUNT + (fcb_index / MAX_ENTRY));
    struct FCB fcb = fcb_table->fcbs[fcb_index % MAX_ENTRY]; // Get file's FCB
    int iblock_index = fcb.iblock_index; // Block number of index block

    if( fcb.iblock_index == -1 ) { // Index block DNE!
        printf("Error: No index block!\n"); return -1; // Should not happen
    }

    struct IndexBlock * index_block = (struct IndexBlock *) malloc(BLOCKSIZE);
    read_block(index_block, iblock_index);

    //printf("---APPEND:--- Appending to file with fd=(%d) name=\"%s\". Block Number of Index Block=(%d). Used block count=(%d)\n", fd, filename, iblock_index, fcb.used_block_count);

    // Do not have any data blocks inside index block!
    if( fcb.used_block_count == 0 ) {
        //printf("No data blocks -> Allocating new block for file %s\n", filename);
        int free_index = find_free_block();

        // Add new data block into index table
        index_block->ptr[fcb.used_block_count] = free_index;
        fcb_table->fcbs[fcb_index % MAX_ENTRY].used_block_count++; // Increment used block count

        update_bitmap(free_index, 1);
        write_block(index_block, iblock_index);
        write_block(fcb_table, 1 + FCB_BLOCK_COUNT + BITMAP_BLOCK_COUNT + (fcb_index / MAX_ENTRY));
    }

    if( fcb.last_item_offset + n >= BLOCKSIZE ) { // If the size exceeds the block size
        // Need to add another block into index node table
        fcb_table->fcbs[fcb_index % MAX_ENTRY].last_item_offset = 0;
        int free_index = find_free_block();
        printf("(APPEND) Block full: Allocating additional data block for file \"%s\" on index %d \n", filename, free_index);
        index_block->ptr[fcb.used_block_count] = free_index;
        fcb_table->fcbs[fcb_index % MAX_ENTRY].used_block_count++; // Increment used block count
        update_bitmap(free_index, 1);

        // Now add the actual data into new data block
        char * data_block = (char *) malloc(BLOCKSIZE);
        read_block(data_block, free_index);

        char * buffer = (char *)buf;
        for( int i = 0; i < n; i++ ) { // Add the data into data block
            data_block[i] = buffer[i];
        }

        write_block(data_block, free_index);
        free(data_block);
    } else {
        int curr_data_block = (int) index_block->ptr[fcb_table->fcbs[fcb_index % MAX_ENTRY].used_block_count - 1];
        int last_offset = fcb_table->fcbs[fcb_index % MAX_ENTRY].last_item_offset;

        //printf("Appending to the end of the current file \"%s\". Last item offset after insert: (%d). Current data block: (%d)\n", filename, last_offset, curr_data_block);

        // Now add the data into the existing data block
        char * data_block = (char *) malloc(BLOCKSIZE);
        read_block(data_block, curr_data_block);

        char * buffer = (char *)buf;
        for( int i = last_offset; i < last_offset + n; i++ ) { // Append the data into data block
            data_block[i] = buffer[i - last_offset];
        }

        // We can append to the end of the current block
        fcb_table->fcbs[fcb_index % MAX_ENTRY].last_item_offset += n;

        write_block(data_block, curr_data_block);
        free(data_block);
    }

    dir->entries[dir_entry_index % 32].file_size += n;

    // Save all unsaved changes
    write_block(index_block, iblock_index);
    write_block(dir, 1 + BITMAP_BLOCK_COUNT + (dir_entry_index / MAX_ENTRY));
    write_block(fcb_table, 1 + BITMAP_BLOCK_COUNT + FCB_BLOCK_COUNT + (fcb_index / MAX_ENTRY));

    free(fcb_table);
    free(index_block);
    free(dir);
    free(sb);
    return (0); 
}

int sfs_delete(char *filename) {
    printf("Deleting file \"%s\"...\n", filename);

    // Get the index block of the file
    struct Superblock * sb = (struct Superblock *) malloc(BLOCKSIZE);
    read_block(sb, 0);

    // Find directory entry with name 'filename' and update its value
    struct Directory* dir = (struct Directory *) malloc(BLOCKSIZE);

    int fcb_index = -1;
    int entry_iblock_index = -1; // Index inside index block
    int entry_block = -1; // Index of the entry block

    // Find the location of the file with name filename
    for( int i = 5; i < ROOT_BLOCK_COUNT + 5; i++ ) { // In each root directory block
        read_block(dir, i);
        for( int j = 0; j < MAX_ENTRY; j++ ) {
            if( strcmp(dir->entries[j].name, filename) == 0 ) {
                fcb_index = dir->entries[j].fcb_index;
                entry_block = i - 5;
                entry_iblock_index = j;
                break;
            }
        }

        if( fcb_index != -1) { break; }
    }

    if( fcb_index == -1 ) { printf("Error: This file does not exist.\n"); return -1; }

    // Delete from directory entries
    dir->entries[entry_iblock_index].name[0] = '\0';
    dir->entries[entry_iblock_index].file_size = -1;
    dir->entries[entry_iblock_index].fcb_index = -1;
    dir->entries[entry_iblock_index].mode = -1;

    // Delete the index block, corresponding data blocks and the bitmap
    struct FCBTable * fcb_table = (struct FCBTable *) malloc(BLOCKSIZE);
    read_block(fcb_table, 1 + BITMAP_BLOCK_COUNT + FCB_BLOCK_COUNT + (fcb_index / MAX_ENTRY));
    struct FCB fcb = fcb_table->fcbs[fcb_index % MAX_ENTRY]; // Get file's FCB
    int iblock_index = fcb.iblock_index; // Block number of index block

    if( fcb.iblock_index == -1 ) { // Index block DNE.
        printf("Error: No index block!\n"); return 0;
    }

    struct IndexBlock * index_block = (struct IndexBlock *) malloc(BLOCKSIZE);
    read_block(index_block, iblock_index);

    t_bitmap bm = create_bitmap(MAX_BITMAP_SIZE);

    int curr_bm_block = 0;
    read_block(bm, 1);

    // Get all used blocks and clear them
    for( int i = 0; i < fcb.used_block_count; i++ ) {
        if( i == MAX_BITMAP_SIZE ) {
            write_block(bm, 1 + curr_bm_block);
            curr_bm_block++;
            read_block(bm, curr_bm_block + 1);
        }

        int bm_i = ((int)index_block->ptr[i] % MAX_BITMAP_SIZE); // Bitmap block index
        bm_set_zero(bm, bm_i);
        index_block->ptr[i] = 0;
    }

    write_block(bm, 1 + curr_bm_block);

    read_block(bm, 1 + iblock_index / MAX_BITMAP_SIZE);
    bm_set_zero(bm, iblock_index % MAX_BITMAP_SIZE);

    write_block(bm, 1 + iblock_index / MAX_BITMAP_SIZE);

    //printf("\nBitmap After Delete:\n");
    //for( int i = 0; i < 100; i++ ) {
    //    printf("(%d: %d, )", i, get_bm_value(bm, i));
    //}
    //printf("\n");

    // DELETE THE FCB
    fcb_table->fcbs[fcb_index % MAX_ENTRY].used_block_count = 0;
    fcb_table->fcbs[fcb_index % MAX_ENTRY].iblock_index = -1;
    fcb_table->fcbs[fcb_index % MAX_ENTRY].used = 0;
    fcb_table->fcbs[fcb_index % MAX_ENTRY].last_item_offset = -1;
    fcb_table->fcbs[fcb_index % MAX_ENTRY].last_read_offset = -1;

    sb->curr_file_amt--;


    write_block(fcb_table, 1 + BITMAP_BLOCK_COUNT + ROOT_BLOCK_COUNT + (fcb_index / MAX_ENTRY));
    write_block(index_block, iblock_index);
    write_block(dir, 1 + BITMAP_BLOCK_COUNT + entry_block);
    write_block(sb, 0);

    free(fcb_table);
    free(index_block);
    free(dir);
    free(sb);
    free(bm);

    return (0); 
}
