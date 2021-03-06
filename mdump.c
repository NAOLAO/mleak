/* malloc tracer for memory leak tracking
 * This program reads the output generated by mleak.so
 * and prints the leak report.
 * -- Howard Chu, hyc@symas.com 2015-03-24
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "avl.h"
#include "mleak.h"
#include "mdump.h"

/* mdname.c */
extern int md_extract_names(int options, char *exec);

/* object files */
MD_DynObj **md_objects;
int md_nobjects;

/* blocks of memory */
Avlnode *md_mems;
unsigned int blocks;

/* memory count. after counting, this is the memory leak */
long md_nb_mem_used=0;
long md_nb_mem_max=0;	/* gives the real MAX memory used */
/* length of the memory used */
long md_nb_heap_size=0;

static void *md_heap_start=(void*)-1;
static void *md_heap_end=0;

static int
md_index_cmp(MD_Mem *m1, MD_Mem *m2)
{
	/* sort by stack depth first, then stack addrs */
	long l;
	int i;
	l = (int)m1->nb_stack_a - (int)m2->nb_stack_a;
	if (l)
		return l < 0 ? -1 : l > 0;
	l = (char *)m1->where_a - (char *)m2->where_a;
	if (l)
		return l < 0 ? -1 : l > 0;
	for (i=0; i<m1->nb_stack_a; i++) {
		l = (char *)m1->stack_a[i].addr - (char *)m2->stack_a[i].addr;
		if (l)
			return l < 0 ? -1 : l > 0;
	}
	return l;
}

/* search for the given record */
static Avlnode *md_search_pointer(MD_Mem *me)
{
	return avl_find(md_mems, me, (AVL_CMP) md_index_cmp);
}

static void md_update_heap(void *ptr, unsigned int size)
{
	md_heap_start = MD_MIN(md_heap_start, ptr);
	md_heap_end	 = MD_MAX(md_heap_end,	 ptr+size);
	md_nb_heap_size = md_heap_end - md_heap_start;
}

/* add new malloc entry */
static MD_Mem *md_add_malloc(Avlnode *av, ml_rec2 *mr, MD_Loc *stack)
{
	MD_Mem mt = {0};
	MD_Mem *me = &mt;

	me->ptr = mr->addr;

	me->block = 1;
	me->where_a = stack->addr;
	me->size_a = mr->size;
	me->stack_a = stack+1;
	me->nb_stack_a = mr->nstk-2;

	if (!av)
		av = md_search_pointer(me);
	if (!av) {
		me = calloc(1, sizeof(*me));
		*me = mt;
		avl_insert( &md_mems, me, (AVL_CMP)md_index_cmp, avl_dup_error );
		blocks++;
	} else {
		me = av->avl_data;
		me->block++;
		me->size_a += mr->size;
	}

	/* the current memory size */
	md_nb_mem_used += mr->size;
	/* store the MAX of memory allocated */
	md_nb_mem_max = MD_MAX(md_nb_mem_used, md_nb_mem_max);
	/* compute the memory block size */
	md_update_heap(mr->addr, mr->size);
	return me;
}


static MD_Loc *md_expand_stack(int nstk, void **stk)
{
	MD_Loc *ml;
	int i;
	stk++;	/* skip ml_backtrace stack frame */
	nstk--;
	ml = malloc(nstk * sizeof(MD_Loc));
	for (i=0; i<nstk; i++) {
		ml[i].addr = stk[i];
		ml[i].name = NULL;
		ml[i].file = NULL;
	}
	return ml;
}

/* read a memory trace file */
int md_read_memory_trace(int options, char *file)
{
	int fd;
	MD_Loc *cstack;
	struct stat st;
	ml_rec *mr;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		perror("open");
		exit(1);
	}
	fstat(fd, &st);
	mr = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	madvise(mr, st.st_size, MADV_SEQUENTIAL);
	close(fd);

	/* read the file and generate memory datas */
	while (mr->code) {
		void **stk;
		if (mr->code == ALLOC)	{
			ml_rec2 *m2 = (ml_rec2 *)mr;
			stk = (void **)(m2+1);
			cstack = md_expand_stack(mr->nstk, stk);
			md_add_malloc(NULL, m2, cstack);
		}
		mr = (ml_rec *)(stk + mr->nstk);
	}

	/* done */
	return(1);
}

static void
md_read_info(char *file)
{
	void *base;
	int len;
	int fd = open(file, O_RDONLY);

	while (read(fd, &base, sizeof(base)) == sizeof(base)) {
		MD_DynObj *mo;
		read(fd, &len, sizeof(len));
		mo = malloc(sizeof(*mo) + len);
		mo->base = base;
		read(fd, mo->path, len+1);
		mo->path[len] = '\0';
		md_objects = realloc(md_objects, (md_nobjects+1) * sizeof(MD_DynObj *));
		md_objects[md_nobjects++] = mo;
	}
	close(fd);
}

#include "mx.c"

int main(int argc, char *argv[])
{
	int i;
	char *exec = argv[1];
	
	for (i=2; i<argc; i++) {
		if (strcmp(argv[i], "ml.info"))
			md_read_memory_trace(0, argv[i]);
		else
			md_read_info(argv[i]);
	}
	md_extract_names(0, exec);
	md_display_leaks(MD_MEMORY_LINE);
}
