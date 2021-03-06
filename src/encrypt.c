/*
 *  @Author : Harsha vardhan Ghanta
 *  @email :  hghanta@andrew.cmu.edu,id
 *
 */


/**********************************************************************************************************************
 *
 *  The utility is a simple XOR crypto encryptor that can process large chunks of input data streamed in
 *  via STDIN taking advantage of parallelism existing on the host machine by spawning multiple threads.
 *  The output will be streamed back to STDOUT. All the errors are print to STRERR.
 *
 *  The error handling is done inside wrapper.c , All the potential system calls are wrapped inside wrapper function
 *  calls that start with Captial lettr so that the code looks cleaner.
 *
 *  The utility uses two levels of parallelism one between worker threads that does the actual work and the other
 *  between the printer thread . Shared buffers are used in both the cases to synchronize so that the output does not
 *  get clobbered .
 *
 *  The input will be split into chunks , whose size is determined by the length of the encryption key provided.
 *  Each chunk will be operated on with a differnet key that is obtained by left shifting the previous key by 1 bit.
 *  So the key repeats itself every N chunks , where N is the sie of the chunk .
 *
 *  @cmdline-arguments N  :Number of threads to spawn . ( Irrespective of the input , there will be an additonal thread
 *                         that runs as part of the design )
 *            -argument k : Absolute file path of the keyfile that will be used as encryption key.
 *
 **********************************************************************************************************************/


#include <stdio.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <pthread.h>
#include "../inc/encrypt.h"
#include "../inc/mypool.h"
#include "../inc/wrappers.h"

#define POOL_QUE_CNST 5
#define RIGHT_SHIFT_BITS 7

// constants
static const char *HELP_MESSAGE = " The usage encrypt -k keyfile.bin -n 1 < plain.bin > cypher.bin";
const size_t CHAR_SIZE= sizeof(char);
static const int BUFFER_CONSTANT = 4;
static const int PRINT_BUFFER_SIZE = 100;

// Global variables and forward declarations
static shared_buffer *shared_buff;
static void *xor_transform(thread_input *);
static void print_output();
static void key_left_shift(uint8_t *, long );
static void error_out();


int main(int argc, char **argv) {
    int c ;
    char *keyFile=NULL;
    int  num_threads=0;
    char *temp = Malloc(sizeof(char));


    while ((c = getopt(argc, argv, "k:N:d")) != EOF) {
        switch (c) {
            case 'N':           // Take number of threads as the input
                num_threads = (int) strtol(optarg, &temp, 10);
                break;
            case 'k':           // key file
                keyFile = Malloc(sizeof(char)*strlen(optarg));
                strncpy(keyFile,optarg,strlen(optarg));
                break;
            case 'h':           // print the help message
               printf("%s",HELP_MESSAGE);
                break;
            default:
                printf("%s",HELP_MESSAGE);
        }
    }

    if(num_threads == 0 || num_threads <0){
        fprintf(stderr," Number of threads should be greater than 0");
        exit(EXIT_FAILURE);
    }
    if(keyFile == NULL){
        fprintf(stderr,"Key file is mandatory , plese provide the path ");
        exit(EXIT_FAILURE);
    }
    FILE *key_file = Fopen(keyFile,"rb");

    // Find the size of the keyfile
    if(fseek(key_file,0L,SEEK_END) != 0){
        error_out();
    }
    long chunk_size = ftell(key_file);
    if(chunk_size == -1){
        error_out();
    }
    rewind(key_file);


    size_t buffer_size = (size_t) (BUFFER_CONSTANT * num_threads * chunk_size);
    // copy the key into local buffer or we can mmap .
    uint8_t key_buffer[chunk_size];
    Fread(key_buffer,CHAR_SIZE,chunk_size,key_file);
    fclose(key_file);

    // Create a thread pool to reduce the over head of thread creation on the fly.
    // The pool job que size is determined by the constant
    tpool *pool = threadpool_init(num_threads,num_threads *POOL_QUE_CNST);

    // Printer Thread : As writing to the output to disk / STDOUT is slow process and there is a chance the program
    // might get choked on waiting for the task , So creating a seperate thread that will consume the output
    // and writes to the screen at its own pace.
    pthread_t outputThread;
    Pthread_create(&outputThread, NULL, (void *(*)(void *)) print_output, NULL);

    // Shared buffer between the Printer thread and the main program
    shared_buff = Malloc(sizeof(shared_buffer));
    sharedbuffer_init(shared_buff,PRINT_BUFFER_SIZE);

    long bytes_read=0;
    // Check if input is stopped
    do {
        // Read more data than all of the treads can process at one time so that we can reduce the
        // number of read calls as they are expensive . So here we are reading buffer const X times of input data.
        char *work_buffer = Malloc(sizeof(char) * buffer_size);
        bytes_read= Fread(work_buffer,CHAR_SIZE,buffer_size,stdin);
        if(bytes_read==0) {
            Free(work_buffer);
            break;
        }
        // variable to keep track of the bytes allocated to various chunks
        size_t allocated_bytes=0;

        while(allocated_bytes < bytes_read){
            // Pack all the data that a worker thread might need to process a given chunk of input data
            // into a structure and insert into thread pool que.
            thread_input *tempStore = Malloc(sizeof(thread_input));
            if(allocated_bytes+chunk_size < bytes_read) {
                // offset at which this chunk begins
                tempStore->input=  work_buffer+allocated_bytes;
                tempStore->inputsize = chunk_size;
                allocated_bytes=allocated_bytes+chunk_size;
            }else{
                long left_over = bytes_read-allocated_bytes;
                tempStore->input= work_buffer+allocated_bytes;
                tempStore->inputsize = left_over;
                allocated_bytes += left_over;
            }
            // Copy the current key into the package for the chunk
            tempStore->key = Malloc(sizeof(char) * chunk_size);
            memcpy(tempStore->key,key_buffer,chunk_size);
            threadpool_add_work(pool, (void (*)(void *)) xor_transform, tempStore, false);
            // left shift the key  for the next chunk to use
            key_left_shift(key_buffer, chunk_size);
        }

        // wait for the read buffer worth of data to be completely processed
        // threadpool_wait will wait until all the worker threads finish their work
        threadpool_wait(pool);
        // pack the processed buffer of data into a struct and push it into a shared buffer with Printer thread .
        outpack *out = Malloc(sizeof(outpack));
        // buffer that is just processed
        out->buffer=work_buffer;
        out->end=false;
        out->size=allocated_bytes;
        sharebuffer_insert(shared_buff,out);

    }while(bytes_read != 0);

    // Signal the printer thread to exit
    outpack *end_signal = Malloc(sizeof(outpack));
    end_signal->size=0;
    end_signal->end=true;
    sharebuffer_insert(shared_buff,end_signal);
    Pthread_join(outputThread,NULL);
    sharedbuffer_free(shared_buff);
    return 0;
}

static void clean_up(){


}

/*
 * @brief Function that performs the actual encryption . It does this inplace . The output is placed
 * again in the same place as input
 *
 * @param  tip pointer to the chunk of data that the function should process
 */
static void *xor_transform(thread_input *tip){
    int i;
    for( i=0;i < (tip->inputsize) ; i++){
        tip->input[i] = tip->input[i] ^ (tip->key[i]);
    }
    Free(tip->key);
}

/*
 *  @brief This function runs on a different thread and reads the data that has to be printed from the shared
 *  buffer shared between main thread and itself. It acts like a consumer , as the buffer is FIFO discipliened
 *  output is not garbled.
 *
 *  reads data from the shared buffer
 *
 */

static void print_output() {
    while (1) {
        outpack *output = sharedbuffer_remove(shared_buff);
        if(output->end){
            Free(output);
            break;
        }
        Fwrite(output->buffer,CHAR_SIZE,output->size,stdout);
        Free(output->buffer);
        Free(output);
    }
}

/*
 * @brief This function turns the multi byte key handed over to it left by one bit .
 *
 * @param existing_key pointer to current key
 * @param size size of the key
 */
static void key_left_shift(uint8_t *existing_key, long size){

    int i;
    uint8_t shifted ;
    uint8_t overflow = (uint8_t) ((existing_key[0] >> (RIGHT_SHIFT_BITS)) & 0x1);
    for (i = (int) (size - 1); i >= 0 ; i--)
    {
        shifted = (existing_key[i] << 1) | overflow;
        overflow = (uint8_t) ((existing_key[i] >> (RIGHT_SHIFT_BITS)) & 0x1);
        existing_key[i] = shifted;
    }
}

static void error_out(){
    fprintf(stderr,"Error is %s:",strerror(errno));
    exit(EXIT_FAILURE);
}


