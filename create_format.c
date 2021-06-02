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

int main(int argc, char **argv)
{
    int ret;
    char vdiskname[200];
    int m; 

    if (argc != 3) {
	printf ("usage: create_format <vdiskname> <m>\n"); 
	exit(1); 
    }

    strcpy (vdiskname, argv[1]); 
    m = atoi(argv[2]); 
    
    printf ("started\n");

    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    ret  = create_format_vdisk (vdiskname, m); 
    if (ret != 0) {
        printf ("there was an error in creating the disk\n");
        exit(1); 
    }

    gettimeofday(&end, NULL);
    printf("\n\nElapsed time creating disk %s with size %d = %f ms\n", vdiskname, 1 << m, time_delta(end, start));

    printf ("disk created and formatted. %s %d\n", vdiskname, m); 
}
