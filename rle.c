#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#define CHUNK_SIZE 4096
#define MAX_SIZE 1073741824

pthread_mutex_t task_q_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t task_q_not_empty = PTHREAD_COND_INITIALIZER;
pthread_mutex_t completed_q_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t completed_q_not_empty = PTHREAD_COND_INITIALIZER;

typedef struct {
    int task_id;
    char *start;
    int task_size;
    char *output;
    int output_size;
} Task;

typedef struct tasknode {
    Task *task;
    struct tasknode *next;
} TaskNode;

typedef struct {
    TaskNode *front;
    TaskNode *rear;
} Queue;

void q_init(Queue* q) {
    q->front = NULL;
    q->rear = NULL;
}

int is_q_empty(Queue* q) {
    return q->front == NULL;
}

void q_enqueue(Queue* q, Task *new_task) {
    TaskNode* new_node = (TaskNode*)malloc(sizeof(TaskNode));
    new_node->task = new_task;
    new_node->next = NULL;
    if (is_q_empty(q)) {
        q->front = new_node;
        q->rear = new_node;
    } else {
        q->rear->next = new_node;
        q->rear = new_node;
    }
}

Task* q_dequeue(Queue* q) {
    Task *next_task = q->front->task;
    TaskNode* temp_front = q->front;
    q->front = q->front->next;
    free(temp_front);
    if (q->front == NULL) {
        q->rear = NULL;
    }
    return next_task;
}

struct thread_args {
    Queue *task_q;
    Queue *completed_q;
};

void run_length_encode(char* input, int input_len, char* output, int* output_size_ptr){
    unsigned int count=0;
    char cur_char = input[0];
    int out_index = 0;

    for (int i=0; i<input_len; i++){
        if (input[i] != cur_char){
            output[out_index] = cur_char;
            out_index++;
            output[out_index] = count;
            out_index++;
            cur_char = input[i];
            count = 1;
        }
        else{
            count++;
        }
    }

    output[out_index] = cur_char;
    out_index++;
    output[out_index] = count;
    out_index++;
    *output_size_ptr = out_index;
}

void *worker_encode(void *worker_args){
    struct thread_args *args = worker_args;

    while(1){
        pthread_mutex_lock(&task_q_mutex);
        while (is_q_empty(args->task_q)){
            pthread_cond_wait(&task_q_not_empty, &task_q_mutex);
        }
        Task *task = q_dequeue(args->task_q);
        pthread_mutex_unlock(&task_q_mutex);

        run_length_encode(task->start, task->task_size, task->output, &task->output_size);

        pthread_mutex_lock(&completed_q_mutex);
        q_enqueue(args->completed_q, task);
        pthread_cond_signal(&completed_q_not_empty);
        pthread_mutex_unlock(&completed_q_mutex);
    }
}

void write_task_outputs(int output_fd, int task_id, char* last_char, unsigned int* last_count, char* output, int output_len){
    if (task_id == 0){
        write(output_fd, output, output_len-2);
        *last_char = output[output_len-2];
        *last_count = output[output_len-1];
    }
    else{
        if (*last_char == output[0]){
            output[1] = output[1] + *last_count;
            write(output_fd, output, output_len-2);
            *last_char = output[output_len-2];
            *last_count = output[output_len-1];
        }
        else{
            write(output_fd, last_char, 1);
            write(output_fd, last_count, 1);
            write(output_fd, output, output_len-2);
            *last_char = output[output_len-2];
            *last_count = output[output_len-1];
        }
    }
}

int main(int argc, char *argv[]){
    int output_fd = fileno(stdout), opt, num_threads=1;

    // Parse command line arguments - get number of threads
    int is_opt = 0;
    while ((opt = getopt(argc, argv, "j:")) != -1) {
        switch (opt) {
            case 'j':
                num_threads = atoi(optarg);
                is_opt = 1;
                break;
            default:
                fprintf(stderr, "Error: Invalid command.");
                exit(EXIT_FAILURE);
        }
    }

    // Create thread pool
    Queue task_q, completed_q;
    q_init(&task_q);
    q_init(&completed_q);

    struct thread_args worker_args = {&task_q, &completed_q};

    pthread_t threads[num_threads];
    for (int i=0; i<num_threads; i++){
        pthread_create(&threads[i], NULL, worker_encode, &worker_args);
    }
    
    char last_char;
    unsigned int last_count;
    int num_chunks_total = 0, task_id=0;

    for (int i = is_opt==1 ? 3 : 1; i<argc; i++){
        
        // Read input - Map input file
        int input_fd = open(argv[i], O_RDONLY);
        if(input_fd == -1){
            fprintf(stderr, "Error: Failed to open input file.");
            exit(EXIT_FAILURE);
        }
        struct stat sb;
        if(fstat(input_fd, &sb) == -1){
            fprintf(stderr, "Error: Failed to get file size.");
            exit(EXIT_FAILURE);
        }
        char *input = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, input_fd, 0);
        if(input == MAP_FAILED){
            fprintf(stderr, "Error: Failed to map file.");
            exit(EXIT_FAILURE);
        }

        // Create tasks and send to task_q (split input into chunks of 4096 bytes)
        int num_chunks = sb.st_size / CHUNK_SIZE + (sb.st_size % CHUNK_SIZE == 0 ? 0 : 1);

        for (int j=0; j<num_chunks; j++){
            Task *task = malloc(sizeof(Task));
            task->task_id = task_id;
            task_id++;
            task->task_size = (j == num_chunks-1 ? sb.st_size - j*CHUNK_SIZE : CHUNK_SIZE);
            task->start = input + j*CHUNK_SIZE;
            task->output = malloc(task->task_size * 2);
            pthread_mutex_lock(&task_q_mutex);
            q_enqueue(&task_q, task);
            pthread_cond_signal(&task_q_not_empty);
            pthread_mutex_unlock(&task_q_mutex);
        }

        num_chunks_total += num_chunks;
        close(input_fd);
    }

    // Write output to file. Write in order of task_id. Wait for task to be completed before writing.
    int write_index = 0;
    Task *to_write_arr[num_chunks_total];
    for (int k=0; k<num_chunks_total; k++){
        to_write_arr[k] = NULL;
    }

    while(write_index < num_chunks_total){
        
        if (to_write_arr[write_index] != NULL){
            Task *task_write = to_write_arr[write_index];
            write_task_outputs(output_fd, task_write->task_id, &last_char, &last_count, task_write->output, task_write->output_size);
            write_index++;
            continue;
        }

        pthread_mutex_lock(&completed_q_mutex);
        while(is_q_empty(&completed_q)){
            pthread_cond_wait(&completed_q_not_empty, &completed_q_mutex);
        }
        Task *completed_task = q_dequeue(&completed_q);
        pthread_mutex_unlock(&completed_q_mutex);
        
        to_write_arr[completed_task->task_id] = completed_task;
    }

    // Write last char and count of last task
    write(output_fd, &last_char, 1);
    write(output_fd, &last_count, 1);
    
    return 0;
}



/* References:
- Man pages
- lecutre notes
- https://stackoverflow.com/questions/6121094/how-do-i-run-a-program-with-commandline-arguments-using-gdb-within-a-bash-script
- https://www.geeksforgeeks.org/sprintf-in-c/
- https://stackoverflow.com/questions/20460670/reading-a-file-to-string-with-mmap
- https://linuxhint.com/using_mmap_function_linux/
- https://stackoverflow.com/questions/19823359/represent-unsigned-integer-in-binary
- https://www.geeksforgeeks.org/unsigned-char-in-c-with-examples/
- https://stackoverflow.com/questions/73619819/variable-has-incomplete-type-struct-stat
- https://www.geeksforgeeks.org/introduction-and-array-implementation-of-queue/
- https://man7.org/linux/man-pages/man2/lstat.2.html
- https://en.wikipedia.org/wiki/Pthreads
- https://en.wikipedia.org/wiki/Thread_pool
- https://nachtimwald.com/2019/04/12/thread-pool-in-c/
- https://man7.org/linux/man-pages/man3/pthread_create.3.html
- https://www.ibm.com/docs/en/zos/2.3.0?topic=functions-pthread-create-create-thread
- https://www.geeksforgeeks.org/getopt-function-in-c-to-parse-command-line-arguments/
- https://hpc-tutorials.llnl.gov/posix/passing_args/#:~:text=For%20cases%20where%20multiple%20arguments,and%20cast%20to%20(void%20*).
- https://stackoverflow.com/questions/1352749/multiple-arguments-to-function-called-by-pthread-create
- https://man7.org/linux/man-pages/man3/pthread_exit.3.html
- https://www.tutorialspoint.com/c_standard_library/c_function_memcpy.htm
*/