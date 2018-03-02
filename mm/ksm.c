/*
 * Memory merging support.
 *
 * This code enables dynamic sharing of identical pages found in different
 * memory areas, even if they are not shared by fork()
 *
 * Copyright (C) 2008-2009 Red Hat, Inc.
 * Authors:
 *	Izik Eidus
 *	Andrea Arcangeli
 *	Chris Wright
 *	Hugh Dickins
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 */

#include <linux/kvm_host.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/sched.h>
#include <linux/rwsem.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/spinlock.h>
#include <linux/jhash.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/memory.h>
#include <linux/mmu_notifier.h>
#include <linux/swap.h>
#include <linux/ksm.h>
#include <linux/hashtable.h>
#include <linux/freezer.h>
#include <linux/oom.h>
#include <linux/numa.h>
#include <linux/time.h>
#include <asm/tlbflush.h>
#include "internal.h"
#include "table.h"

#ifdef CONFIG_NUMA
#define NUMA(x)		(x)
#define DO_NUMA(x)	do { (x); } while (0)
#else
#define NUMA(x)		(0)
#define DO_NUMA(x)	do { } while (0)
#endif

/*
 * A few notes about the KSM scanning process,
 * to make it easier to understand the data structures below:
 *
 * In order to reduce excessive scanning, KSM sorts the memory pages by their
 * contents into a data structure that holds pointers to the pages' locations.
 *
 * Since the contents of the pages may change at any moment, KSM cannot just
 * insert the pages into a normal sorted tree and expect it to find anything.
 * Therefore KSM uses two data structures - the stable and the unstable tree.
 *
 * The stable tree holds pointers to all the merged pages (ksm pages), sorted
 * by their contents.  Because each such page is write-protected, searching on
 * this tree is fully assured to be working (except when pages are unmapped),
 * and therefore this tree is called the stable tree.
 *
 * In addition to the stable tree, KSM uses a second data structure called the
 * unstable tree: this tree holds pointers to pages which have been found to
 * be "unchanged for a period of time".  The unstable tree sorts these pages
 * by their contents, but since they are not write-protected, KSM cannot rely
 * upon the unstable tree to work correctly - the unstable tree is liable to
 * be corrupted as its contents are modified, and so it is called unstable.
 *
 * KSM solves this problem by several techniques:
 *
 * 1) The unstable tree is flushed every time KSM completes scanning all
 *    memory areas, and then the tree is rebuilt again from the beginning.
 * 2) KSM will only insert into the unstable tree, pages whose hash value
 *    has not changed since the previous scan of all memory areas.
 * 3) The unstable tree is a RedBlack Tree - so its balancing is based on the
 *    colors of the nodes and not on their contents, assuring that even when
 *    the tree gets "corrupted" it won't get out of balance, so scanning time
 *    remains the same (also, searching and inserting nodes in an rbtree uses
 *    the same algorithm, so we have no overhead when we flush and rebuild).
 * 4) KSM never flushes the stable tree, which means that even if it were to
 *    take 10 attempts to find a page in the unstable tree, once it is found,
 *    it is secured in the stable tree.  (When we scan a new page, we first
 *    compare it against the stable tree, and then against the unstable tree.)
 *
 * If the merge_across_nodes tunable is unset, then KSM maintains multiple
 * stable trees and multiple unstable trees: one of each for each NUMA node.
 */

/**
 * struct mm_slot - ksm information per mm that is being scanned
 * @link: link to the mm_slots hash list
 * @mm_list: link into the mm_slots list, rooted in ksm_mm_head
 * @rmap_list: head for this mm_slot's singly-linked list of rmap_items
 * @mm: the mm that this information is valid for
 */
struct mm_slot {
	struct hlist_node link;
	struct list_head mm_list;
	struct rmap_item *rmap_list;
	struct mm_struct *mm;
};

/**
 * struct ksm_scan - cursor for scanning
 * @mm_slot: the current mm_slot we are scanning
 * @address: the next address inside that to be scanned
 * @rmap_list: link to the next rmap to be scanned in the rmap_list
 * @seqnr: count of completed full scans (needed when removing unstable node)
 *
 * There is only the one ksm_scan instance of this cursor structure.
 */
struct ksm_scan {
	struct mm_slot *mm_slot;
	unsigned long address;
	struct rmap_item **rmap_list;
	unsigned long seqnr;
};

/**
 * struct stable_node - node of the stable rbtree
 * @node: rb node of this ksm page in the stable tree
 * @head: (overlaying parent) &migrate_nodes indicates temporarily on that list
 * @list: linked into migrate_nodes, pending placement in the proper node tree
 * @hlist: hlist head of rmap_items using this ksm page
 * @kpfn: page frame number of this ksm page (perhaps temporarily on wrong nid)
 * @nid: NUMA node id of stable tree in which linked (may not match kpfn)
 */
struct stable_node {
	union {
		struct rb_node node;	/* when node of stable tree */
		struct {		/* when listed for migration */
			struct list_head *head;
			struct list_head list;
		};
	};
	struct hlist_head hlist;
	unsigned long kpfn;
#ifdef CONFIG_NUMA
	int nid;
#endif
};

/* hz ksm */
struct gpa_node {
	struct list_head link;
	unsigned long gfn;
	struct hlist_head hlist;
	int count;
	int sum;
	int in_hot_zone;
};

/* hz ksm */
struct hotzone {
	unsigned long gfn;
	int vm1;
	int vm2;
	int sum;
	int count;
	struct rb_node node;
};

/**
 * struct rmap_item - reverse mapping item for virtual addresses
 * @rmap_list: next rmap_item in mm_slot's singly-linked rmap_list
 * @anon_vma: pointer to anon_vma for this mm,address, when in stable tree
 * @nid: NUMA node id of unstable tree in which linked (may not match page)
 * @mm: the memory structure this rmap_item is pointing into
 * @address: the virtual address this rmap_item tracks (+ flags in low bits)
 * @oldchecksum: previous checksum of the page at that virtual address
 * @node: rb node of this rmap_item in the unstable tree
 * @head: pointer to stable_node heading this list in the stable tree
 * @hlist: link into hlist of rmap_items hanging off that stable_node
 */
struct rmap_item {
	struct rmap_item *rmap_list;

	int number; /*which vm own this page*/
	unsigned long gfn;

	union {
		struct anon_vma *anon_vma;	/* when stable */
#ifdef CONFIG_NUMA
		int nid;		/* when node of unstable tree */
#endif
	};
	struct mm_struct *mm;
	unsigned long address;		/* + low bits used for flags below */
	unsigned int oldchecksum;	/* when unstable */
	union {
		struct rb_node node;	/* when node of unstable tree */
		struct {		/* when listed from stable tree */
			struct stable_node *head;
			struct hlist_node hlist;
		};
	};
	struct hlist_node gfnhlist;    /* link into hlist of rmap_items hanging off that gpa_node */
	int scanned;
	struct list_head list;
};

static struct rb_root hot_zone[1] = { RB_ROOT };
static struct rb_root *hot_zone_table = hot_zone;
/*for hot zone*/
#define hotzone_count 300000
int hotzone_used = 0;
int define_prescan = 0;
int scan_hot_zone = 0;
int scan_remain = 0;
int scan_none = 0;

/*For time analysis*/
struct timeval before, after;
unsigned long ksm_time = 0;
unsigned long break_time = 0;
int time_flag = 0;

int hit = 0; 
int skip = 0;

int print_vma = 0;

#define SEQNR_MASK	0x0ff	/* low bits of unstable tree seqnr */
#define UNSTABLE_FLAG	0x100	/* is a node of the unstable tree */
#define STABLE_FLAG	0x200	/* is listed from the stable tree */

/* The stable and unstable tree heads */
static struct rb_root one_stable_tree[1] = { RB_ROOT };
static struct rb_root one_unstable_tree[1] = { RB_ROOT };
static struct rb_root *root_stable_tree = one_stable_tree;
static struct rb_root *root_unstable_tree = one_unstable_tree;

/* Recently migrated nodes of stable tree, pending proper placement */
static LIST_HEAD(migrate_nodes);

#define MM_SLOTS_HASH_BITS 10
static DEFINE_HASHTABLE(mm_slots_hash, MM_SLOTS_HASH_BITS);

static struct mm_slot ksm_mm_head = {
	.mm_list = LIST_HEAD_INIT(ksm_mm_head.mm_list),
};

/* ------------------ hz_ksm ------------------ */
/*head for gpa node*/
static LIST_HEAD(gpa_node_head1);
static LIST_HEAD(gpa_node_head1a);
static LIST_HEAD(gpa_node_head1b);
static LIST_HEAD(gpa_node_head1c);
static LIST_HEAD(gpa_node_head2);
static LIST_HEAD(gpa_node_head2a);
static LIST_HEAD(gpa_node_head2b);
static LIST_HEAD(gpa_node_head2c);
static LIST_HEAD(gpa_node_head3);
static LIST_HEAD(gpa_node_head3a);
static LIST_HEAD(gpa_node_head3b);
static LIST_HEAD(gpa_node_head3c);
static LIST_HEAD(gpa_node_head4);
static LIST_HEAD(gpa_node_head4a);
static LIST_HEAD(gpa_node_head4b);
static LIST_HEAD(gpa_node_head4c);
static LIST_HEAD(gpa_node_head5);
static LIST_HEAD(gpa_node_head5a);
static LIST_HEAD(gpa_node_head5b);
static LIST_HEAD(gpa_node_head5c);
static LIST_HEAD(gpa_node_head6);
static LIST_HEAD(gpa_node_head6a);
static LIST_HEAD(gpa_node_head6b);
static LIST_HEAD(gpa_node_head6c);
static LIST_HEAD(gpa_node_head7);
static LIST_HEAD(gpa_node_head7a);
static LIST_HEAD(gpa_node_head7b);
static LIST_HEAD(gpa_node_head7c);
static LIST_HEAD(gpa_node_head8);
static LIST_HEAD(gpa_node_head8a);
static LIST_HEAD(gpa_node_head8b);
static LIST_HEAD(gpa_node_head8c);
static LIST_HEAD(gpa_node_head9);
static LIST_HEAD(gpa_node_head9a);
static LIST_HEAD(gpa_node_head9b);
static LIST_HEAD(gpa_node_head9c);
static LIST_HEAD(gpa_node_head10);
static LIST_HEAD(gpa_node_head10a);
static LIST_HEAD(gpa_node_head10b);
static LIST_HEAD(gpa_node_head10c);
static LIST_HEAD(gpa_node_head11);
static LIST_HEAD(gpa_node_head11a);
static LIST_HEAD(gpa_node_head11b);
static LIST_HEAD(gpa_node_head11c);
static LIST_HEAD(gpa_node_head12);
static LIST_HEAD(gpa_node_head12a);
static LIST_HEAD(gpa_node_head12b);
static LIST_HEAD(gpa_node_head12c);
static LIST_HEAD(gpa_node_head13);
static LIST_HEAD(gpa_node_head13a);
static LIST_HEAD(gpa_node_head13b);
static LIST_HEAD(gpa_node_head13c);
static LIST_HEAD(gpa_node_head14);
static LIST_HEAD(gpa_node_head14a);
static LIST_HEAD(gpa_node_head14b);
static LIST_HEAD(gpa_node_head14c);
static LIST_HEAD(gpa_node_head15);
static LIST_HEAD(gpa_node_head15a);
static LIST_HEAD(gpa_node_head15b);
static LIST_HEAD(gpa_node_head15c);
static LIST_HEAD(gpa_node_head16);
static LIST_HEAD(gpa_node_head16a);
static LIST_HEAD(gpa_node_head16b);
static LIST_HEAD(gpa_node_head16c);
static LIST_HEAD(gpa_node_head17);
static LIST_HEAD(gpa_node_head17a);
static LIST_HEAD(gpa_node_head17b);
static LIST_HEAD(gpa_node_head17c);
static LIST_HEAD(gpa_node_head18);
static LIST_HEAD(gpa_node_head18a);
static LIST_HEAD(gpa_node_head18b);
static LIST_HEAD(gpa_node_head18c);
static LIST_HEAD(gpa_node_head19);
static LIST_HEAD(gpa_node_head19a);
static LIST_HEAD(gpa_node_head19b);
static LIST_HEAD(gpa_node_head19c);
static LIST_HEAD(gpa_node_head20);
static LIST_HEAD(gpa_node_head20a);
static LIST_HEAD(gpa_node_head20b);
static LIST_HEAD(gpa_node_head20c);

/* head for rmap_item list (vm3, vm4, ...) */
static LIST_HEAD(hot_zone_node);
static LIST_HEAD(rest_gpa_node_head);

/* unused in current version */
// static LIST_HEAD(none_node); //if a rmap_item's gpa is 0, adding it to this list
// static LIST_HEAD(rmap_item_head);
/*if a rmap_item is in hot zone, adding it to this list*/
// static LIST_HEAD(hot_zone_list);
/*static struct gpa_node gpa_node_head = {
	.link = LIST_HEAD_INIT(gpa_node_head.link),
};*/

/* --------------------------------------------------- */

static struct ksm_scan ksm_scan = {
	.mm_slot = &ksm_mm_head,
};

static struct kmem_cache *rmap_item_cache;
static struct kmem_cache *stable_node_cache;
static struct kmem_cache *mm_slot_cache;
static struct kmem_cache *gpa_node_cache; /*hz_ksm*/

/* The number of nodes in the stable tree */
static unsigned long ksm_pages_shared;

/* The number of page slots additionally sharing those nodes */
static unsigned long ksm_pages_sharing;

/* The number of nodes in the unstable tree */
static unsigned long ksm_pages_unshared;

/* The number of rmap_items in use: to calculate pages_volatile */
static unsigned long ksm_rmap_items;

/* Number of pages ksmd should scan in one batch */
static unsigned int ksm_thread_pages_to_scan = 100;

/* Milliseconds ksmd should sleep between batches */
static unsigned int ksm_thread_sleep_millisecs = 20;

#ifdef CONFIG_NUMA
/* Zeroed when merging across nodes is not allowed */
static unsigned int ksm_merge_across_nodes = 1;
static int ksm_nr_node_ids = 1;
#else
#define ksm_merge_across_nodes	1U
#define ksm_nr_node_ids		1
#endif

#define KSM_RUN_STOP	0
#define KSM_RUN_MERGE	1
#define KSM_RUN_UNMERGE	2
#define KSM_RUN_OFFLINE	4
static unsigned long ksm_run = KSM_RUN_STOP;
static void wait_while_offlining(void);

static DECLARE_WAIT_QUEUE_HEAD(ksm_thread_wait);
static DEFINE_MUTEX(ksm_thread_mutex);
static DEFINE_SPINLOCK(ksm_mmlist_lock);

#define KSM_KMEM_CACHE(__struct, __flags) kmem_cache_create("ksm_"#__struct,\
		sizeof(struct __struct), __alignof__(struct __struct),\
		(__flags), NULL)

//static struct file *filp = NULL;
/*
static struct file *open_file(char *filename)
{
	struct file *filp;
	mm_segment_t old_fs = get_fs();
	set_fs(KERNEL_DS);

	filp = filp_open(filename, O_CREAT | O_RDWR, 0644);
	if(IS_ERR(filp))
		printk("open error\n");
	else
		printk("open success\n");

	set_fs(old_fs);
	return filp;
}

static void write_file(struct file *filp, unsigned long long hva, unsigned long long gpa, int number)
{
	mm_segment_t old_fs = get_fs();
	set_fs(KERNEL_DS);
	char buf1[500], buf2[100], buf3[3] = "\n", blank[3] = " ", buf4[50];
	sprintf(buf1, "%lu", hva);
	strcat(buf1, blank);
	sprintf(buf2, "%llu", gpa);
	strcat(buf1, buf2);
	strcat(buf1, blank);
	sprintf(buf4, "%d", number);
	strcat(buf1, buf4);
	strcat(buf1, buf3);
	vfs_write(filp, buf1, strlen(buf1), &filp->f_pos);

	set_fs(old_fs);
}

static void handle_file(struct file *filpr, unsigned long scan_round, unsigned long long hva, unsigned long long gpa, int number)
{
	if(filpr == NULL)
	{
		printk("open file!\n");
		char filename[1000] = "/home/tsai-te-yu/ksmresult/out";
		char txt[6] = ".txt";
		char round[50];
		sprintf(round, "%lu", scan_round);
		strcat(filename, round);
		strcat(filename, txt);
		filp = open_file(filename);
		write_file(filp, hva, gpa, number);
	}
	else
		write_file(filp, hva, gpa, number);

}
*/

static int __init ksm_slab_init(void)
{
	rmap_item_cache = KSM_KMEM_CACHE(rmap_item, 0);
	if (!rmap_item_cache)
		goto out;

	stable_node_cache = KSM_KMEM_CACHE(stable_node, 0);
	if (!stable_node_cache)
		goto out_free1;

	mm_slot_cache = KSM_KMEM_CACHE(mm_slot, 0);
	if (!mm_slot_cache)
		goto out_free2;
	/* hz ksm */
	gpa_node_cache = KSM_KMEM_CACHE(gpa_node, 0);
	if(!gpa_node_cache)
		goto out_free3;

	return 0;

out_free3:
	kmem_cache_destroy(gpa_node_cache);
out_free2:
	kmem_cache_destroy(stable_node_cache);
out_free1:
	kmem_cache_destroy(rmap_item_cache);
out:
	return -ENOMEM;
}

static void __init ksm_slab_free(void)
{
	kmem_cache_destroy(mm_slot_cache);
	kmem_cache_destroy(stable_node_cache);
	kmem_cache_destroy(rmap_item_cache);
	/* hz ksm */
	kmem_cache_destroy(gpa_node_cache);
	mm_slot_cache = NULL;
}

static inline struct rmap_item *alloc_rmap_item(void)
{
	struct rmap_item *rmap_item;

	rmap_item = kmem_cache_zalloc(rmap_item_cache, GFP_KERNEL |
						__GFP_NORETRY | __GFP_NOWARN);
	if (rmap_item)
		ksm_rmap_items++;
	return rmap_item;
}

static inline void free_rmap_item(struct rmap_item *rmap_item)
{
	ksm_rmap_items--;
	rmap_item->mm = NULL;	/* debug safety */
	kmem_cache_free(rmap_item_cache, rmap_item);
}

static inline struct stable_node *alloc_stable_node(void)
{
	return kmem_cache_alloc(stable_node_cache, GFP_KERNEL);
}

static inline void free_stable_node(struct stable_node *stable_node)
{
	kmem_cache_free(stable_node_cache, stable_node);
}

static inline struct mm_slot *alloc_mm_slot(void)
{
	if (!mm_slot_cache)	/* initialization failed */
		return NULL;
	return kmem_cache_zalloc(mm_slot_cache, GFP_KERNEL);
}

static inline void free_mm_slot(struct mm_slot *mm_slot)
{
	kmem_cache_free(mm_slot_cache, mm_slot);
}

/* HZ KSM */
static inline struct gpa_node *alloc_gpa_node(void)
{
	return kmem_cache_zalloc(gpa_node_cache, GFP_KERNEL);
}

static inline void free_gpa_node(struct gpa_node *gpa_node)
{
	kmem_cache_free(gpa_node_cache, gpa_node);
}

static struct mm_slot *get_mm_slot(struct mm_struct *mm)
{
	struct mm_slot *slot;

	hash_for_each_possible(mm_slots_hash, slot, link, (unsigned long)mm)
		if (slot->mm == mm)
			return slot;

	return NULL;
}

static void insert_to_mm_slots_hash(struct mm_struct *mm,
				    struct mm_slot *mm_slot)
{
	mm_slot->mm = mm;
	hash_add(mm_slots_hash, &mm_slot->link, (unsigned long)mm);
}

/*
 * ksmd, and unmerge_and_remove_all_rmap_items(), must not touch an mm's
 * page tables after it has passed through ksm_exit() - which, if necessary,
 * takes mmap_sem briefly to serialize against them.  ksm_exit() does not set
 * a special flag: they can just back out as soon as mm_users goes to zero.
 * ksm_test_exit() is used throughout to make this test for exit: in some
 * places for correctness, in some places just to avoid unnecessary work.
 */
static inline bool ksm_test_exit(struct mm_struct *mm)
{
	return atomic_read(&mm->mm_users) == 0;
}

/*
 * We use break_ksm to break COW on a ksm page: it's a stripped down
 *
 *	if (get_user_pages(current, mm, addr, 1, 1, 1, &page, NULL) == 1)
 *		put_page(page);
 *
 * but taking great care only to touch a ksm page, in a VM_MERGEABLE vma,
 * in case the application has unmapped and remapped mm,addr meanwhile.
 * Could a ksm page appear anywhere else?  Actually yes, in a VM_PFNMAP
 * mmap of /dev/mem or /dev/kmem, where we would not want to touch it.
 */
static int break_ksm(struct vm_area_struct *vma, unsigned long addr)
{
	struct page *page;
	int ret = 0;

	do {
		cond_resched();
		page = follow_page(vma, addr, FOLL_GET | FOLL_MIGRATION);
		if (IS_ERR_OR_NULL(page))
			break;
		if (PageKsm(page))
			ret = handle_mm_fault(vma->vm_mm, vma, addr,
							FAULT_FLAG_WRITE);
		else
			ret = VM_FAULT_WRITE;
		put_page(page);
	} while (!(ret & (VM_FAULT_WRITE | VM_FAULT_SIGBUS | VM_FAULT_SIGSEGV | VM_FAULT_OOM)));
	/*
	 * We must loop because handle_mm_fault() may back out if there's
	 * any difficulty e.g. if pte accessed bit gets updated concurrently.
	 *
	 * VM_FAULT_WRITE is what we have been hoping for: it indicates that
	 * COW has been broken, even if the vma does not permit VM_WRITE;
	 * but note that a concurrent fault might break PageKsm for us.
	 *
	 * VM_FAULT_SIGBUS could occur if we race with truncation of the
	 * backing file, which also invalidates anonymous pages: that's
	 * okay, that truncation will have unmapped the PageKsm for us.
	 *
	 * VM_FAULT_OOM: at the time of writing (late July 2009), setting
	 * aside mem_cgroup limits, VM_FAULT_OOM would only be set if the
	 * current task has TIF_MEMDIE set, and will be OOM killed on return
	 * to user; and ksmd, having no mm, would never be chosen for that.
	 *
	 * But if the mm is in a limited mem_cgroup, then the fault may fail
	 * with VM_FAULT_OOM even if the current task is not TIF_MEMDIE; and
	 * even ksmd can fail in this way - though it's usually breaking ksm
	 * just to undo a merge it made a moment before, so unlikely to oom.
	 *
	 * That's a pity: we might therefore have more kernel pages allocated
	 * than we're counting as nodes in the stable tree; but ksm_do_scan
	 * will retry to break_cow on each pass, so should recover the page
	 * in due course.  The important thing is to not let VM_MERGEABLE
	 * be cleared while any such pages might remain in the area.
	 */
	return (ret & VM_FAULT_OOM) ? -ENOMEM : 0;
}

static struct vm_area_struct *find_mergeable_vma(struct mm_struct *mm,
		unsigned long addr)
{
	struct vm_area_struct *vma;
	if (ksm_test_exit(mm))
		return NULL;
	vma = find_vma(mm, addr);
	if (!vma || vma->vm_start > addr)
		return NULL;
	if (!(vma->vm_flags & VM_MERGEABLE) || !vma->anon_vma)
		return NULL;
	return vma;
}

static void break_cow(struct rmap_item *rmap_item)
{
	struct mm_struct *mm = rmap_item->mm;
	unsigned long addr = rmap_item->address;
	struct vm_area_struct *vma;

	/*
	 * It is not an accident that whenever we want to break COW
	 * to undo, we also need to drop a reference to the anon_vma.
	 */
	put_anon_vma(rmap_item->anon_vma);

	down_read(&mm->mmap_sem);
	vma = find_mergeable_vma(mm, addr);
	if (vma)
		break_ksm(vma, addr);
	up_read(&mm->mmap_sem);
}

static struct page *page_trans_compound_anon(struct page *page)
{
	if (PageTransCompound(page)) {
		struct page *head = compound_head(page);
		/*
		 * head may actually be splitted and freed from under
		 * us but it's ok here.
		 */
		if (PageAnon(head))
			return head;
	}
	return NULL;
}

static struct page *get_mergeable_page(struct rmap_item *rmap_item)
{
	struct mm_struct *mm = rmap_item->mm;
	unsigned long addr = rmap_item->address;
	struct vm_area_struct *vma;
	struct page *page;

	down_read(&mm->mmap_sem);
	vma = find_mergeable_vma(mm, addr);
	if (!vma)
		goto out;

	page = follow_page(vma, addr, FOLL_GET);
	if (IS_ERR_OR_NULL(page))
		goto out;
	if (PageAnon(page) || page_trans_compound_anon(page)) {
		flush_anon_page(vma, page, addr);
		flush_dcache_page(page);
	} else {
		put_page(page);
out:
		page = NULL;
	}
	up_read(&mm->mmap_sem);
	return page;
}

/*
 * This helper is used for getting right index into array of tree roots.
 * When merge_across_nodes knob is set to 1, there are only two rb-trees for
 * stable and unstable pages from all nodes with roots in index 0. Otherwise,
 * every node has its own stable and unstable tree.
 */
static inline int get_kpfn_nid(unsigned long kpfn)
{
	return ksm_merge_across_nodes ? 0 : NUMA(pfn_to_nid(kpfn));
}

static void remove_node_from_stable_tree(struct stable_node *stable_node)
{
	struct rmap_item *rmap_item;

	hlist_for_each_entry(rmap_item, &stable_node->hlist, hlist) {
		if (rmap_item->hlist.next)
			ksm_pages_sharing--;
		else
			ksm_pages_shared--;
		put_anon_vma(rmap_item->anon_vma);
		rmap_item->address &= PAGE_MASK;
		cond_resched();
	}

	if (stable_node->head == &migrate_nodes)
		list_del(&stable_node->list);
	else
		rb_erase(&stable_node->node,
			 root_stable_tree + NUMA(stable_node->nid));
	free_stable_node(stable_node);
}

/*
 * get_ksm_page: checks if the page indicated by the stable node
 * is still its ksm page, despite having held no reference to it.
 * In which case we can trust the content of the page, and it
 * returns the gotten page; but if the page has now been zapped,
 * remove the stale node from the stable tree and return NULL.
 * But beware, the stable node's page might be being migrated.
 *
 * You would expect the stable_node to hold a reference to the ksm page.
 * But if it increments the page's count, swapping out has to wait for
 * ksmd to come around again before it can free the page, which may take
 * seconds or even minutes: much too unresponsive.  So instead we use a
 * "keyhole reference": access to the ksm page from the stable node peeps
 * out through its keyhole to see if that page still holds the right key,
 * pointing back to this stable node.  This relies on freeing a PageAnon
 * page to reset its page->mapping to NULL, and relies on no other use of
 * a page to put something that might look like our key in page->mapping.
 * is on its way to being freed; but it is an anomaly to bear in mind.
 */
static struct page *get_ksm_page(struct stable_node *stable_node, bool lock_it)
{
	struct page *page;
	void *expected_mapping;
	unsigned long kpfn;

	expected_mapping = (void *)stable_node +
				(PAGE_MAPPING_ANON | PAGE_MAPPING_KSM);
again:
	kpfn = READ_ONCE(stable_node->kpfn);
	page = pfn_to_page(kpfn);

	/*
	 * page is computed from kpfn, so on most architectures reading
	 * page->mapping is naturally ordered after reading node->kpfn,
	 * but on Alpha we need to be more careful.
	 */
	smp_read_barrier_depends();
	if (READ_ONCE(page->mapping) != expected_mapping)
		goto stale;

	/*
	 * We cannot do anything with the page while its refcount is 0.
	 * Usually 0 means free, or tail of a higher-order page: in which
	 * case this node is no longer referenced, and should be freed;
	 * however, it might mean that the page is under page_freeze_refs().
	 * The __remove_mapping() case is easy, again the node is now stale;
	 * but if page is swapcache in migrate_page_move_mapping(), it might
	 * still be our page, in which case it's essential to keep the node.
	 */
	while (!get_page_unless_zero(page)) {
		/*
		 * Another check for page->mapping != expected_mapping would
		 * work here too.  We have chosen the !PageSwapCache test to
		 * optimize the common case, when the page is or is about to
		 * be freed: PageSwapCache is cleared (under spin_lock_irq)
		 * in the freeze_refs section of __remove_mapping(); but Anon
		 * page->mapping reset to NULL later, in free_pages_prepare().
		 */
		if (!PageSwapCache(page))
			goto stale;
		cpu_relax();
	}

	if (READ_ONCE(page->mapping) != expected_mapping) {
		put_page(page);
		goto stale;
	}

	if (lock_it) {
		lock_page(page);
		if (READ_ONCE(page->mapping) != expected_mapping) {
			unlock_page(page);
			put_page(page);
			goto stale;
		}
	}
	return page;

stale:
	/*
	 * We come here from above when page->mapping or !PageSwapCache
	 * suggests that the node is stale; but it might be under migration.
	 * We need smp_rmb(), matching the smp_wmb() in ksm_migrate_page(),
	 * before checking whether node->kpfn has been changed.
	 */
	smp_rmb();
	if (READ_ONCE(stable_node->kpfn) != kpfn)
		goto again;
	remove_node_from_stable_tree(stable_node);
	return NULL;
}

/*
 * Removing rmap_item from stable or unstable tree.
 * This function will clean the information from the stable/unstable tree.
 */
static void remove_rmap_item_from_tree(struct rmap_item *rmap_item)
{
	if (rmap_item->address & STABLE_FLAG) {
		struct stable_node *stable_node;
		struct page *page;

		stable_node = rmap_item->head;
		page = get_ksm_page(stable_node, true);
		if (!page)
			goto out;

		hlist_del(&rmap_item->hlist);
		unlock_page(page);
		put_page(page);

		if (!hlist_empty(&stable_node->hlist))
			ksm_pages_sharing--;
		else
			ksm_pages_shared--;

		put_anon_vma(rmap_item->anon_vma);
		rmap_item->address &= PAGE_MASK;

	} else if (rmap_item->address & UNSTABLE_FLAG) {
		unsigned char age;
		/*
		 * Usually ksmd can and must skip the rb_erase, because
		 * root_unstable_tree was already reset to RB_ROOT.
		 * But be careful when an mm is exiting: do the rb_erase
		 * if this rmap_item was inserted by this scan, rather
		 * than left over from before.
		 */
		age = (unsigned char)(ksm_scan.seqnr - rmap_item->address);
		/* for debugging purpose */
		/*
		if(age > 1) {
			printk("BEFORE: round = %lu, address = %x, gfn = %lu, age = %x\n",
			 ksm_scan.seqnr, rmap_item->address, rmap_item->gfn, age);

			 rmap_item->address |= (ksm_scan.seqnr & SEQNR_MASK);
			 age = (unsigned char)(ksm_scan.seqnr - rmap_item->address);

			 printk("After: round = %lu, address = %x, gfn = %lu, age = %x\n",
			 ksm_scan.seqnr, rmap_item->address, rmap_item->gfn, age);
		}
		else if(!age) {
			printk("Normal status: round = %lu, address = %x\n", ksm_scan.seqnr, rmap_item->address);
		}
		*/
		BUG_ON(age > 1);
		if (!age)
			rb_erase(&rmap_item->node,
				 root_unstable_tree + NUMA(rmap_item->nid));
		ksm_pages_unshared--;
		rmap_item->address &= PAGE_MASK;
	}
out:
	cond_resched();		/* we're called from many long loops */
}

static void remove_trailing_rmap_items(struct mm_slot *mm_slot,
				       struct rmap_item **rmap_list)
{
	while (*rmap_list) {
		struct rmap_item *rmap_item = *rmap_list;
		*rmap_list = rmap_item->rmap_list;	/* rmap_list = rmap_item->next */
		remove_rmap_item_from_tree(rmap_item);
		free_rmap_item(rmap_item);
	}
}

/*
 * Though it's very tempting to unmerge rmap_items from stable tree rather
 * than check every pte of a given vma, the locking doesn't quite work for
 * that - an rmap_item is assigned to the stable tree after inserting ksm
 * page and upping mmap_sem.  Nor does it fit with the way we skip dup'ing
 * rmap_items from parent to child at fork time (so as not to waste time
 * if exit comes before the next scan reaches it).
 *
 * Similarly, although we'd like to remove rmap_items (so updating counts
 * and freeing memory) when unmerging an area, it's easier to leave that
 * to the next pass of ksmd - consider, for example, how ksmd might be
 * in cmp_and_merge_page on one of the rmap_items we would be removing.
 */
static int unmerge_ksm_pages(struct vm_area_struct *vma,
			     unsigned long start, unsigned long end)
{
	unsigned long addr;
	int err = 0;

	for (addr = start; addr < end && !err; addr += PAGE_SIZE) {
		if (ksm_test_exit(vma->vm_mm))
			break;
		if (signal_pending(current))
			err = -ERESTARTSYS;
		else
			err = break_ksm(vma, addr);
	}
	return err;
}

#ifdef CONFIG_SYSFS
/*
 * Only called through the sysfs control interface:
 */
static int remove_stable_node(struct stable_node *stable_node)
{
	struct page *page;
	int err;

	page = get_ksm_page(stable_node, true);
	if (!page) {
		/*
		 * get_ksm_page did remove_node_from_stable_tree itself.
		 */
		return 0;
	}

	if (WARN_ON_ONCE(page_mapped(page))) {
		/*
		 * This should not happen: but if it does, just refuse to let
		 * merge_across_nodes be switched - there is no need to panic.
		 */
		err = -EBUSY;
	} else {
		/*
		 * The stable node did not yet appear stale to get_ksm_page(),
		 * since that allows for an unmapped ksm page to be recognized
		 * right up until it is freed; but the node is safe to remove.
		 * This page might be in a pagevec waiting to be freed,
		 * or it might be PageSwapCache (perhaps under writeback),
		 * or it might have been removed from swapcache a moment ago.
		 */
		set_page_stable_node(page, NULL);
		remove_node_from_stable_tree(stable_node);
		err = 0;
	}

	unlock_page(page);
	put_page(page);
	return err;
}

static int remove_all_stable_nodes(void)
{
	struct stable_node *stable_node;
	struct list_head *this, *next;
	int nid;
	int err = 0;

	for (nid = 0; nid < ksm_nr_node_ids; nid++) {
		while (root_stable_tree[nid].rb_node) {
			stable_node = rb_entry(root_stable_tree[nid].rb_node,
						struct stable_node, node);
			if (remove_stable_node(stable_node)) {
				err = -EBUSY;
				break;	/* proceed to next nid */
			}
			cond_resched();
		}
	}
	list_for_each_safe(this, next, &migrate_nodes) {
		stable_node = list_entry(this, struct stable_node, list);
		if (remove_stable_node(stable_node))
			err = -EBUSY;
		cond_resched();
	}
	return err;
}

static int unmerge_and_remove_all_rmap_items(void)
{
	struct mm_slot *mm_slot;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int err = 0;

	spin_lock(&ksm_mmlist_lock);
	ksm_scan.mm_slot = list_entry(ksm_mm_head.mm_list.next,
						struct mm_slot, mm_list);
	spin_unlock(&ksm_mmlist_lock);

	for (mm_slot = ksm_scan.mm_slot;
			mm_slot != &ksm_mm_head; mm_slot = ksm_scan.mm_slot) {
		mm = mm_slot->mm;
		down_read(&mm->mmap_sem);
		for (vma = mm->mmap; vma; vma = vma->vm_next) {
			if (ksm_test_exit(mm))
				break;
			if (!(vma->vm_flags & VM_MERGEABLE) || !vma->anon_vma)
				continue;
			err = unmerge_ksm_pages(vma,
						vma->vm_start, vma->vm_end);
			if (err)
				goto error;
		}

		remove_trailing_rmap_items(mm_slot, &mm_slot->rmap_list);

		spin_lock(&ksm_mmlist_lock);
		ksm_scan.mm_slot = list_entry(mm_slot->mm_list.next,
						struct mm_slot, mm_list);
		if (ksm_test_exit(mm)) {
			hash_del(&mm_slot->link);
			list_del(&mm_slot->mm_list);
			spin_unlock(&ksm_mmlist_lock);

			free_mm_slot(mm_slot);
			clear_bit(MMF_VM_MERGEABLE, &mm->flags);
			up_read(&mm->mmap_sem);
			mmdrop(mm);
		} else {
			spin_unlock(&ksm_mmlist_lock);
			up_read(&mm->mmap_sem);
		}
	}

	/* Clean up stable nodes, but don't worry if some are still busy */
	remove_all_stable_nodes();
	ksm_scan.seqnr = 0;
	return 0;

error:
	up_read(&mm->mmap_sem);
	spin_lock(&ksm_mmlist_lock);
	ksm_scan.mm_slot = &ksm_mm_head;
	spin_unlock(&ksm_mmlist_lock);
	return err;
}
#endif /* CONFIG_SYSFS */

static u32 calc_checksum(struct page *page)
{
	u32 checksum;
	void *addr = kmap_atomic(page);
	checksum = jhash2(addr, PAGE_SIZE / 4, 17);
	kunmap_atomic(addr);
	return checksum;
}

static int memcmp_pages(struct page *page1, struct page *page2)
{
	char *addr1, *addr2;
	int ret;

	addr1 = kmap_atomic(page1);
	addr2 = kmap_atomic(page2);
	ret = memcmp(addr1, addr2, PAGE_SIZE);
	kunmap_atomic(addr2);
	kunmap_atomic(addr1);
	return ret;
}

static inline int pages_identical(struct page *page1, struct page *page2)
{
	return !memcmp_pages(page1, page2);
}

static int write_protect_page(struct vm_area_struct *vma, struct page *page,
			      pte_t *orig_pte)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long addr;
	pte_t *ptep;
	spinlock_t *ptl;
	int swapped;
	int err = -EFAULT;
	unsigned long mmun_start;	/* For mmu_notifiers */
	unsigned long mmun_end;		/* For mmu_notifiers */

	addr = page_address_in_vma(page, vma);
	if (addr == -EFAULT)
		goto out;

	BUG_ON(PageTransCompound(page));

	mmun_start = addr;
	mmun_end   = addr + PAGE_SIZE;
	mmu_notifier_invalidate_range_start(mm, mmun_start, mmun_end);

	ptep = page_check_address(page, mm, addr, &ptl, 0);
	if (!ptep)
		goto out_mn;

	if (pte_write(*ptep) || pte_dirty(*ptep)) {
		pte_t entry;

		swapped = PageSwapCache(page);
		flush_cache_page(vma, addr, page_to_pfn(page));
		/*
		 * Ok this is tricky, when get_user_pages_fast() run it doesn't
		 * take any lock, therefore the check that we are going to make
		 * with the pagecount against the mapcount is racey and
		 * O_DIRECT can happen right after the check.
		 * So we clear the pte and flush the tlb before the check
		 * this assure us that no O_DIRECT can happen after the check
		 * or in the middle of the check.
		 */
		entry = ptep_clear_flush_notify(vma, addr, ptep);
		/*
		 * Check that no O_DIRECT or similar I/O is in progress on the
		 * page
		 */
		if (page_mapcount(page) + 1 + swapped != page_count(page)) {
			set_pte_at(mm, addr, ptep, entry);
			goto out_unlock;
		}
		if (pte_dirty(entry))
			set_page_dirty(page);
		entry = pte_mkclean(pte_wrprotect(entry));
		set_pte_at_notify(mm, addr, ptep, entry);
	}
	*orig_pte = *ptep;
	err = 0;

out_unlock:
	pte_unmap_unlock(ptep, ptl);
out_mn:
	mmu_notifier_invalidate_range_end(mm, mmun_start, mmun_end);
out:
	return err;
}

/**
 * replace_page - replace page in vma by new ksm page
 * @vma:      vma that holds the pte pointing to page
 * @page:     the page we are replacing by kpage
 * @kpage:    the ksm page we replace page by
 * @orig_pte: the original value of the pte
 *
 * Returns 0 on success, -EFAULT on failure.
 */
static int replace_page(struct vm_area_struct *vma, struct page *page,
			struct page *kpage, pte_t orig_pte)
{
	struct mm_struct *mm = vma->vm_mm;
	pmd_t *pmd;
	pte_t *ptep;
	spinlock_t *ptl;
	unsigned long addr;
	int err = -EFAULT;
	unsigned long mmun_start;	/* For mmu_notifiers */
	unsigned long mmun_end;		/* For mmu_notifiers */

	addr = page_address_in_vma(page, vma);
	if (addr == -EFAULT)
		goto out;

	pmd = mm_find_pmd(mm, addr);
	if (!pmd)
		goto out;

	mmun_start = addr;
	mmun_end   = addr + PAGE_SIZE;
	mmu_notifier_invalidate_range_start(mm, mmun_start, mmun_end);

	ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!pte_same(*ptep, orig_pte)) {
		pte_unmap_unlock(ptep, ptl);
		goto out_mn;
	}

	get_page(kpage);
	page_add_anon_rmap(kpage, vma, addr);

	flush_cache_page(vma, addr, pte_pfn(*ptep));
	ptep_clear_flush_notify(vma, addr, ptep);
	set_pte_at_notify(mm, addr, ptep, mk_pte(kpage, vma->vm_page_prot));

	page_remove_rmap(page);
	if (!page_mapped(page))
		try_to_free_swap(page);
	put_page(page);

	pte_unmap_unlock(ptep, ptl);
	err = 0;
out_mn:
	mmu_notifier_invalidate_range_end(mm, mmun_start, mmun_end);
out:
	return err;
}

static int page_trans_compound_anon_split(struct page *page)
{
	int ret = 0;
	struct page *transhuge_head = page_trans_compound_anon(page);
	if (transhuge_head) {
		/* Get the reference on the head to split it. */
		if (get_page_unless_zero(transhuge_head)) {
			/*
			 * Recheck we got the reference while the head
			 * was still anonymous.
			 */
			if (PageAnon(transhuge_head))
				ret = split_huge_page(transhuge_head);
			else
				/*
				 * Retry later if split_huge_page run
				 * from under us.
				 */
				ret = 1;
			put_page(transhuge_head);
		} else
			/* Retry later if split_huge_page run from under us. */
			ret = 1;
	}
	return ret;
}

/*
 * try_to_merge_one_page - take two pages and merge them into one
 * @vma: the vma that holds the pte pointing to page
 * @page: the PageAnon page that we want to replace with kpage
 * @kpage: the PageKsm page that we want to map instead of page,
 *         or NULL the first time when we want to use page as kpage.
 *
 * This function returns 0 if the pages were merged, -EFAULT otherwise.
 */
static int try_to_merge_one_page(struct vm_area_struct *vma,
				 struct page *page, struct page *kpage)
{
	pte_t orig_pte = __pte(0);
	int err = -EFAULT;

	if (page == kpage)			/* ksm page forked */
		return 0;

	if (PageTransCompound(page) && page_trans_compound_anon_split(page))
		goto out;
	BUG_ON(PageTransCompound(page));
	if (!PageAnon(page))
		goto out;

	/*
	 * We need the page lock to read a stable PageSwapCache in
	 * write_protect_page().  We use trylock_page() instead of
	 * lock_page() because we don't want to wait here - we
	 * prefer to continue scanning and merging different pages,
	 * then come back to this page when it is unlocked.
	 */
	if (!trylock_page(page))
		goto out;
	/*
	 * If this anonymous page is mapped only here, its pte may need
	 * to be write-protected.  If it's mapped elsewhere, all of its
	 * ptes are necessarily already write-protected.  But in either
	 * case, we need to lock and check page_count is not raised.
	 */
	if (write_protect_page(vma, page, &orig_pte) == 0) {
		if (!kpage) {
			/*
			 * While we hold page lock, upgrade page from
			 * PageAnon+anon_vma to PageKsm+NULL stable_node:
			 * stable_tree_insert() will update stable_node.
			 */
			set_page_stable_node(page, NULL);
			mark_page_accessed(page);
			err = 0;
		} else if (pages_identical(page, kpage))
			err = replace_page(vma, page, kpage, orig_pte);
	}

	if ((vma->vm_flags & VM_LOCKED) && kpage && !err) {
		munlock_vma_page(page);
		if (!PageMlocked(kpage)) {
			unlock_page(page);
			lock_page(kpage);
			mlock_vma_page(kpage);
			page = kpage;		/* for final unlock */
		}
	}

	unlock_page(page);
out:
	return err;
}

/*
 * try_to_merge_with_ksm_page - like try_to_merge_two_pages,
 * but no new kernel page is allocated: kpage must already be a ksm page.
 *
 * This function returns 0 if the pages were merged, -EFAULT otherwise.
 */
static int try_to_merge_with_ksm_page(struct rmap_item *rmap_item,
				      struct page *page, struct page *kpage)
{
	struct mm_struct *mm = rmap_item->mm;
	struct vm_area_struct *vma;
	int err = -EFAULT;

	down_read(&mm->mmap_sem);
	vma = find_mergeable_vma(mm, rmap_item->address);
	if (!vma)
		goto out;

	err = try_to_merge_one_page(vma, page, kpage);
	if (err)
		goto out;

	/* Unstable nid is in union with stable anon_vma: remove first */
	remove_rmap_item_from_tree(rmap_item);

	/* Must get reference to anon_vma while still holding mmap_sem */
	rmap_item->anon_vma = vma->anon_vma;
	get_anon_vma(vma->anon_vma);
out:
	up_read(&mm->mmap_sem);
	return err;
}

/*
 * try_to_merge_two_pages - take two identical pages and prepare them
 * to be merged into one page.
 *
 * This function returns the kpage if we successfully merged two identical
 * pages into one ksm page, NULL otherwise.
 *
 * Note that this function upgrades page to ksm page: if one of the pages
 * is already a ksm page, try_to_merge_with_ksm_page should be used.
 */
static struct page *try_to_merge_two_pages(struct rmap_item *rmap_item,
					   struct page *page,
					   struct rmap_item *tree_rmap_item,
					   struct page *tree_page)
{
	int err;

	err = try_to_merge_with_ksm_page(rmap_item, page, NULL);
	if (!err) {
		err = try_to_merge_with_ksm_page(tree_rmap_item,
							tree_page, page);
		/*
		 * If that fails, we have a ksm page with only one pte
		 * pointing to it: so break it.
		 */
		if (err)
			break_cow(rmap_item);
	}
	return err ? NULL : page;
}

/*
 * stable_tree_search - search for page inside the stable tree
 *
 * This function checks if there is a page inside the stable tree
 * with identical content to the page that we are scanning right now.
 *
 * This function returns the stable tree node of identical content if found,
 * NULL otherwise.
 */
static struct page *stable_tree_search(struct page *page)
{
	int nid;
	struct rb_root *root;
	struct rb_node **new;
	struct rb_node *parent;
	struct stable_node *stable_node;
	struct stable_node *page_node;

	page_node = page_stable_node(page);
	if (page_node && page_node->head != &migrate_nodes) {
		/* ksm page forked */
		get_page(page);
		return page;
	}

	nid = get_kpfn_nid(page_to_pfn(page));
	root = root_stable_tree + nid;
again:
	new = &root->rb_node;
	parent = NULL;

	while (*new) {
		struct page *tree_page;
		int ret;

		cond_resched();
		stable_node = rb_entry(*new, struct stable_node, node);
		tree_page = get_ksm_page(stable_node, false);
		if (!tree_page) {
			/*
			 * If we walked over a stale stable_node,
			 * get_ksm_page() will call rb_erase() and it
			 * may rebalance the tree from under us. So
			 * restart the search from scratch. Returning
			 * NULL would be safe too, but we'd generate
			 * false negative insertions just because some
			 * stable_node was stale.
			 */
			goto again;
		}

		ret = memcmp_pages(page, tree_page);
		put_page(tree_page);

		parent = *new;
		if (ret < 0)
			new = &parent->rb_left;
		else if (ret > 0)
			new = &parent->rb_right;
		else {
			/*
			 * Lock and unlock the stable_node's page (which
			 * might already have been migrated) so that page
			 * migration is sure to notice its raised count.
			 * It would be more elegant to return stable_node
			 * than kpage, but that involves more changes.
			 */
			tree_page = get_ksm_page(stable_node, true);
			if (tree_page) {
				unlock_page(tree_page);
				if (get_kpfn_nid(stable_node->kpfn) !=
						NUMA(stable_node->nid)) {
					put_page(tree_page);
					goto replace;
				}
				return tree_page;
			}
			/*
			 * There is now a place for page_node, but the tree may
			 * have been rebalanced, so re-evaluate parent and new.
			 */
			if (page_node)
				goto again;
			return NULL;
		}
	}

	if (!page_node)
		return NULL;

	list_del(&page_node->list);
	DO_NUMA(page_node->nid = nid);
	rb_link_node(&page_node->node, parent, new);
	rb_insert_color(&page_node->node, root);
	get_page(page);
	return page;

replace:
	if (page_node) {
		list_del(&page_node->list);
		DO_NUMA(page_node->nid = nid);
		rb_replace_node(&stable_node->node, &page_node->node, root);
		get_page(page);
	} else {
		rb_erase(&stable_node->node, root);
		page = NULL;
	}
	stable_node->head = &migrate_nodes;
	list_add(&stable_node->list, stable_node->head);
	return page;
}

/*
 * stable_tree_insert - insert stable tree node pointing to new ksm page
 * into the stable tree.
 *
 * This function returns the stable tree node just allocated on success,
 * NULL otherwise.
 */
static struct stable_node *stable_tree_insert(struct page *kpage)
{
	int nid;
	unsigned long kpfn;
	struct rb_root *root;
	struct rb_node **new;
	struct rb_node *parent;
	struct stable_node *stable_node;

	kpfn = page_to_pfn(kpage);
	nid = get_kpfn_nid(kpfn);
	root = root_stable_tree + nid;
again:
	parent = NULL;
	new = &root->rb_node;

	while (*new) {
		struct page *tree_page;
		int ret;

		cond_resched();
		stable_node = rb_entry(*new, struct stable_node, node);
		tree_page = get_ksm_page(stable_node, false);
		if (!tree_page) {
			/*
			 * If we walked over a stale stable_node,
			 * get_ksm_page() will call rb_erase() and it
			 * may rebalance the tree from under us. So
			 * restart the search from scratch. Returning
			 * NULL would be safe too, but we'd generate
			 * false negative insertions just because some
			 * stable_node was stale.
			 */
			goto again;
		}

		ret = memcmp_pages(kpage, tree_page);
		put_page(tree_page);

		parent = *new;
		if (ret < 0)
			new = &parent->rb_left;
		else if (ret > 0)
			new = &parent->rb_right;
		else {
			/*
			 * It is not a bug that stable_tree_search() didn't
			 * find this node: because at that time our page was
			 * not yet write-protected, so may have changed since.
			 */
			return NULL;
		}
	}

	stable_node = alloc_stable_node();
	if (!stable_node)
		return NULL;

	INIT_HLIST_HEAD(&stable_node->hlist);
	stable_node->kpfn = kpfn;
	set_page_stable_node(kpage, stable_node);
	DO_NUMA(stable_node->nid = nid);
	rb_link_node(&stable_node->node, parent, new);
	rb_insert_color(&stable_node->node, root);

	return stable_node;
}

/*
 * unstable_tree_search_insert - search for identical page,
 * else insert rmap_item into the unstable tree.
 *
 * This function searches for a page in the unstable tree identical to the
 * page currently being scanned; and if no identical page is found in the
 * tree, we insert rmap_item as a new object into the unstable tree.
 *
 * This function returns pointer to rmap_item found to be identical
 * to the currently scanned page, NULL otherwise.
 *
 * This function does both searching and inserting, because they share
 * the same walking algorithm in an rbtree.
 */
static
struct rmap_item *unstable_tree_search_insert(struct rmap_item *rmap_item,
					      struct page *page,
					      struct page **tree_pagep)
{
	struct rb_node **new;
	struct rb_root *root;
	struct rb_node *parent = NULL;
	int nid;

	nid = get_kpfn_nid(page_to_pfn(page));
	root = root_unstable_tree + nid;
	new = &root->rb_node;

	while (*new) {
		struct rmap_item *tree_rmap_item;
		struct page *tree_page;
		int ret;

		cond_resched();
		tree_rmap_item = rb_entry(*new, struct rmap_item, node);
		tree_page = get_mergeable_page(tree_rmap_item);
		if (!tree_page)
			return NULL;

		/*
		 * Don't substitute a ksm page for a forked page.
		 */
		if (page == tree_page) {
			put_page(tree_page);
			return NULL;
		}

		ret = memcmp_pages(page, tree_page);

		parent = *new;
		if (ret < 0) {
			put_page(tree_page);
			new = &parent->rb_left;
		} else if (ret > 0) {
			put_page(tree_page);
			new = &parent->rb_right;
		} else if (!ksm_merge_across_nodes &&
			   page_to_nid(tree_page) != nid) {
			/*
			 * If tree_page has been migrated to another NUMA node,
			 * it will be flushed out and put in the right unstable
			 * tree next time: only merge with it when across_nodes.
			 */
			put_page(tree_page);
			return NULL;
		} else {
			*tree_pagep = tree_page;
			return tree_rmap_item;
		}
	}

	rmap_item->address |= UNSTABLE_FLAG;
	rmap_item->address |= (ksm_scan.seqnr & SEQNR_MASK);
	DO_NUMA(rmap_item->nid = nid);
	rb_link_node(&rmap_item->node, parent, new);
	rb_insert_color(&rmap_item->node, root);

	ksm_pages_unshared++;
	return NULL;
}

/*
 * stable_tree_append - add another rmap_item to the linked list of
 * rmap_items hanging off a given node of the stable tree, all sharing
 * the same ksm page.
 */
static void stable_tree_append(struct rmap_item *rmap_item,
			       struct stable_node *stable_node)
{
	rmap_item->head = stable_node;
	rmap_item->address |= STABLE_FLAG;
	hlist_add_head(&rmap_item->hlist, &stable_node->hlist);

	if (rmap_item->hlist.next)
		ksm_pages_sharing++;
	else
		ksm_pages_shared++;
}

// HZ ksm
int hz_size = 0;	/* hz_size = hotzone size(in one round) */
static void vm12_record(struct rmap_item *rmap_item)
{
	struct rb_root *root;
	struct rb_node **new;
	struct rb_node *parent = NULL;
	struct hotzone *hotzone;

	root = hot_zone_table;
	new = &root->rb_node;

	while(*new)
	{
		hotzone = rb_entry(*new, struct hotzone, node);
		parent = *new;
		if(rmap_item->gfn > hotzone->gfn)
			new = &parent->rb_right;
		else if(rmap_item->gfn < hotzone->gfn)
			new = &parent->rb_left;
		else
		{
			if(rmap_item->number == 1)
				hotzone->vm1 = ksm_scan.seqnr;
			else if(rmap_item->number == 2)
				hotzone->vm2 = ksm_scan.seqnr;
			hotzone->count++;
			if(hotzone->count == 2)
				hz_size++;

			return;
		}
	}

	hotzone = kmalloc(sizeof(struct hotzone), GFP_KERNEL);
	hotzone->count = 1;
	hotzone->gfn = rmap_item->gfn;
	if(rmap_item->number == 1)
	{
		hotzone->vm1 = ksm_scan.seqnr;
		hotzone->vm2 = 0;
	}
	else if(rmap_item->number == 2)
	{
		hotzone->vm1 = 0;
		hotzone->vm2 = ksm_scan.seqnr;
	}
	/* new = hotzone->node */
	rb_link_node(&hotzone->node, parent, new);
	rb_insert_color(&hotzone->node, root);

}

static void hz_record(struct rmap_item *rmap_item)
{
	struct rb_root *root;
	struct rb_node **new;
	struct rb_node *parent = NULL;
	struct hotzone *hotzone;

	root = hot_zone_table;
	new = &root->rb_node;

	while(*new)
	{
		hotzone = rb_entry(*new, struct hotzone, node);
		parent = *new;
		if(rmap_item->gfn > hotzone->gfn)
			new = &parent->rb_right;
		else if(rmap_item->gfn < hotzone->gfn)
			new = &parent->rb_left;
		else
		{
			hotzone->sum += rmap_item->number;
			hotzone->count++;
			// TODO: do better log here.
			if(hotzone->count == 2)
				hz_size++;

			return;
		}
	}

	hotzone = kmalloc(sizeof(struct hotzone), GFP_KERNEL);
	hotzone->count = 1;
	hotzone->gfn = rmap_item->gfn;
	hotzone->sum = rmap_item->number;
	rb_link_node(&hotzone->node, parent, new);
	rb_insert_color(&hotzone->node, root);
}

/*
 * cmp_and_merge_page - first see if page can be merged into the stable tree;
 * if not, compare checksum to previous and if it's the same, see if page can
 * be inserted into the unstable tree, or merged with a page already there and
 * both transferred to the stable tree.
 *
 * @page: the page that we are searching identical page to.
 * @rmap_item: the reverse mapping into the virtual address of this page
 */
static void cmp_and_merge_page(struct page *page, struct rmap_item *rmap_item)
{
	struct rmap_item *tree_rmap_item;
	struct page *tree_page = NULL;
	struct stable_node *stable_node;
	struct page *kpage;
	unsigned int checksum;
	int err;

	stable_node = page_stable_node(page);
	if (stable_node) {
		if (stable_node->head != &migrate_nodes &&
		    get_kpfn_nid(stable_node->kpfn) != NUMA(stable_node->nid)) {
			rb_erase(&stable_node->node,
				 root_stable_tree + NUMA(stable_node->nid));
			stable_node->head = &migrate_nodes;
			list_add(&stable_node->list, stable_node->head);
		}
		if (stable_node->head != &migrate_nodes &&
		    rmap_item->head == stable_node)
			return;
	}

	/* We first start with searching the page inside the stable tree */
	kpage = stable_tree_search(page);
	if (kpage == page && rmap_item->head == stable_node) {
		put_page(kpage);
		return;
	}

	remove_rmap_item_from_tree(rmap_item);

	if (kpage) {
		err = try_to_merge_with_ksm_page(rmap_item, page, kpage);
		if (!err) {
			/*
			 * The page was successfully merged:
			 * add its rmap_item to the stable tree.
			 */
			// HZ ksm
			if(rmap_item->gfn != 0 && ksm_scan.seqnr == 1)
				printk("Stable: GFN = %lu, #VM = %d, checksum = %u\n", rmap_item->gfn, rmap_item->number, rmap_item->oldchecksum);
			// ------------------
			lock_page(kpage);
			/* add rmap_item into stable tree node's rmap_hlist */
			stable_tree_append(rmap_item, page_stable_node(kpage));
			unlock_page(kpage);
		}
		put_page(kpage);
		return;
	}

	/*
	 * If the hash value of the page has changed from the last time
	 * we calculated it, this page is changing frequently: therefore we
	 * don't want to insert it in the unstable tree, and we don't want
	 * to waste our time searching for something identical to it there.
	 */
	checksum = calc_checksum(page);
	if (rmap_item->oldchecksum != checksum) {
		rmap_item->oldchecksum = checksum;
		return;
	}

	tree_rmap_item =
		/* return ptr to rmap_item if identical page found, NULL otherwise */
		unstable_tree_search_insert(rmap_item, page, &tree_page);
	if (tree_rmap_item) {
		kpage = try_to_merge_two_pages(rmap_item, page,
						tree_rmap_item, tree_page);
		put_page(tree_page);
		if (kpage) {
			/*
			 * The pages were successfully merged: insert new
			 * node in the stable tree and add both rmap_items.
			 */

			if(rmap_item->gfn != 0 && ksm_scan.seqnr == 1) {
				printk("Rmap: GFN = %lu, #VM = %d, checksum = %u; ", rmap_item->gfn, rmap_item->number), rmap_item->oldchecksum;
				printk("Unstable: GFN = %lu, #VM = %d, checksum = %u\n", tree_rmap_item->gfn, tree_rmap_item->number, tree_rmap_item->oldchecksum);
			}

			lock_page(kpage);
			stable_node = stable_tree_insert(kpage);
			if (stable_node) {
				stable_tree_append(tree_rmap_item, stable_node);
				stable_tree_append(rmap_item, stable_node);
			}
			unlock_page(kpage);

			/*
			 * If we fail to insert the page into the stable tree,
			 * we will have 2 virtual addresses that are pointing
			 * to a ksm page left outside the stable tree,
			 * in which case we need to break_cow on both.
			 */
			if (!stable_node) {
				break_cow(tree_rmap_item);
				break_cow(rmap_item);
			}
		}
	}
}

static struct rmap_item *get_next_rmap_item(struct mm_slot *mm_slot,
					    struct rmap_item **rmap_list,
					    unsigned long addr)
{
	struct rmap_item *rmap_item;

	while (*rmap_list) {
		rmap_item = *rmap_list;
		if ((rmap_item->address & PAGE_MASK) == addr)
			return rmap_item;
		if (rmap_item->address > addr)
			break;
		*rmap_list = rmap_item->rmap_list;
		remove_rmap_item_from_tree(rmap_item);
		free_rmap_item(rmap_item);
	}

	rmap_item = alloc_rmap_item();
	if (rmap_item) {
		/* It has already been zeroed */
		rmap_item->mm = mm_slot->mm;
		rmap_item->address = addr;

		/* hz ksm */
		//rmap_item->scanned = 0;

		rmap_item->rmap_list = *rmap_list;
		*rmap_list = rmap_item;
	}
	return rmap_item;
}

int h1=0,h2=0,h3=0,h4=0,h5=0,h6=0,h7=0,h8=0,h9=0,h10=0,h11=0,h12=0,h13=0,h14=0,h15=0,h16=0,h17=0,h18=0,h19=0, h19a=0;
int total_rmap = 0;

static void hotzone_show(void)
{
	struct gpa_node *gpa_node;
	struct rmap_item *rmap_item;

	gpa_node = list_first_entry(&hot_zone_node, struct gpa_node, link);

	gpa_node = list_prepare_entry(gpa_node, &hot_zone_node, link);
	list_for_each_entry_continue(gpa_node, &hot_zone_node, link) {
		printk("GFN = %lu, count = %d, sum = %d\n", gpa_node->gfn, gpa_node->count, gpa_node->sum);
		// if(gpa_node->in_hot_zone) {
		// 	hlist_for_each_entry(rmap_item, &gpa_node->hlist, gfnhlist) {
		// 		printk("#VM = %d, gfn = %lu\n", rmap_item->number, rmap_item->gfn);
		// 	}
		// }
	}
	printk("Finished dumping hotzone information.\n");
}

struct vm_area_struct *last_vma = NULL;
static struct rmap_item *scan_get_next_rmap_item(struct page **page)
{
	struct mm_struct *mm;
	struct mm_slot *slot;
	struct vm_area_struct *vma;
	struct rmap_item *rmap_item;
	int nid;

	if (list_empty(&ksm_mm_head.mm_list))
		return NULL;

	slot = ksm_scan.mm_slot;
	// flush unstable tree .
	if (slot == &ksm_mm_head) {
		/*
		 * A number of pages can hang around indefinitely on per-cpu
		 * pagevecs, raised page count preventing write_protect_page
		 * from merging them.  Though it doesn't really matter much,
		 * it is puzzling to see some stuck in pages_volatile until
		 * other activity jostles them out, and they also prevented
		 * LTP's KSM test from succeeding deterministically; so drain
		 * them here (here rather than on entry to ksm_do_scan(),
		 * so we don't IPI too often when pages_to_scan is set low).
		 */
		lru_add_drain_all();

		/*
		 * Whereas stale stable_nodes on the stable_tree itself
		 * get pruned in the regular course of stable_tree_search(),
		 * those moved out to the migrate_nodes list can accumulate:
		 * so prune them once before each full scan.
		 */
		// will not executed.
		if (!ksm_merge_across_nodes) {
			struct stable_node *stable_node;
			struct list_head *this, *next;
			struct page *page;

			list_for_each_safe(this, next, &migrate_nodes) {
				stable_node = list_entry(this,
						struct stable_node, list);
				page = get_ksm_page(stable_node, false);
				if (page)
					put_page(page);
				cond_resched();
			}
		}
		// nid = 0; nid < 1; nid++
		for (nid = 0; nid < ksm_nr_node_ids; nid++)
			root_unstable_tree[nid] = RB_ROOT;

		spin_lock(&ksm_mmlist_lock);
		/* get the next slot in the mm_slot's list,
		 * done by list_entry
		 */
		slot = list_entry(slot->mm_list.next, struct mm_slot, mm_list);
		/* change to another VM */
		ksm_scan.mm_slot = slot;
		spin_unlock(&ksm_mmlist_lock);
		/*
		 * Although we tested list_empty() above, a racing __ksm_exit
		 * of the last mm on the list may have removed it since then.
		 */
		if (slot == &ksm_mm_head)
			return NULL;
next_mm:
		// printk("==========NEXT MM==========\n");
		// printk("VMA count of this mm = %d\n", slot->mm->map_count);
		/* update cursor */
		ksm_scan.address = 0;
		ksm_scan.rmap_list = &slot->rmap_list;
	}

	mm = slot->mm;
	down_read(&mm->mmap_sem);
	if (ksm_test_exit(mm))
		vma = NULL;
	else
		vma = find_vma(mm, ksm_scan.address);
		/*
		if(vma != last_vma) {
			last_vma = vma;
			printk("VMA start = %lu, end = %lu\n", vma->vm_start >> 12, vma->vm_end >> 12);
		}
		*/

	for (; vma; vma = vma->vm_next) {
		if (!(vma->vm_flags & VM_MERGEABLE))
			continue;
		/* find the next vma and set the address */
		if (ksm_scan.address < vma->vm_start)
			ksm_scan.address = vma->vm_start;
		/* ksm only scan anonymous pages */
		if (!vma->anon_vma)
			ksm_scan.address = vma->vm_end;
		/* search through current vma */
		while (ksm_scan.address < vma->vm_end) {
			if (ksm_test_exit(mm))
				break;
			*page = follow_page(vma, ksm_scan.address, FOLL_GET);
			if (IS_ERR_OR_NULL(*page)) {
				ksm_scan.address += PAGE_SIZE;
				cond_resched();
				continue;
			}
			if (PageAnon(*page) ||
			    page_trans_compound_anon(*page)) {
				flush_anon_page(vma, *page, ksm_scan.address);
				flush_dcache_page(*page);
				rmap_item = get_next_rmap_item(slot,
					ksm_scan.rmap_list, ksm_scan.address);
				if (rmap_item) {
					ksm_scan.rmap_list =
							&rmap_item->rmap_list;
					ksm_scan.address += PAGE_SIZE;
				} else
					put_page(*page);
				up_read(&mm->mmap_sem);
				return rmap_item;
			}
			put_page(*page);
			ksm_scan.address += PAGE_SIZE;
			cond_resched();
		}
	}

	if (ksm_test_exit(mm)) {
		ksm_scan.address = 0;
		ksm_scan.rmap_list = &slot->rmap_list;
	}
	/*
	 * Nuke all the rmap_items that are above this current rmap:
	 * because there were no VM_MERGEABLE vmas with such addresses.
	 */
	remove_trailing_rmap_items(slot, ksm_scan.rmap_list);

	spin_lock(&ksm_mmlist_lock);
	ksm_scan.mm_slot = list_entry(slot->mm_list.next,
						struct mm_slot, mm_list);
	if (ksm_scan.address == 0) {
		/*
		 * We've completed a full scan of all vmas, holding mmap_sem
		 * throughout, and found no VM_MERGEABLE: so do the same as
		 * __ksm_exit does to remove this mm from all our lists now.
		 * This applies either when cleaning up after __ksm_exit
		 * (but beware: we can reach here even before __ksm_exit),
		 * or when all VM_MERGEABLE areas have been unmapped (and
		 * mmap_sem then protects against race with MADV_MERGEABLE).
		 */
		hash_del(&slot->link);
		list_del(&slot->mm_list);
		spin_unlock(&ksm_mmlist_lock);

		free_mm_slot(slot);
		clear_bit(MMF_VM_MERGEABLE, &mm->flags);
		up_read(&mm->mmap_sem);
		mmdrop(mm);
	} else {
		spin_unlock(&ksm_mmlist_lock);
		up_read(&mm->mmap_sem);
	}

	/* Repeat until we've completed scanning the whole list */
	slot = ksm_scan.mm_slot;
	if (slot != &ksm_mm_head)
		goto next_mm;

	ksm_scan.seqnr++;	/* one round is finished */
	time_flag = 1;
	// define_prescan = 0;
	hz_size = 0;

	/* dumping hotzone information */
	
	if(ksm_scan.seqnr > 0)  {
		printk("Start dumping hotzone information in hotzone_show() after round %lu :\n", ksm_scan.seqnr);
		hotzone_show();	
	}
	

	printk("(potential) hotzone size in round #%lu: %d\n", ksm_scan.seqnr, hz_size);
	printk("total rmap_item in hotzone: %d\n", total_rmap);

	return NULL;
}

/* hz ksm */
static void collect_page_info(struct rmap_item *rmap_item)
{
	struct gpa_node *gpa_node, *search_node;
	struct list_head *head;

	if(rmap_item->gfn <= 3686)
		head = &gpa_node_head1;
	else if(rmap_item->gfn >= 3687 && rmap_item->gfn <= 7373)
		head = &gpa_node_head1a;
	else if(rmap_item->gfn >= 7374 && rmap_item->gfn <= 11060)
		head = &gpa_node_head1b;
	else if(rmap_item->gfn >= 11061 && rmap_item->gfn <= 14747)
        head = &gpa_node_head1c;

	else if(rmap_item->gfn >= 14748 && rmap_item->gfn <= 18434)
		head = &gpa_node_head2;
	else if(rmap_item->gfn >= 18435 && rmap_item->gfn <= 22121)
		head = &gpa_node_head2a;
	else if(rmap_item->gfn >= 22122 && rmap_item->gfn <= 25808)
		head = &gpa_node_head2b;
	else if(rmap_item->gfn >= 25809 && rmap_item->gfn <= 29495)
		head = &gpa_node_head2c;

    else if(rmap_item->gfn >= 29496 && rmap_item->gfn <= 33182)
		head = &gpa_node_head3;
	else if(rmap_item->gfn >= 33183 && rmap_item->gfn <= 36869)
		head = &gpa_node_head3a;
	else if(rmap_item->gfn >= 36870 && rmap_item->gfn <= 40556)
		head = &gpa_node_head3b;
	else if(rmap_item->gfn >= 40557 && rmap_item->gfn <= 44243)
		head = &gpa_node_head3c;

    else if(rmap_item->gfn >= 44244 && rmap_item->gfn <= 47930)
		head = &gpa_node_head4;
	else if(rmap_item->gfn >= 47931 && rmap_item->gfn <= 51617)
		head = &gpa_node_head4a;
	else if(rmap_item->gfn >= 51618 && rmap_item->gfn <= 55304)
		head = &gpa_node_head4b;
	else if(rmap_item->gfn >= 55305 && rmap_item->gfn <= 58991)
		head = &gpa_node_head4c;

	/* bug here ? */
	else if(rmap_item->gfn >= 58992 && rmap_item->gfn <= 62678)
		head = &gpa_node_head5;
	else if(rmap_item->gfn >= 62679 && rmap_item->gfn <= 66365)
		head = &gpa_node_head5a;
	else if(rmap_item->gfn >= 66366 && rmap_item->gfn <= 70052)
		head = &gpa_node_head5b;
	else if(rmap_item->gfn >= 70053 && rmap_item->gfn <= 73739)
		head = &gpa_node_head5c;

	else if(rmap_item->gfn >= 73740 && rmap_item->gfn <= 77426)
		head = &gpa_node_head6;
	else if(rmap_item->gfn >= 77427 && rmap_item->gfn <= 81113)
		head = &gpa_node_head6a;
	else if(rmap_item->gfn >= 81114 && rmap_item->gfn <= 84800)
		head = &gpa_node_head6b;
	else if(rmap_item->gfn >= 84801 && rmap_item->gfn <= 88487)
		head = &gpa_node_head6c;

	else if(rmap_item->gfn >= 88488 && rmap_item->gfn <= 92174)
		head = &gpa_node_head7;
	else if(rmap_item->gfn >= 92175 && rmap_item->gfn <= 95861)
		head = &gpa_node_head7a;
	else if(rmap_item->gfn >= 95862 && rmap_item->gfn <= 99548)
		head = &gpa_node_head7b;
	else if(rmap_item->gfn >= 99549 && rmap_item->gfn <= 103235)
		head = &gpa_node_head7c;

    else if(rmap_item->gfn >= 103236 && rmap_item->gfn <= 106922)
		head = &gpa_node_head8;
	else if(rmap_item->gfn >= 106923 && rmap_item->gfn <= 110609)
		head = &gpa_node_head8a;
	else if(rmap_item->gfn >= 110610 && rmap_item->gfn <= 114296)
		head = &gpa_node_head8b;
	else if(rmap_item->gfn >= 114297 && rmap_item->gfn <= 117983)
		head = &gpa_node_head8c;

	else if(rmap_item->gfn >= 117984 && rmap_item->gfn <= 121670)
		head = &gpa_node_head9;
	else if(rmap_item->gfn >= 121671 && rmap_item->gfn <= 125357)
		head = &gpa_node_head9a;
	else if(rmap_item->gfn >= 125358 && rmap_item->gfn <= 129044)
		head = &gpa_node_head9b;
	else if(rmap_item->gfn >= 129045 && rmap_item->gfn <= 132731)
		head = &gpa_node_head9c;

    else if(rmap_item->gfn >= 132732 && rmap_item->gfn <= 136418)
		head = &gpa_node_head10;
	else if(rmap_item->gfn >= 136419 && rmap_item->gfn <= 140105)
		head = &gpa_node_head10a;
	else if(rmap_item->gfn >= 140106 && rmap_item->gfn <= 143792)
		head = &gpa_node_head10b;
	else if(rmap_item->gfn >= 143793 && rmap_item->gfn <= 147479)
		head = &gpa_node_head10c;

	else if(rmap_item->gfn >= 147480 && rmap_item->gfn <= 151166)
		head = &gpa_node_head11;
	else if(rmap_item->gfn >= 151167 && rmap_item->gfn <= 154850)
		head = &gpa_node_head11a;
	else if(rmap_item->gfn >= 154851 && rmap_item->gfn <= 158540)
		head = &gpa_node_head11b;
	else if(rmap_item->gfn >= 158541 && rmap_item->gfn <= 162227)
		head = &gpa_node_head11c;

	else if(rmap_item->gfn >= 162228 && rmap_item->gfn <= 165914)
		head = &gpa_node_head12;
	else if(rmap_item->gfn >= 165915 && rmap_item->gfn <= 169601)
		head = &gpa_node_head12a;
	else if(rmap_item->gfn >= 169602 && rmap_item->gfn <= 173288)
		head = &gpa_node_head12b;
	else if(rmap_item->gfn >= 173289 && rmap_item->gfn <= 176975)
		head = &gpa_node_head12c;

	else if(rmap_item->gfn >= 176976 && rmap_item->gfn <= 180662)
		head = &gpa_node_head13;
	else if(rmap_item->gfn >= 180663 && rmap_item->gfn <= 184349)
		head = &gpa_node_head13a;
	else if(rmap_item->gfn >= 184350 && rmap_item->gfn <= 188036)
		head = &gpa_node_head13b;
	else if(rmap_item->gfn >= 188037 && rmap_item->gfn <= 191723)
		head = &gpa_node_head13c;

	else if(rmap_item->gfn >= 191724 && rmap_item->gfn <= 195410)
		head = &gpa_node_head14;
	else if(rmap_item->gfn >= 195411 && rmap_item->gfn <= 199097)
		head = &gpa_node_head14a;
	else if(rmap_item->gfn >= 199098 && rmap_item->gfn <= 202784)
		head = &gpa_node_head14b;
	else if(rmap_item->gfn >= 202785 && rmap_item->gfn <= 206471)
		head = &gpa_node_head14c;

	else if(rmap_item->gfn >= 206472 && rmap_item->gfn <= 210158)
		head = &gpa_node_head15;
	else if(rmap_item->gfn >= 210159 && rmap_item->gfn <= 213845)
		head = &gpa_node_head15a;
	else if(rmap_item->gfn >= 213846 && rmap_item->gfn <= 217532)
		head = &gpa_node_head15b;
	else if(rmap_item->gfn >= 217533 && rmap_item->gfn <= 221219)
		head = &gpa_node_head15c;

	else if(rmap_item->gfn >= 221220 && rmap_item->gfn <= 224906)
		head = &gpa_node_head16;
	else if(rmap_item->gfn >= 224907 && rmap_item->gfn <= 228593)
		head = &gpa_node_head16a;
	else if(rmap_item->gfn >= 228594 && rmap_item->gfn <= 232280)
		head = &gpa_node_head16b;
	else if(rmap_item->gfn >= 232281 && rmap_item->gfn <= 235967)
		head = &gpa_node_head16c;

	else if(rmap_item->gfn >= 235968 && rmap_item->gfn <= 239654)
		head = &gpa_node_head17;
	else if(rmap_item->gfn >= 239655 && rmap_item->gfn <= 243341)
		head = &gpa_node_head17a;
	else if(rmap_item->gfn >= 243342 && rmap_item->gfn <= 247028)
		head = &gpa_node_head17b;
	else if(rmap_item->gfn >= 247029 && rmap_item->gfn <= 250715)
		head = &gpa_node_head17c;

	else if(rmap_item->gfn >= 250716 && rmap_item->gfn <= 254402)
		head = &gpa_node_head18;
	else if(rmap_item->gfn >= 254403 && rmap_item->gfn <= 258089)
		head = &gpa_node_head18a;
	else if(rmap_item->gfn >= 258090 && rmap_item->gfn <= 261888)
		head = &gpa_node_head18b;
	else if(rmap_item->gfn >= 999424 && rmap_item->gfn <= 1003110)
		head = &gpa_node_head18c;

	else if(rmap_item->gfn >= 1003111 && rmap_item->gfn <= 1006797)
		head = &gpa_node_head19;
	else if(rmap_item->gfn >= 1006798 && rmap_item->gfn <= 1010484)
		head = &gpa_node_head19a;
	else if(rmap_item->gfn >= 1010485 && rmap_item->gfn <= 1014171)
		head = &gpa_node_head19b;
	else if(rmap_item->gfn >= 1014172 && rmap_item->gfn <= 1017858)
		head = &gpa_node_head19c;

	else if(rmap_item->gfn >= 1017859 && rmap_item->gfn <= 1021545)
		head = &gpa_node_head20;
	else if(rmap_item->gfn >= 1021546 && rmap_item->gfn <= 1025232)
		head = &gpa_node_head20a;
	else if(rmap_item->gfn >= 1025233 && rmap_item->gfn <= 1028919)
		head = &gpa_node_head20b;
	else
		head = &gpa_node_head20c;


	if(list_empty(head))
		goto create;

	list_for_each_entry(search_node, head, link) {
		if(search_node->gfn == rmap_item->gfn)
		{
			/* insert rmap_item at gpa_node's hlist head */
			search_node->count++;
			search_node->sum += rmap_item->number;
			hlist_add_head(&rmap_item->gfnhlist, &search_node->hlist);
			return;
		}
	}

create:
	gpa_node = alloc_gpa_node();
	INIT_HLIST_HEAD(&gpa_node->hlist);
	gpa_node->gfn = rmap_item->gfn;
	gpa_node->count = 1;
	gpa_node->sum = rmap_item->number;
	/* insert rmap_item at gpa_node's hlist head */
	hlist_add_head(&rmap_item->gfnhlist, &gpa_node->hlist);
	/* insert arg1 after arg2 */
	list_add(&gpa_node->link, head);

}

/* 
 * go through each list and check each search_node is in hot zone or not,
 * and then move them to corresponding lists of either
 * hot_zone_node or rest_gpa_node_head
 */
static void table_lookup(struct list_head *head)
{
	int find;
	struct gpa_node *search_node, *next;

	list_for_each_entry_safe(search_node, next, head, link) {
		find = 0;
		if(intable(search_node->gfn)) {
			hit++;
			search_node->in_hot_zone = 1;
			list_move(&search_node->link, &hot_zone_node);
			find = 1;
		}
		if(!find) {
			search_node->in_hot_zone = 0;
			list_move(&search_node->link, &rest_gpa_node_head);
		}
	}
	/*
	list_for_each_entry_safe(search_node, next, head, link) {
		struct rb_root *root;
		struct rb_node **new;
		struct rb_node *parent;
		struct hotzone *hotzone;
		find = 0;

		root = hot_zone_table;
		new = &root->rb_node;
		parent = NULL;

		while(*new)
		{
			hotzone = rb_entry(*new, struct hotzone, node);
			parent = *new;

			if(search_node->gfn > hotzone->gfn)
				new = &parent->rb_right;
			else if(search_node->gfn < hotzone->gfn)
				new = &parent->rb_left;
			else
			{	
				
				// if(hotzone->vm1 == ksm_scan.seqnr && hotzone->vm2 == ksm_scan.seqnr)
				// {
				// 	search_node->in_hot_zone = 1;
				// 	list_move(&search_node->link, &hot_zone_node);
				// 	find = 1;
				// }

				if(hotzone->count >= 2) {
					search_node->in_hot_zone = 1;
					list_move(&search_node->link, &hot_zone_node);
					find = 1;
				}
				break;
			}
		}

		if(!find)
		{
			search_node->in_hot_zone = 0;
			list_move(&search_node->link, &rest_gpa_node_head);
		}
	}
	*/
}

/* hz ksm */
static void define_prescan_section(void)
{
	struct list_head *head;

	head = &gpa_node_head1;
	table_lookup(head);
	head = &gpa_node_head1a;
	table_lookup(head);
	head = &gpa_node_head1b;
	table_lookup(head);
	head = &gpa_node_head1c;
	table_lookup(head);

	head = &gpa_node_head2;
	table_lookup(head);
	head = &gpa_node_head2a;
	table_lookup(head);
	head = &gpa_node_head2b;
	table_lookup(head);
	head = &gpa_node_head2c;
	table_lookup(head);

	head = &gpa_node_head3;
	table_lookup(head);
	head = &gpa_node_head3a;
	table_lookup(head);
	head = &gpa_node_head3b;
	table_lookup(head);
	head = &gpa_node_head3c;
	table_lookup(head);

	head = &gpa_node_head4;
	table_lookup(head);
	head = &gpa_node_head4a;
	table_lookup(head);
	head = &gpa_node_head4b;
	table_lookup(head);
	head = &gpa_node_head4c;
	table_lookup(head);

	head = &gpa_node_head5;
	table_lookup(head);
	head = &gpa_node_head5a;
	table_lookup(head);
	head = &gpa_node_head5b;
	table_lookup(head);
	head = &gpa_node_head5c;
	table_lookup(head);

	head = &gpa_node_head6;
	table_lookup(head);
	head = &gpa_node_head6a;
	table_lookup(head);
	head = &gpa_node_head6b;
	table_lookup(head);
	head = &gpa_node_head6c;
	table_lookup(head);

	head = &gpa_node_head7;
	table_lookup(head);
	head = &gpa_node_head7a;
	table_lookup(head);
	head = &gpa_node_head7b;
	table_lookup(head);
	head = &gpa_node_head7c;
	table_lookup(head);

	head = &gpa_node_head8;
	table_lookup(head);
	head = &gpa_node_head8a;
	table_lookup(head);
	head = &gpa_node_head8b;
	table_lookup(head);
	head = &gpa_node_head8c;
	table_lookup(head);

	head = &gpa_node_head9;
	table_lookup(head);
	head = &gpa_node_head9a;
	table_lookup(head);
	head = &gpa_node_head9b;
	table_lookup(head);
	head = &gpa_node_head9c;
	table_lookup(head);

	head = &gpa_node_head10;
	table_lookup(head);
	head = &gpa_node_head10a;
	table_lookup(head);
	head = &gpa_node_head10b;
	table_lookup(head);
	head = &gpa_node_head10c;
	table_lookup(head);

	head = &gpa_node_head11;
	table_lookup(head);
	head = &gpa_node_head11a;
	table_lookup(head);
	head = &gpa_node_head11b;
	table_lookup(head);
	head = &gpa_node_head11c;
	table_lookup(head);

	head = &gpa_node_head12;
	table_lookup(head);
	head = &gpa_node_head12a;
	table_lookup(head);
	head = &gpa_node_head12b;
	table_lookup(head);
	head = &gpa_node_head12c;
	table_lookup(head);

	head = &gpa_node_head13;
	table_lookup(head);
	head = &gpa_node_head13a;
	table_lookup(head);
	head = &gpa_node_head13b;
	table_lookup(head);
	head = &gpa_node_head13c;
	table_lookup(head);

	head = &gpa_node_head14;
	table_lookup(head);
	head = &gpa_node_head14a;
	table_lookup(head);
	head = &gpa_node_head14b;
	table_lookup(head);
	head = &gpa_node_head14c;
	table_lookup(head);

	head = &gpa_node_head15;
	table_lookup(head);
	head = &gpa_node_head15a;
	table_lookup(head);
	head = &gpa_node_head15b;
	table_lookup(head);
	head = &gpa_node_head15c;
	table_lookup(head);

	head = &gpa_node_head16;
	table_lookup(head);
	head = &gpa_node_head16a;
	table_lookup(head);
	head = &gpa_node_head16b;
	table_lookup(head);
	head = &gpa_node_head16c;
	table_lookup(head);

	head = &gpa_node_head17;
	table_lookup(head);
	head = &gpa_node_head17a;
	table_lookup(head);
	head = &gpa_node_head17b;
	table_lookup(head);
	head = &gpa_node_head17c;
	table_lookup(head);

	head = &gpa_node_head18;
	table_lookup(head);
	head = &gpa_node_head18a;
	table_lookup(head);
	head = &gpa_node_head18b;
	table_lookup(head);
	head = &gpa_node_head18c;
	table_lookup(head);

	head = &gpa_node_head19;
	table_lookup(head);
	head = &gpa_node_head19a;
	table_lookup(head);
	head = &gpa_node_head19b;
	table_lookup(head);
	head = &gpa_node_head19c;
	table_lookup(head);

	head = &gpa_node_head20;
	table_lookup(head);
	head = &gpa_node_head20a;
	table_lookup(head);
	head = &gpa_node_head20b;
	table_lookup(head);
	head = &gpa_node_head20c;
	table_lookup(head);
}

/*
static void redefine_hot_zone_list(void)
{
	struct gpa_node *search_node, *next;
	list_for_each_entry_safe(search_node, next, &hot_zone_node, link) {
		struct rb_root *root;
		struct rb_node **new;
		struct rb_node *parent;
		struct hotzone *hotzone;

		root = hot_zone_table;
		new = &root->rb_node;
		parent = NULL;

		while(*new)
		{
			hotzone = rb_entry(*new, struct hotzone, node);
			parent = *new;

			if(search_node->gpa > hotzone->gpa)
				new = &parent->rb_right;
			else if(search_node->gpa < hotzone->gpa)
				new = &parent->rb_left;
			else
			{
				if(hotzone->vm1 != hotzone->vm2)
				{
					search_node->in_hot_zone = 0;
					list_move(&search_node->link, &gpa_node_head);
				}
				break;
			}
		}
	}
}
*/

/* hz ksm */
struct gpa_node *cursor = NULL;
static void hot_zone_scan(unsigned int *scan_npages)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct page *page;
	struct rmap_item *rmap_item;
	struct gpa_node *gpa_node;

	if(cursor == NULL)
		gpa_node = list_first_entry(&hot_zone_node, struct gpa_node, link);
	else
		gpa_node = cursor;

	gpa_node = list_prepare_entry(gpa_node, &hot_zone_node, link);
	list_for_each_entry_continue(gpa_node, &hot_zone_node, link) {

		(*scan_npages)--;
		if(*scan_npages == 0)
		{
			cursor = gpa_node;
			break;
		}

		if(list_is_last(&gpa_node->link, &hot_zone_node))
		{
			scan_hot_zone = 0;
			scan_remain = 1;	/*hot zone end, starting scan remain zone*/
			cursor = NULL;
		}

		if(gpa_node->in_hot_zone)
		{
			//printk("gfn %lu:", gpa_node->gfn);
			hlist_for_each_entry(rmap_item, &gpa_node->hlist, gfnhlist) {
				total_rmap++;
				//rmap_item->scanned = 1;
				//printk(" #VM = %d, gfn = %lu", rmap_item->number, rmap_item->gfn);
				//printk(" hva %lu \n", rmap_item->address);
				mm = rmap_item->mm;
				down_read(&mm->mmap_sem);
				vma = find_vma(mm, rmap_item->address);
				page = follow_page(vma, rmap_item->address, FOLL_GET);
				up_read(&mm->mmap_sem);

				cmp_and_merge_page(page, rmap_item);
				put_page(page);
			}
		}
	}
}

static void remain_zone_scan(unsigned int *scan_npages)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct page *page;
	struct rmap_item *rmap_item;
	struct gpa_node *gpa_node;

	if(cursor == NULL)
		gpa_node = list_first_entry(&rest_gpa_node_head, struct gpa_node, link);
	else
		gpa_node = cursor;

	gpa_node = list_prepare_entry(gpa_node, &rest_gpa_node_head, link);
	list_for_each_entry_continue(gpa_node, &rest_gpa_node_head, link) {

		(*scan_npages)--;
		if(*scan_npages == 0)
		{
			cursor = gpa_node;
			break;
		}

		if(list_is_last(&gpa_node->link, &rest_gpa_node_head))
		{
			scan_remain = 0;
			cursor = NULL;
		}

		hlist_for_each_entry(rmap_item, &gpa_node->hlist, gfnhlist) {
			mm = rmap_item->mm;
			//rmap_item->scanned = 1;
			down_read(&mm->mmap_sem);
			vma = find_vma(mm, rmap_item->address);
			page = follow_page(vma, rmap_item->address, FOLL_GET);
			up_read(&mm->mmap_sem);

			cmp_and_merge_page(page, rmap_item);
			put_page(page);
		}
	}
}

/*
static void remaining_show(void)
{
	struct gpa_node *gpa_node;
	struct rmap_item *rmap_item;
	list_for_each_entry(gpa_node, &rest_gpa_node_head, link) {
		printk("gfn: %lu ", gpa_node->gfn);
		hlist_for_each_entry(rmap_item, &gpa_node->hlist,gfnhlist) {
			printk("%lu ", rmap_item->gfn);
		}
		printk("\n");
	}
}
*/

/**
 * ksm_do_scan  - the ksm scanner main worker function.
 * @scan_npages - number of pages we want to scan before we return.
 */
static void ksm_do_scan(unsigned int scan_npages)
{
	struct rmap_item *rmap_item;
	struct page *uninitialized_var(page);
	unsigned long hva;

	while (scan_npages-- && likely(!freezing(current))) {
		cond_resched();
		rmap_item = scan_get_next_rmap_item(&page);
		if (!rmap_item)
			return;

		/* store gfn, #vm in each rmap_item at first round */
		if(ksm_scan.seqnr == 0)
		{
			rmap_item->number = 0;
			hva = rmap_item->address >> 12;		/* >> 12 so it's basically a page number */
			rmap_item->gfn = kvm_hva_to_gfn(hva, &rmap_item->number);
		}

		cmp_and_merge_page(page, rmap_item);
		put_page(page);
	}
}

static int ksmd_should_run(void)
{
	return (ksm_run & KSM_RUN_MERGE) && !list_empty(&ksm_mm_head.mm_list);
}

static int ksm_scan_thread(void *nothing)
{
	set_freezable();
	set_user_nice(current, 5);
	//hotzone_init();

	while (!kthread_should_stop()) {
		mutex_lock(&ksm_thread_mutex);
		// hz ksm time measurement
		do_gettimeofday(&before);
		wait_while_offlining();
		if (ksmd_should_run())
			ksm_do_scan(ksm_thread_pages_to_scan);
		do_gettimeofday(&after);
		ksm_time += (after.tv_sec - before.tv_sec) * 1000 * 1000;
		ksm_time += (after.tv_usec - before.tv_usec);
		mutex_unlock(&ksm_thread_mutex);

		if(time_flag == 1) {
			printk("Round: %lu Ksm Time: %lu us\n", ksm_scan.seqnr - 1, ksm_time);
			printk("Round: %li Break Time: %lu us\n", ksm_scan.seqnr - 1, break_time);
			time_flag = 0;
			ksm_time = 0;
			break_time = 0;
		}

		try_to_freeze();

		if (ksmd_should_run()) {
			do_gettimeofday(&before);
			schedule_timeout_interruptible(
				msecs_to_jiffies(ksm_thread_sleep_millisecs));
			do_gettimeofday(&after);
			break_time += (after.tv_sec - before.tv_sec) * 1000 * 1000;
			break_time += (after.tv_usec - before.tv_usec);
		} else {
			wait_event_freezable(ksm_thread_wait,
				ksmd_should_run() || kthread_should_stop());
		}
	}
	ksm_time = 0;
	break_time = 0;
	print_vma = 0;
	return 0;
}

int ksm_madvise(struct vm_area_struct *vma, unsigned long start,
		unsigned long end, int advice, unsigned long *vm_flags)
{
	struct mm_struct *mm = vma->vm_mm;
	int err;

	switch (advice) {
	case MADV_MERGEABLE:
		/*
		 * Be somewhat over-protective for now!
		 */
		if (*vm_flags & (VM_MERGEABLE | VM_SHARED  | VM_MAYSHARE   |
				 VM_PFNMAP    | VM_IO      | VM_DONTEXPAND |
				 VM_HUGETLB | VM_MIXEDMAP))
			return 0;		/* just ignore the advice */

#ifdef VM_SAO
		if (*vm_flags & VM_SAO)
			return 0;
#endif

		if (!test_bit(MMF_VM_MERGEABLE, &mm->flags)) {
			err = __ksm_enter(mm);
			if (err)
				return err;
		}

		*vm_flags |= VM_MERGEABLE;
		break;

	case MADV_UNMERGEABLE:
		if (!(*vm_flags & VM_MERGEABLE))
			return 0;		/* just ignore the advice */

		if (vma->anon_vma) {
			err = unmerge_ksm_pages(vma, start, end);
			if (err)
				return err;
		}

		*vm_flags &= ~VM_MERGEABLE;
		break;
	}

	return 0;
}

int __ksm_enter(struct mm_struct *mm)
{
	struct mm_slot *mm_slot;
	int needs_wakeup;

	mm_slot = alloc_mm_slot();
	if (!mm_slot)
		return -ENOMEM;

	/* Check ksm_run too?  Would need tighter locking */
	needs_wakeup = list_empty(&ksm_mm_head.mm_list);

	spin_lock(&ksm_mmlist_lock);
	insert_to_mm_slots_hash(mm, mm_slot);
	/*
	 * When KSM_RUN_MERGE (or KSM_RUN_STOP),
	 * insert just behind the scanning cursor, to let the area settle
	 * down a little; when fork is followed by immediate exec, we don't
	 * want ksmd to waste time setting up and tearing down an rmap_list.
	 *
	 * But when KSM_RUN_UNMERGE, it's important to insert ahead of its
	 * scanning cursor, otherwise KSM pages in newly forked mms will be
	 * missed: then we might as well insert at the end of the list.
	 */
	if (ksm_run & KSM_RUN_UNMERGE)
		list_add_tail(&mm_slot->mm_list, &ksm_mm_head.mm_list);
	else
		list_add_tail(&mm_slot->mm_list, &ksm_scan.mm_slot->mm_list);
	spin_unlock(&ksm_mmlist_lock);

	set_bit(MMF_VM_MERGEABLE, &mm->flags);
	atomic_inc(&mm->mm_count);

	if (needs_wakeup)
		wake_up_interruptible(&ksm_thread_wait);

	return 0;
}

void __ksm_exit(struct mm_struct *mm)
{
	struct mm_slot *mm_slot;
	int easy_to_free = 0;

	/*
	 * This process is exiting: if it's straightforward (as is the
	 * case when ksmd was never running), free mm_slot immediately.
	 * But if it's at the cursor or has rmap_items linked to it, use
	 * mmap_sem to synchronize with any break_cows before pagetables
	 * are freed, and leave the mm_slot on the list for ksmd to free.
	 * Beware: ksm may already have noticed it exiting and freed the slot.
	 */

	spin_lock(&ksm_mmlist_lock);
	mm_slot = get_mm_slot(mm);
	if (mm_slot && ksm_scan.mm_slot != mm_slot) {
		if (!mm_slot->rmap_list) {
			hash_del(&mm_slot->link);
			list_del(&mm_slot->mm_list);
			easy_to_free = 1;
		} else {
			list_move(&mm_slot->mm_list,
				  &ksm_scan.mm_slot->mm_list);
		}
	}
	spin_unlock(&ksm_mmlist_lock);

	if (easy_to_free) {
		free_mm_slot(mm_slot);
		clear_bit(MMF_VM_MERGEABLE, &mm->flags);
		mmdrop(mm);
	} else if (mm_slot) {
		down_write(&mm->mmap_sem);
		up_write(&mm->mmap_sem);
	}
}

struct page *ksm_might_need_to_copy(struct page *page,
			struct vm_area_struct *vma, unsigned long address)
{
	struct anon_vma *anon_vma = page_anon_vma(page);
	struct page *new_page;

	if (PageKsm(page)) {
		if (page_stable_node(page) &&
		    !(ksm_run & KSM_RUN_UNMERGE))
			return page;	/* no need to copy it */
	} else if (!anon_vma) {
		return page;		/* no need to copy it */
	} else if (anon_vma->root == vma->anon_vma->root &&
		 page->index == linear_page_index(vma, address)) {
		return page;		/* still no need to copy it */
	}
	if (!PageUptodate(page))
		return page;		/* let do_swap_page report the error */

	new_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, address);
	if (new_page) {
		copy_user_highpage(new_page, page, address, vma);

		SetPageDirty(new_page);
		__SetPageUptodate(new_page);
		__set_page_locked(new_page);
	}

	return new_page;
}

int rmap_walk_ksm(struct page *page, struct rmap_walk_control *rwc)
{
	struct stable_node *stable_node;
	struct rmap_item *rmap_item;
	int ret = SWAP_AGAIN;
	int search_new_forks = 0;

	VM_BUG_ON_PAGE(!PageKsm(page), page);

	/*
	 * Rely on the page lock to protect against concurrent modifications
	 * to that page's node of the stable tree.
	 */
	VM_BUG_ON_PAGE(!PageLocked(page), page);

	stable_node = page_stable_node(page);
	if (!stable_node)
		return ret;
again:
	hlist_for_each_entry(rmap_item, &stable_node->hlist, hlist) {
		struct anon_vma *anon_vma = rmap_item->anon_vma;
		struct anon_vma_chain *vmac;
		struct vm_area_struct *vma;

		cond_resched();
		anon_vma_lock_read(anon_vma);
		anon_vma_interval_tree_foreach(vmac, &anon_vma->rb_root,
					       0, ULONG_MAX) {
			cond_resched();
			vma = vmac->vma;
			if (rmap_item->address < vma->vm_start ||
			    rmap_item->address >= vma->vm_end)
				continue;
			/*
			 * Initially we examine only the vma which covers this
			 * rmap_item; but later, if there is still work to do,
			 * we examine covering vmas in other mms: in case they
			 * were forked from the original since ksmd passed.
			 */
			if ((rmap_item->mm == vma->vm_mm) == search_new_forks)
				continue;

			if (rwc->invalid_vma && rwc->invalid_vma(vma, rwc->arg))
				continue;

			ret = rwc->rmap_one(page, vma,
					rmap_item->address, rwc->arg);
			if (ret != SWAP_AGAIN) {
				anon_vma_unlock_read(anon_vma);
				goto out;
			}
			if (rwc->done && rwc->done(page)) {
				anon_vma_unlock_read(anon_vma);
				goto out;
			}
		}
		anon_vma_unlock_read(anon_vma);
	}
	if (!search_new_forks++)
		goto again;
out:
	return ret;
}

#ifdef CONFIG_MIGRATION
void ksm_migrate_page(struct page *newpage, struct page *oldpage)
{
	struct stable_node *stable_node;

	VM_BUG_ON_PAGE(!PageLocked(oldpage), oldpage);
	VM_BUG_ON_PAGE(!PageLocked(newpage), newpage);
	VM_BUG_ON_PAGE(newpage->mapping != oldpage->mapping, newpage);

	stable_node = page_stable_node(newpage);
	if (stable_node) {
		VM_BUG_ON_PAGE(stable_node->kpfn != page_to_pfn(oldpage), oldpage);
		stable_node->kpfn = page_to_pfn(newpage);
		/*
		 * newpage->mapping was set in advance; now we need smp_wmb()
		 * to make sure that the new stable_node->kpfn is visible
		 * to get_ksm_page() before it can see that oldpage->mapping
		 * has gone stale (or that PageSwapCache has been cleared).
		 */
		smp_wmb();
		set_page_stable_node(oldpage, NULL);
	}
}
#endif /* CONFIG_MIGRATION */

#ifdef CONFIG_MEMORY_HOTREMOVE
static void wait_while_offlining(void)
{
	while (ksm_run & KSM_RUN_OFFLINE) {
		mutex_unlock(&ksm_thread_mutex);
		wait_on_bit(&ksm_run, ilog2(KSM_RUN_OFFLINE),
			    TASK_UNINTERRUPTIBLE);
		mutex_lock(&ksm_thread_mutex);
	}
}

static void ksm_check_stable_tree(unsigned long start_pfn,
				  unsigned long end_pfn)
{
	struct stable_node *stable_node;
	struct list_head *this, *next;
	struct rb_node *node;
	int nid;

	for (nid = 0; nid < ksm_nr_node_ids; nid++) {
		node = rb_first(root_stable_tree + nid);
		while (node) {
			stable_node = rb_entry(node, struct stable_node, node);
			if (stable_node->kpfn >= start_pfn &&
			    stable_node->kpfn < end_pfn) {
				/*
				 * Don't get_ksm_page, page has already gone:
				 * which is why we keep kpfn instead of page*
				 */
				remove_node_from_stable_tree(stable_node);
				node = rb_first(root_stable_tree + nid);
			} else
				node = rb_next(node);
			cond_resched();
		}
	}
	list_for_each_safe(this, next, &migrate_nodes) {
		stable_node = list_entry(this, struct stable_node, list);
		if (stable_node->kpfn >= start_pfn &&
		    stable_node->kpfn < end_pfn)
			remove_node_from_stable_tree(stable_node);
		cond_resched();
	}
}

static int ksm_memory_callback(struct notifier_block *self,
			       unsigned long action, void *arg)
{
	struct memory_notify *mn = arg;

	switch (action) {
	case MEM_GOING_OFFLINE:
		/*
		 * Prevent ksm_do_scan(), unmerge_and_remove_all_rmap_items()
		 * and remove_all_stable_nodes() while memory is going offline:
		 * it is unsafe for them to touch the stable tree at this time.
		 * But unmerge_ksm_pages(), rmap lookups and other entry points
		 * which do not need the ksm_thread_mutex are all safe.
		 */
		mutex_lock(&ksm_thread_mutex);
		ksm_run |= KSM_RUN_OFFLINE;
		mutex_unlock(&ksm_thread_mutex);
		break;

	case MEM_OFFLINE:
		/*
		 * Most of the work is done by page migration; but there might
		 * be a few stable_nodes left over, still pointing to struct
		 * pages which have been offlined: prune those from the tree,
		 * otherwise get_ksm_page() might later try to access a
		 * non-existent struct page.
		 */
		ksm_check_stable_tree(mn->start_pfn,
				      mn->start_pfn + mn->nr_pages);
		/* fallthrough */

	case MEM_CANCEL_OFFLINE:
		mutex_lock(&ksm_thread_mutex);
		ksm_run &= ~KSM_RUN_OFFLINE;
		mutex_unlock(&ksm_thread_mutex);

		smp_mb();	/* wake_up_bit advises this */
		wake_up_bit(&ksm_run, ilog2(KSM_RUN_OFFLINE));
		break;
	}
	return NOTIFY_OK;
}
#else
static void wait_while_offlining(void)
{
}
#endif /* CONFIG_MEMORY_HOTREMOVE */

#ifdef CONFIG_SYSFS
/*
 * This all compiles without CONFIG_SYSFS, but is a waste of space.
 */

#define KSM_ATTR_RO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RO(_name)
#define KSM_ATTR(_name) \
	static struct kobj_attribute _name##_attr = \
		__ATTR(_name, 0644, _name##_show, _name##_store)

static ssize_t sleep_millisecs_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", ksm_thread_sleep_millisecs);
}

static ssize_t sleep_millisecs_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned long msecs;
	int err;

	err = kstrtoul(buf, 10, &msecs);
	if (err || msecs > UINT_MAX)
		return -EINVAL;

	ksm_thread_sleep_millisecs = msecs;

	return count;
}
KSM_ATTR(sleep_millisecs);

static ssize_t pages_to_scan_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", ksm_thread_pages_to_scan);
}

static ssize_t pages_to_scan_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int err;
	unsigned long nr_pages;

	err = kstrtoul(buf, 10, &nr_pages);
	if (err || nr_pages > UINT_MAX)
		return -EINVAL;

	ksm_thread_pages_to_scan = nr_pages;

	return count;
}
KSM_ATTR(pages_to_scan);

static ssize_t run_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%lu\n", ksm_run);
}

// hz ksm
static void deletelist(struct list_head *head)
{
	struct gpa_node *delete_node, *next;

	list_for_each_entry_safe(delete_node, next, head, link) {
		struct rmap_item *rmap_item;
		struct hlist_node *node;

		hlist_for_each_entry_safe(rmap_item, node, &delete_node->hlist, gfnhlist) {
			hlist_del(&rmap_item->gfnhlist);
		}

		list_del(&delete_node->link);
		free_gpa_node(delete_node);
	}
}

/*for new ksm*/
static void clean_gpa_node_list(void)
{
	hotzone_used = 0;
	define_prescan = 0;
	scan_hot_zone = 0;
	scan_remain = 0;
	ksm_time = 0, break_time = 0, time_flag = 0, print_vma = 0;
	h1=0,h2=0,h3=0,h4=0,h5=0,h6=0,h7=0,h8=0,h9=0,h10=0,h11=0,h12=0,h13=0,h14=0,h15=0,h16=0,h17=0,h18=0,h19=0;
	hz_size = 0, total_rmap = 0;
	cursor = NULL;
	struct list_head *head;

	head = &gpa_node_head1;
	deletelist(head);
	head = &gpa_node_head1a;
	deletelist(head);
	head = &gpa_node_head1b;
	deletelist(head);
	head = &gpa_node_head1c;
	deletelist(head);
	head = &gpa_node_head2;
	deletelist(head);
	head = &gpa_node_head2a;
	deletelist(head);
	head = &gpa_node_head2b;
	deletelist(head);
	head = &gpa_node_head2c;
	deletelist(head);
	head = &gpa_node_head3;
	deletelist(head);
	head = &gpa_node_head3a;
	deletelist(head);
	head = &gpa_node_head3b;
	deletelist(head);
	head = &gpa_node_head3c;
	deletelist(head);
	head = &gpa_node_head4;
	deletelist(head);
	head = &gpa_node_head4a;
	deletelist(head);
	head = &gpa_node_head4b;
	deletelist(head);
	head = &gpa_node_head4c;
	deletelist(head);
	head = &gpa_node_head5;
	deletelist(head);
	head = &gpa_node_head5a;
	deletelist(head);
	head = &gpa_node_head5b;
	deletelist(head);
	head = &gpa_node_head5c;
	deletelist(head);
	head = &gpa_node_head6;
	deletelist(head);
	head = &gpa_node_head6a;
	deletelist(head);
	head = &gpa_node_head6b;
	deletelist(head);
	head = &gpa_node_head6c;
	deletelist(head);
	head = &gpa_node_head7;
	deletelist(head);
	head = &gpa_node_head7a;
	deletelist(head);
	head = &gpa_node_head7b;
	deletelist(head);
	head = &gpa_node_head7c;
	deletelist(head);
	head = &gpa_node_head8;
	deletelist(head);
	head = &gpa_node_head8a;
	deletelist(head);
	head = &gpa_node_head8b;
	deletelist(head);
	head = &gpa_node_head8c;
	deletelist(head);
	head = &gpa_node_head9;
	deletelist(head);
	head = &gpa_node_head9a;
	deletelist(head);
	head = &gpa_node_head9b;
	deletelist(head);
	head = &gpa_node_head9c;
	deletelist(head);
	head = &gpa_node_head10;
	deletelist(head);
	head = &gpa_node_head10a;
	deletelist(head);
	head = &gpa_node_head10b;
	deletelist(head);
	head = &gpa_node_head10c;
	deletelist(head);
	head = &gpa_node_head11;
	deletelist(head);
	head = &gpa_node_head11a;
	deletelist(head);
	head = &gpa_node_head11b;
	deletelist(head);
	head = &gpa_node_head11c;
	deletelist(head);
	head = &gpa_node_head12;
	deletelist(head);
	head = &gpa_node_head12a;
	deletelist(head);
	head = &gpa_node_head12b;
	deletelist(head);
	head = &gpa_node_head12c;
	deletelist(head);
	head = &gpa_node_head13;
	deletelist(head);
	head = &gpa_node_head13a;
	deletelist(head);
	head = &gpa_node_head13b;
	deletelist(head);
	head = &gpa_node_head13c;
	deletelist(head);
	head = &gpa_node_head14;
	deletelist(head);
	head = &gpa_node_head14a;
	deletelist(head);
	head = &gpa_node_head14b;
	deletelist(head);
	head = &gpa_node_head14c;
	deletelist(head);
	head = &gpa_node_head15;
	deletelist(head);
	head = &gpa_node_head15a;
	deletelist(head);
	head = &gpa_node_head15b;
	deletelist(head);
	head = &gpa_node_head15c;
	deletelist(head);
	head = &gpa_node_head16;
	deletelist(head);
	head = &gpa_node_head16a;
	deletelist(head);
	head = &gpa_node_head16b;
	deletelist(head);
	head = &gpa_node_head16c;
	deletelist(head);
	head = &gpa_node_head17;
	deletelist(head);
	head = &gpa_node_head17a;
	deletelist(head);
	head = &gpa_node_head17b;
	deletelist(head);
	head = &gpa_node_head17c;
	deletelist(head);
	head = &gpa_node_head18;
	deletelist(head);
	head = &gpa_node_head18a;
	deletelist(head);
	head = &gpa_node_head18b;
	deletelist(head);
	head = &gpa_node_head18c;
	deletelist(head);
	head = &gpa_node_head19;
	deletelist(head);
	head = &gpa_node_head19a;
	deletelist(head);
	head = &gpa_node_head19b;
	deletelist(head);
	head = &gpa_node_head19c;
	deletelist(head);
	head = &gpa_node_head20;
	deletelist(head);
	head = &gpa_node_head20a;
	deletelist(head);
	head = &gpa_node_head20b;
	deletelist(head);
	head = &gpa_node_head20c;
	deletelist(head);

	head = &hot_zone_node;
	deletelist(head);
	head = &rest_gpa_node_head;
	deletelist(head);

}


static ssize_t run_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	int err;
	unsigned long flags;

	err = kstrtoul(buf, 10, &flags);
	if (err || flags > UINT_MAX)
		return -EINVAL;
	if (flags > KSM_RUN_UNMERGE)
		return -EINVAL;

	/*
	 * KSM_RUN_MERGE sets ksmd running, and 0 stops it running.
	 * KSM_RUN_UNMERGE stops it running and unmerges all rmap_items,
	 * breaking COW to free the pages_shared (but leaves mm_slots
	 * on the list for when ksmd may be set running again).
	 */

	mutex_lock(&ksm_thread_mutex);
	wait_while_offlining();
	if (ksm_run != flags) {
		ksm_run = flags;
		if (flags & KSM_RUN_UNMERGE) {
			set_current_oom_origin();
			printk("In run_store, where we turned off ksm:\n");
			err = unmerge_and_remove_all_rmap_items();
			clean_gpa_node_list();/*new ksm clean function*/
			clear_current_oom_origin();
			if (err) {
				ksm_run = KSM_RUN_STOP;
				count = err;
			}
		}
	}
	mutex_unlock(&ksm_thread_mutex);

	if (flags & KSM_RUN_MERGE)
		wake_up_interruptible(&ksm_thread_wait);

	return count;
}
KSM_ATTR(run);

#ifdef CONFIG_NUMA
static ssize_t merge_across_nodes_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", ksm_merge_across_nodes);
}

static ssize_t merge_across_nodes_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int err;
	unsigned long knob;

	err = kstrtoul(buf, 10, &knob);
	if (err)
		return err;
	if (knob > 1)
		return -EINVAL;

	mutex_lock(&ksm_thread_mutex);
	wait_while_offlining();
	if (ksm_merge_across_nodes != knob) {
		if (ksm_pages_shared || remove_all_stable_nodes())
			err = -EBUSY;
		else if (root_stable_tree == one_stable_tree) {
			struct rb_root *buf;
			/*
			 * This is the first time that we switch away from the
			 * default of merging across nodes: must now allocate
			 * a buffer to hold as many roots as may be needed.
			 * Allocate stable and unstable together:
			 * MAXSMP NODES_SHIFT 10 will use 16kB.
			 */
			buf = kcalloc(nr_node_ids + nr_node_ids, sizeof(*buf),
				      GFP_KERNEL);
			/* Let us assume that RB_ROOT is NULL is zero */
			if (!buf)
				err = -ENOMEM;
			else {
				root_stable_tree = buf;
				root_unstable_tree = buf + nr_node_ids;
				/* Stable tree is empty but not the unstable */
				root_unstable_tree[0] = one_unstable_tree[0];
			}
		}
		if (!err) {
			ksm_merge_across_nodes = knob;
			ksm_nr_node_ids = knob ? 1 : nr_node_ids;
		}
	}
	mutex_unlock(&ksm_thread_mutex);

	return err ? err : count;
}
KSM_ATTR(merge_across_nodes);
#endif

static ssize_t pages_shared_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_pages_shared);
}
KSM_ATTR_RO(pages_shared);

static ssize_t pages_sharing_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_pages_sharing);
}
KSM_ATTR_RO(pages_sharing);

static ssize_t pages_unshared_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_pages_unshared);
}
KSM_ATTR_RO(pages_unshared);

static ssize_t pages_volatile_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	long ksm_pages_volatile;

	ksm_pages_volatile = ksm_rmap_items - ksm_pages_shared
				- ksm_pages_sharing - ksm_pages_unshared;
	/*
	 * It was not worth any locking to calculate that statistic,
	 * but it might therefore sometimes be negative: conceal that.
	 */
	if (ksm_pages_volatile < 0)
		ksm_pages_volatile = 0;
	return sprintf(buf, "%ld\n", ksm_pages_volatile);
}
KSM_ATTR_RO(pages_volatile);

static ssize_t full_scans_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ksm_scan.seqnr);
}
KSM_ATTR_RO(full_scans);

static struct attribute *ksm_attrs[] = {
	&sleep_millisecs_attr.attr,
	&pages_to_scan_attr.attr,
	&run_attr.attr,
	&pages_shared_attr.attr,
	&pages_sharing_attr.attr,
	&pages_unshared_attr.attr,
	&pages_volatile_attr.attr,
	&full_scans_attr.attr,
#ifdef CONFIG_NUMA
	&merge_across_nodes_attr.attr,
#endif
	NULL,
};

static struct attribute_group ksm_attr_group = {
	.attrs = ksm_attrs,
	.name = "ksm",
};
#endif /* CONFIG_SYSFS */

static int __init ksm_init(void)
{
	struct task_struct *ksm_thread;
	int err;

	err = ksm_slab_init();
	if (err)
		goto out;

	ksm_thread = kthread_run(ksm_scan_thread, NULL, "ksmd");
	if (IS_ERR(ksm_thread)) {
		pr_err("ksm: creating kthread failed\n");
		err = PTR_ERR(ksm_thread);
		goto out_free;
	}

#ifdef CONFIG_SYSFS
	err = sysfs_create_group(mm_kobj, &ksm_attr_group);
	if (err) {
		pr_err("ksm: register sysfs failed\n");
		kthread_stop(ksm_thread);
		goto out_free;
	}
#else
	ksm_run = KSM_RUN_MERGE;	/* no way for user to start it */

#endif /* CONFIG_SYSFS */

#ifdef CONFIG_MEMORY_HOTREMOVE
	/* There is no significance to this priority 100 */
	hotplug_memory_notifier(ksm_memory_callback, 100);
#endif
	return 0;

out_free:
	ksm_slab_free();
out:
	return err;
}
subsys_initcall(ksm_init);
