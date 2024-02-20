# multithreaded-run-length-encoder
Multithreaded run-length encoding (RLE) utility in C for efficient data compression. Using pthreads for multithreading, this utility enhances the encoding process's speed, making it suitable for handling large files in parallel. Users can specify the number of threads to be used, optimizing performance based on the system's capabilities.

RLE: reducing sequences of the same element to a single value and count (eg. aaabbbbbcc -> a3b5c2)


