#include "spdk/stdinc.h"

#define INODENUM              1024
#define DIRECT_EXTENT_NUM     6
#define FREE_BLOCKS_PER_GROUP 511    // blocksize / 8 - 1
#define INODENUM_PER_BLOCK    16     // block_size / inode_size
#define IMAP_START            1      // 0号LBA是superblock
#define LBA_SIZE              4096
#define ROOT_INO              0
#define EXTENT_BLOCK_MIN_NUM  4

#define BYTENUM(n)   (((n) + 7) / 8)
#define DIV_UP(x, b) (((x) + (b) - 1) / (b))
#define MIN(m,n) ((m) < (n) ? (m) : (n))

typedef enum {
    UNKNOWN = 0,
    REGULAR,    // 普通文件
    DIRECTORY,  // 目录
    SYMLINK,    // 符号链接
} file_type_t;

typedef struct {
	uint64_t logical_start;
    uint64_t physical_start;
    uint64_t block_count;
} Extent;

typedef struct {
	uint64_t idx;
    uint8_t used;
	file_type_t type;   
	uint64_t file_size;
    Extent direct_extents[DIRECT_EXTENT_NUM];
	uint64_t indirect_extent_block;
	char pad[80];
} Inode; // 256B

typedef struct {
    int top;                        
    uint64_t blocks[FREE_BLOCKS_PER_GROUP];   // [0] 存下一组的块号
} FreeBlockStk;
typedef struct {
	int cnt;
	uint64_t blocks[FREE_BLOCKS_PER_GROUP];
} nextGroup;

typedef struct {
	uint32_t       magic_number;                 // 0x0517
   
	uint32_t       block_size;                   // 每块（LBA）的大小（B）
    uint64_t       total_blocks_num;             // 总块数

	uint64_t       inode_size;                   // inode 大小
	uint64_t       inode_num;                    // inode 数目

	uint64_t       free_stk_len;                 // free block stack 的容量

	uint64_t       imap_block_num;               // inode bitmap 占用的块数
	uint64_t       imap_block_start;             // inode bitmap 起始块

	uint64_t       free_stk_block_num;           // free stack 占用的块数
	uint64_t       free_stk_block_start;         // free stack （用于分配）起始块
	uint64_t       free_stk_block2_start;        // free stack2（用于回收）起始块
	
	uint64_t       inode_table_block_num;        // inode table 占用的块数
    uint64_t       inode_table_block_start;      // inode table 起始块

	uint64_t       root_inode;                   // root 目录对应的 inode 号
   
	uint64_t       data_block_num;               // data占用的块数
    uint64_t       data_block_start;             // data起始块
} SuperBlock;

#define DIRSIZ 32
typedef struct {
	uint64_t inum;
	char name[DIRSIZ];
	file_type_t filetype;
} Dirent;