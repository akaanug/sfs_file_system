#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "simplefs.h"
#include <time.h>
#include <sys/time.h>

double time_delta(struct timeval x , struct timeval y) {
    double x_ms, y_ms, diff;

    x_ms = (double) x.tv_sec * 1000000 + (double) x.tv_usec;
    y_ms = (double) y.tv_sec * 1000000 + (double) y.tv_usec;

    diff = (double) x_ms - (double) y_ms;

    return diff / 1000;
}

// Write c to file write_amount times and calculate elapsed time.
void eval_append(char filename[110], int write_amount, char c) {
    // Open file
    int fd = sfs_open(filename, MODE_APPEND);

    struct timeval start, end;
    gettimeofday(&start, NULL);
    char buffer[1024];

    for( int i = 0; i < write_amount; i++ ) {
        buffer[0] =  c;
        sfs_append (fd, (void *) buffer, 1);
    }

    gettimeofday(&end, NULL);
    printf("\n\nElapsed time writing %d bytes into file \"%s\" = %f ms\n", write_amount, filename, time_delta(end, start));

    sfs_close(fd);
}

void eval_create(char filename[110]) {
    struct timeval start, end;
    gettimeofday(&start, NULL);

    printf("Creating file \"%s\"...\n", filename);
    sfs_create(filename);

    gettimeofday(&end, NULL);
    printf("\n\nElapsed time creating file \"%s\" = %f ms\n", filename, time_delta(end, start));
}

// batch_size = amount of bytes to read at a single call
void eval_read(char filename[110], int batch_size) {
    // Open file
    int fd = sfs_open(filename, MODE_READ);

    struct timeval start, end;
    gettimeofday(&start, NULL);

    int size = sfs_getsize(fd);
    char buffer[batch_size];

    char * all = (char *) malloc(size);

    for( int i = 0; i < size; i += batch_size ) {
        sfs_read(fd, (void *) buffer, batch_size);

        all[i] = (char) buffer[0];
    }

    gettimeofday(&end, NULL);
    printf("\n\nElapsed time reading %d bytes from file \"%s\" = %f ms\n", size, filename, time_delta(end, start));

    sfs_close(fd);
}

int main(int argc, char **argv) {
    int ret;
    char vdiskname[200]; 

    printf ("started\n");

    if (argc != 2) {
        printf ("usage: app  <vdiskname>\n");
        exit(0);
    }
    strcpy (vdiskname, argv[1]); 
    
    ret = sfs_mount (vdiskname);
    if (ret != 0) {
        printf ("could not mount \n");
        exit (1);
    }

    eval_create("file1.bin");
    eval_create("file2.bin");
    eval_create("file3.bin");

    eval_append("file1.bin", 10000, 'A');
    eval_append("file2.bin", 10000, 'B');
    eval_append("file3.bin", 10000, 'C');

    eval_read("file1.bin", 1);
    eval_read("file2.bin", 1);
    eval_read("file3.bin", 1);

    sfs_delete("file1.bin");

    eval_create("file1.bin");
    eval_append("file1.bin", 10000, 'A');
    eval_read("file1.bin", 1);

    ret = sfs_umount();

}
