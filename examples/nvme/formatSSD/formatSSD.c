#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "fs.h"
#define DATA_BUFFER_STRING "Hello world!"

struct ctrlr_entry {
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(ctrlr_entry)	link;
	char						name[1024];
};
struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns		*ns;
	TAILQ_ENTRY(ns_entry)	link;
	struct spdk_nvme_qpair	*qpair;
};

static TAILQ_HEAD(, ctrlr_entry) 	  g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) 		  g_namespaces  = TAILQ_HEAD_INITIALIZER(g_namespaces);
static struct spdk_nvme_transport_id  g_trid        = {};
static bool g_vmd = false;

struct ns_entry* mainNS;

static void register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	if (!spdk_nvme_ns_is_active(ns)) return;

	struct ns_entry *entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) { perror("ns_entry malloc"); exit(1); }
	entry->ctrlr = ctrlr;
	entry->ns = ns;
	
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns), spdk_nvme_ns_get_size(ns) / 1000000000);

	if (mainNS == NULL) mainNS = entry;
}
struct hello_world_sequence {
	struct ns_entry		*ns_entry;
	char				*buf;
	unsigned        	using_cmb_io;
	int					is_completed;
};
static void read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence *sequence = arg;

	sequence->is_completed = 1;  // Assume the I/O was successful
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
	if (strcmp(sequence->buf, DATA_BUFFER_STRING)) { fprintf(stderr, "Read data doesn't match write data\n"); exit(1); }

	printf("%s\n", sequence->buf);
	spdk_free(sequence->buf);
}
static void write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence	*sequence = arg;
	
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}

	struct ns_entry	*ns_entry = sequence->ns_entry;
	if (sequence->using_cmb_io) spdk_nvme_ctrlr_unmap_cmb(ns_entry->ctrlr);   // Free the buffer associated with the write I/O
	else                        spdk_free(sequence->buf);

	sequence->buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);   // allocate a new zeroed buffer for reading the data back from the NVMe namespace.
	int rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, sequence->buf, 0 /* LBA start */, 1 /* number of LBAs */, read_complete, (void *)sequence, 0);
	if (rc != 0) { fprintf(stderr, "starting read I/O failed\n"); exit(1); }
}
static void hello_world(void)
{
	struct ns_entry	*ns_entry;
	struct hello_world_sequence	sequence;
	size_t	sz;

	TAILQ_FOREACH(ns_entry, &g_namespaces, link) 
	{
		ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
		if (ns_entry->qpair == NULL) { printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n"); return; }


		sequence.using_cmb_io = 1;
		sequence.buf = spdk_nvme_ctrlr_map_cmb(ns_entry->ctrlr, &sz);
		if (sequence.buf == NULL || sz < 0x1000) 
		{
			sequence.using_cmb_io = 0;
			sequence.buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		}
		if (sequence.buf == NULL) { printf("ERROR: write buffer allocation failed\n"); return; }
		if (sequence.using_cmb_io) printf("INFO: using controller memory buffer for IO\n");
		else					   printf("INFO: using host memory buffer for IO\n");
		

		sequence.is_completed = 0;
		sequence.ns_entry = ns_entry;
		snprintf(sequence.buf, 0x1000, "%s", DATA_BUFFER_STRING);

		int rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair, sequence.buf, 0 /* LBA start */, 1 /* number of LBAs */, write_complete, &sequence, 0);
		if (rc != 0) { fprintf(stderr, "starting write I/O failed\n"); exit(1); }

		while (!sequence.is_completed) 
			spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);

		spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
	}
}
static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);
	return true;
}
static void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attached to %s\n", trid->traddr);

	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) { perror("ctrlr_entry malloc"); exit(1); }
	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	for (int nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) 
	{
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) continue;
		register_ns(ctrlr, ns);

		// printf("ID: %u\n", spdk_nvme_ns_get_id(ns));
		// printf("Is active: %s\n", spdk_nvme_ns_is_active(ns) ? "Yes" : "No");
		// printf("sector num: %lu\n", spdk_nvme_ns_get_num_sectors(ns));
		// printf("sector size: %u\n", spdk_nvme_ns_get_sector_size(ns));
		// printf("total: size: %lu\n", spdk_nvme_ns_get_size(ns));
	}
}
static void cleanup(void)
{
	struct ns_entry *ns_entry, *tmp_ns_entry;
	TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns_entry) 
	{
		TAILQ_REMOVE(&g_namespaces, ns_entry, link);
		free(ns_entry);
	}

	struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;
	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr_entry) 
	{
		TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	if (detach_ctx) spdk_nvme_detach_poll(detach_ctx);
}
static void usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\t\n");
	printf("options:\n");
	printf("\t[-d DPDK huge memory size in MB]\n");
	printf("\t[-g use single file descriptor for DPDK memory segments]\n");
	printf("\t[-i shared memory group ID]\n");
	printf("\t[-r remote NVMe over Fabrics target address]\n");
	printf("\t[-V enumerate VMD]\n");
#ifdef DEBUG
	printf("\t[-L enable debug logging]\n");
#else
	printf("\t[-L enable debug logging (flag disabled, must reconfigure with --enable-debug)]\n");
#endif
}
static int parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, rc;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "d:ghi:r:L:V")) != -1) {
		switch (op) {
		case 'V':
			g_vmd = true;
			break;
		case 'i':
			env_opts->shm_id = spdk_strtol(optarg, 10);
			if (env_opts->shm_id < 0) {
				fprintf(stderr, "Invalid shared memory ID\n");
				return env_opts->shm_id;
			}
			break;
		case 'g':
			env_opts->hugepage_single_segments = true;
			break;
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}
			break;
		case 'd':
			env_opts->mem_size = spdk_strtol(optarg, 10);
			if (env_opts->mem_size < 0) {
				fprintf(stderr, "Invalid DPDK memory size\n");
				return env_opts->mem_size;
			}
			break;
		case 'L':
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
			break;
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
}


void readObj(void* obj, size_t siz, uint64_t lba_start, uint32_t lba_count)
{
	mainNS->qpair = spdk_nvme_ctrlr_alloc_io_qpair(mainNS->ctrlr, NULL, 0);
	if (mainNS->qpair == NULL) { printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n"); return; }

	char *buf = spdk_zmalloc(lba_count * LBA_SIZE, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA); 
	if (buf == NULL) { printf("ERROR: read buffer allocation failed\n"); return; }

	int rc = spdk_nvme_ns_cmd_read(mainNS->ns, mainNS->qpair, buf, lba_start, lba_count, NULL, NULL, 0);
	if (rc != 0) { fprintf(stderr, "starting read I/O failed\n"); exit(1); }
	while (spdk_nvme_qpair_process_completions(mainNS->qpair, 0) != 1);
	printf("Read LBA[%ld~%ld] completed! ", lba_start, lba_start + lba_count - 1);
	memcpy(obj, buf, siz);

	spdk_nvme_ctrlr_free_io_qpair(mainNS->qpair);
	spdk_free(buf);
}
void writeObj(void* obj, size_t siz, uint64_t lba_start, uint32_t lba_count)   // obj为NULL时，初始化为全0
{
	mainNS->qpair = spdk_nvme_ctrlr_alloc_io_qpair(mainNS->ctrlr, NULL, 0);
	if (mainNS->qpair == NULL) { printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n"); return; }

	char *buf = spdk_zmalloc(siz, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (buf == NULL) { printf("ERROR: write buffer allocation failed\n"); return; }
	if (obj) memcpy(buf, obj, siz);

	int rc = spdk_nvme_ns_cmd_write(mainNS->ns, mainNS->qpair, buf, lba_start, lba_count, NULL, NULL, 0);
	if (rc != 0) { fprintf(stderr, "starting write I/O failed\n"); exit(1); }
	while (spdk_nvme_qpair_process_completions(mainNS->qpair, 0) != 1);
	// printf("Write LBA[%ld~%ld] completed!\n", lba_start, lba_start + lba_count - 1);

	spdk_nvme_ctrlr_free_io_qpair(mainNS->qpair);
	spdk_free(buf);
}

void print_bitmap(uint64_t *bm, int bits) 
{
    for (int i = 0; i < bits; i++) 
	{
        int idx = i / 64;              // 第几个 uint64_t
        int offset = i % 64;           // 在这个 uint64_t 中的第几位（低位是第0位）
        uint64_t mask = 1ULL << offset;
        printf("%d", (bm[idx] & mask) ? 1 : 0);

        if ((i + 1) % 64 == 0) printf("\n");  // 每 64 位换行一下
    }
}

void print_basic_info(void)
{
    printf("------------ About the Namespace ------------\n");
    printf("%-18s: %u\n",  "ID",                spdk_nvme_ns_get_id(mainNS->ns));
    printf("%-18s: %s\n",  "Is active",         spdk_nvme_ns_is_active(mainNS->ns) ? "Yes" : "No");
    printf("%-18s: %lu\n", "sector num",        spdk_nvme_ns_get_num_sectors(mainNS->ns));
    printf("%-18s: %u\n",  "sector size",       spdk_nvme_ns_get_sector_size(mainNS->ns));
    printf("%-18s: %lu\n", "total size",        spdk_nvme_ns_get_size(mainNS->ns));
	printf("\n");

    printf("------------ About the FS ------------------\n");
    printf("%-18s: %lu\n", "SuperBlock size",   sizeof(SuperBlock));
    printf("%-18s: %lu\n", "Inode size",        sizeof(Inode));
    printf("%-18s: %lu\n", "FreeBlockStk size", sizeof(FreeBlockStk));
}
void read_superblock(SuperBlock *sb)
{
	printf("Reading SuperBlock ...");
	readObj(sb, sizeof(SuperBlock), 0, 1);
	printf("END.\n");
}
void print_superblock(void)
{
	SuperBlock sb;
	read_superblock(&sb);

	printf("------ SuperBlock Information (%lu B): ------\n", sizeof(SuperBlock));
	printf("%-25s: 0x%x\n",  "magic_number",            sb.magic_number);
	printf("%-25s: %u\n",    "block_size",              sb.block_size);
	printf("%-25s: %lu\n",   "total_blocks_num",        sb.total_blocks_num);
	printf("%-25s: %lu\n",   "free_stk_len",            sb.free_stk_len);
	printf("%-25s: %lu\n",   "inode_size",              sb.inode_size);
	printf("%-25s: %lu\n",   "inode_num",               sb.inode_num);
	printf("%-25s: %lu\n",   "imap_block_start",        sb.imap_block_start);
	printf("%-25s: %lu\n",   "imap_block_num",          sb.imap_block_num);
	printf("%-25s: %lu\n",   "inode_table_block_start", sb.inode_table_block_start);
	printf("%-25s: %lu\n",   "inode_table_block_num",   sb.inode_table_block_num);
	printf("%-25s: %lu\n",   "free_stk_block_start",    sb.free_stk_block_start);
	printf("%-25s: %lu\n",   "free_stk2_block_start",   sb.free_stk_block2_start);
	printf("%-25s: %lu\n",   "free_stk_block_num",      sb.free_stk_block_num);
	printf("%-25s: %lu\n",   "data_block_start",        sb.data_block_start);
	printf("%-25s: %lu\n",   "data_block_num",          sb.data_block_num);
}
void read_imap(void)
{
	SuperBlock sb;
	read_superblock(&sb);

	printf("Reading Inode Bitmap...");
	u_int64_t bm[DIV_UP(INODENUM, 64)];
	readObj(bm, sizeof(bm), sb.imap_block_start, sb.imap_block_num);
	printf("END.\n");

	print_bitmap(bm, INODENUM);
}
void read_inode_table(void)
{
	SuperBlock sb;
	read_superblock(&sb);

	Inode inode_tbl[INODENUM];
	printf("%-30s", "Reading Inode Table...");
	readObj(inode_tbl, sizeof(Inode) * INODENUM, sb.inode_table_block_start, sb.inode_table_block_num);
	printf("END.\n");
	// for (int i = 0; i < 10; i++) 
	// {
	// 	printf("Inode[%d]:\n", i);
	// 	printf("\tidx: %lu\n", inode_tbl[i].idx);
	// 	printf("\tused: %d\n", inode_tbl[i].used);
	// }
}
void read_inode(int idx)
{
	SuperBlock sb;
	read_superblock(&sb);

	u_int64_t blockidx = sb.inode_table_block_start + idx / INODENUM_PER_BLOCK;
	Inode inode_tbl[INODENUM_PER_BLOCK];
	printf("Reading Inode %4d ...", idx);
	readObj(inode_tbl, sizeof(inode_tbl), blockidx, 1);
	printf("END.\n");

	int idx2 = idx % INODENUM_PER_BLOCK;
	printf("Inode[%d]:\n", idx);
	printf("\tidx: %lu\n", inode_tbl[idx2].idx);
	printf("\tused: %d\n", inode_tbl[idx2].used);
}
void read_free_stk(void)
{
	SuperBlock sb;
	read_superblock(&sb);

	FreeBlockStk stk;

	printf("Reading FreeBlockStk ...");
	readObj(&stk, sizeof(FreeBlockStk), sb.free_stk_block_start, sb.free_stk_block_num);
	printf("END.\n");
	printf("FreeBlockStk:\n");
	printf("\ttop = %d\n", stk.top);
	printf("\tFree Blocks: (top -> down)\n");
	for (int i = stk.top - 1; i >= 0; i--) 
	{
		printf("%10lu ", stk.blocks[i]);
		if ((stk.top - i) % 10 == 0) printf("\n");
	}
	printf("\n");


	printf("Reading FreeBlockStk2 ...");
	readObj(&stk, sizeof(FreeBlockStk), sb.free_stk_block2_start, sb.free_stk_block_num);
	printf("END.\n");
	printf("FreeBlockStk2:\n");
	printf("\ttop = %d\n", stk.top);
	printf("\tFree Blocks: (top -> down)\n");
	for (int i = stk.top - 1; i >= 0; i--) 
	{
		printf("%10lu ", stk.blocks[i]);
		if ((stk.top - i) % 10 == 0) printf("\n");
	}
	printf("\n");
}
void read_data_block_Group(u_int64_t LBA)
{
	SuperBlock sb;
	read_superblock(&sb);

	// assert((LBA - sb.data_block_start) % FREE_BLOCKS_PER_GROUP == 0);   // 必须是链接块

	nextGroup g;
	printf("Reading GroupBlock %lu ...", LBA);
	readObj(&g, sizeof(g), LBA, 1);
	printf("END.\n");

	printf("Group %lu:\n", LBA);
	printf("\tcnt: %d\n", g.cnt);
	printf("\tBlocks Num: (top -> down)\n");
	for (int i = g.cnt - 1; i >= 0; i--) 
	{
		printf("%10lu ", g.blocks[i]);
		if ((g.cnt - i) % 10 == 0) printf("\n");
	}
	printf("\n");
}

void InitSuperBlock(void)
{
	printf("\nInitializing SuperBlock... ");
	SuperBlock sb = {};
	sb.magic_number      = 0x0517;
	sb.block_size        = spdk_nvme_ns_get_sector_size(mainNS->ns);
	sb.total_blocks_num  = spdk_nvme_ns_get_num_sectors(mainNS->ns);

	sb.inode_size		 = sizeof(Inode);
	assert(sb.block_size % sb.inode_size == 0);
	sb.inode_num         = INODENUM;

	sb.free_stk_len      = FREE_BLOCKS_PER_GROUP;

	sb.imap_block_start = IMAP_START;
	sb.imap_block_num   = DIV_UP(BYTENUM(sb.inode_num), sb.block_size);

	sb.inode_table_block_start = sb.imap_block_start + sb.imap_block_num;
	sb.inode_table_block_num   = DIV_UP(sb.inode_num * sb.inode_size, sb.block_size);

	sb.free_stk_block_start  = sb.inode_table_block_start + sb.inode_table_block_num;
	sb.free_stk_block2_start = sb.free_stk_block_start + 1;
	sb.free_stk_block_num    = DIV_UP(sizeof(FreeBlockStk), sb.block_size);
	
	sb.data_block_start = sb.free_stk_block2_start + sb.free_stk_block_num;
	sb.data_block_num   = sb.total_blocks_num - sb.data_block_start;	
	
	writeObj(&sb, sizeof(SuperBlock), 0, 1);
	printf("Write LBA[%d~%d]. END.\n", 0, 0);
}
void InitInodeBitmap(void)
{
	SuperBlock sb;
	read_superblock(&sb);

	printf("\nInitializing Inode map... ");
	writeObj(NULL, sb.imap_block_num * sb.block_size, sb.imap_block_start, sb.imap_block_num); 
	printf("Write LBA[%ld~%ld]. END.\n\n", sb.imap_block_start, sb.imap_block_start + sb.imap_block_num - 1);
}
void InitInodeTable(void)
{
	SuperBlock sb;
	read_superblock(&sb);

	printf("\nInitializing Inode table... ");
	Inode *inode_table = calloc(sb.inode_num, sb.inode_size);
	if (!inode_table) { perror("Failed to allocate inode table"); return;}
	for (uint64_t i = 0; i < sb.inode_num; i++) 
	{
        inode_table[i].idx = i;
        inode_table[i].used = 0; // false
        // 其他字段全是0（calloc保证的）
    }
	writeObj(inode_table, sb.inode_num * sb.inode_size, sb.inode_table_block_start, sb.inode_table_block_num); 
	printf("Write LBA[%ld~%ld]. END.\n\n", sb.inode_table_block_start, sb.inode_table_block_start + sb.inode_table_block_num - 1);
}
void InitDataBlockGroup(void)
{
	SuperBlock sb;
	read_superblock(&sb);

	int tot = DIV_UP(sb.data_block_num, FREE_BLOCKS_PER_GROUP);

	printf("\nInitializing datablock group links...");
	nextGroup g = {.cnt = 0};
	int k = 0;
	for (u_int64_t i = FREE_BLOCKS_PER_GROUP; i < sb.data_block_num; i++)   // 从第2组开始 （第1组初始时即放入栈中）
	{
		g.blocks[i % FREE_BLOCKS_PER_GROUP] = sb.data_block_start + i;
		g.cnt++;
		if (g.cnt == FREE_BLOCKS_PER_GROUP) 
		{
			writeObj(&g, sizeof(g), sb.data_block_start + k * FREE_BLOCKS_PER_GROUP, 1);
			g.cnt = 0;
			k++;
			printf("\rInitializing datablock group links... %.2lf%%", 100.0 * k / tot);
			fflush(stdout);
		}
	}
	if (g.cnt > 0) writeObj(&g, sizeof(g), sb.data_block_start + k * FREE_BLOCKS_PER_GROUP, 1);
	printf("\rInitializing datablock group links... %.2lf%%", 100.0 * (++k) / tot);
	fflush(stdout);

	g.cnt = -1;
	uint64_t lastLBA = sb.total_blocks_num - sb.data_block_num % FREE_BLOCKS_PER_GROUP;  // 终止块
	writeObj(&g, sizeof(g), lastLBA, 1);
	printf(" END.\n\n");
}
void InitFreeBlockStk(void)
{
	SuperBlock sb;
	read_superblock(&sb);

	printf("Initializing Free Block Stack...\n");
	FreeBlockStk stk = {.top = 0};
	for (int i = 0; i < FREE_BLOCKS_PER_GROUP; i++)
	{
		stk.blocks[i] = sb.data_block_start + i;
		stk.top++;
	}
	writeObj(&stk, sizeof(stk), sb.free_stk_block_start, sb.free_stk_block_num);
	printf("Write LBA[%ld~%ld]. END.\n", sb.free_stk_block_start, sb.free_stk_block_start + sb.free_stk_block_num - 1);

	FreeBlockStk stk2 = {.top = 0};
	writeObj(&stk2, sizeof(stk2), sb.free_stk_block2_start, sb.free_stk_block_num);
	printf("Write LBA[%ld~%ld]. END.\n\n", sb.free_stk_block2_start, sb.free_stk_block2_start + sb.free_stk_block_num - 1);
}

static void format(void)
{
	InitSuperBlock();
	InitInodeBitmap();
	InitInodeTable();
	InitDataBlockGroup();
	InitFreeBlockStk();
	printf("Format SSD successfully!\n");
}


int main(int argc, char **argv)
{
	int rc;

	struct spdk_env_opts opts;
	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	rc = parse_args(argc, argv, &opts);
	if (rc != 0) return rc;
	opts.name = "hello_world";
	if (spdk_env_init(&opts) < 0) 
	{
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}


	printf("Initializing NVMe Controllers\n");
	if (g_vmd && spdk_vmd_init()) fprintf(stderr, "Failed to initialize VMD. Some NVMe devices can be unavailable.\n");
	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) 
	{
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
		goto exit;
	}
	if (TAILQ_EMPTY(&g_controllers)) 
	{
		fprintf(stderr, "no NVMe controllers found\n");
		rc = 1;
		goto exit;
	}
	printf("NVMe Controllers Initialization complete.\n");
	printf("---------------------------------------------------------------------\n\n");
	// hello_world();


	// print_basic_info();
	// format();

	// InitSuperBlock();
	// print_superblock();
	
	// InitInodeBitmap();
	// read_imap();

	// InitInodeTable();
	// read_inode_table();
	// read_inode(1010);

	// InitDataBlockGroup();
	// read_data_block_Group(937684525);

	// InitFreeBlockStk();
	// read_free_stk();

exit:
	fflush(stdout);
	cleanup();
	if (g_vmd) spdk_vmd_fini();
	spdk_env_fini();
	return rc;
}
