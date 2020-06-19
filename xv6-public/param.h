#define NPROC        64  // maximum number of processes
#define KSTACKSIZE 4096  // size of per-process kernel stack
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#define FSSIZE       40000  // size of file system in blocks
#define TIMEQUANTUM3  5   // length of time quantum in stride process (ticks)
#define TIMEQUANTUM2  5   // length of time quantum in level 2 queue (ticks)
#define TIMEQUANTUM1  10  // length of time quantum in level 1 queue (ticks)
#define TIMEQUANTUM0  20  // length of time quantum in level 0 queue (ticks)
#define TIMEALLOT2    20  // length of time allotment in level 2 queue (ticks)
#define TIMEALLOT1    40  // length of time allotment in level 1 queue (ticks)
