#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>

#include "avl.h"
#include "mleak.h"
#include "mdump.h"

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
	return (char *)m1->ptr - (char *)m2->ptr;
}

static int
md_index_ptr_cmp(void *ptr, MD_Mem *m)
{
	return (char *)ptr - (char *)m->ptr;
}

/* search for the given pointer in the list (starting at the end
		because a same address can be re-given by 'malloc' after
		beeing freed, so we must be sure that it is the LAST one).
	 Need to search into 'reallocs' because the pointer can change */
static Avlnode *md_search_pointer(void *ptr)
{
	return avl_find(md_mems, ptr, (AVL_CMP) md_index_ptr_cmp);
}

static void md_update_heap(void *ptr, unsigned int size)
{
	md_heap_start = MD_MIN(md_heap_start, ptr);
	md_heap_end	 = MD_MAX(md_heap_end,	 ptr+size);
	md_nb_heap_size = md_heap_end - md_heap_start;
}

/* add an invalid entry */
static void md_add_error(Avlnode *av, void *where, void *ptr, void *newptr, unsigned int size,
									 MD_Loc *stack, unsigned int nb)
{
	MD_Mem *me = malloc(sizeof(*me));

	memset(me, 0, sizeof(*me));
	me->ptr = ptr;

	me->block = blocks++;
	me->where_f = where;
	me->stack_f = stack;
	me->nb_stack_f = nb;

	me->func_a = newptr;
	me->size_a = size;
	if (!av)
		avl_insert( &md_mems, me, (AVL_CMP)md_index_cmp, avl_dup_error );
	else
		{
		me->anext = av->avl_data;
		av->avl_data = me;
		}
}


/* add new malloc entry */
static MD_Mem *md_add_malloc(Avlnode *av, ml_rec2 *mr, MD_Loc *stack)
{
	MD_Mem *me = malloc(sizeof(*me));

	memset(me, 0, sizeof(*me));
	me->ptr = mr->addr;

	me->block = blocks++;
	me->where_a = stack->addr;
	me->size_a = mr->size;
	me->stack_a = stack;
	me->nb_stack_a = mr->nstk-1;

	if (!av)
		av = md_search_pointer(mr->addr);
	if (!av)
		avl_insert( &md_mems, me, (AVL_CMP)md_index_cmp, avl_dup_error );
	else
		{
		me->anext = av->avl_data;
		av->avl_data = me;
		}

	/* the current memory size */
	md_nb_mem_used += mr->size;
	/* store the MAX of memory allocated */
	md_nb_mem_max = MD_MAX(md_nb_mem_used, md_nb_mem_max);
	/* compute the memory block size */
	md_update_heap(mr->addr, mr->size);
	return me;
}

static void md_add_free(int options, ml_rec *mr, MD_Loc *stack)
{
	MD_Mem *me;
	Avlnode *av;

	if (!(av = md_search_pointer(mr->addr)))
		{
		md_add_error(NULL, stack->addr, mr->addr, NULL, 0, stack, mr->nstk-1);
		return;
		}
	me = av->avl_data;
	/* this pointer was freed !!! */
	if (me->where_f)
		{
		md_add_error(av, stack->addr, mr->addr, NULL, 0, stack, mr->nstk-1);
		return;
		}
	me->where_f = stack->addr;
	/* remove this block from total */
	md_nb_mem_used -= me->size_a;
	if (options & MD_MORE )
		{
		me->stack_f = stack;
		me->nb_stack_f = mr->nstk-1;
		}
	else
		{
		MD_Mem *m2, *pre = NULL;
		/* Dispose of call stack, we don't need it for non-leaks */
		free(stack);
		me->stack_f = NULL;
		me->nb_stack_f = 0;
		if (me->stack_a)
			{
			free(me->stack_a);
			me->stack_a = NULL;
			}
		me->nb_stack_a = 0;
		/* Dispose of all previous call stacks. */
		for (me = me->rprev; me; me=me->rprev)
			{
			if (!me->where_a)
				continue;
			free(me->stack_a);
			me->stack_a = NULL;
			me->nb_stack_a = 0;
			}
		for (me = av->avl_data; me; me=m2)
		{
			m2 = me->rprev;
			if (!me->anext && !pre)
				avl_delete( &md_mems, me->ptr, (AVL_CMP)md_index_ptr_cmp );
			else if (pre)
				pre->anext = me->anext;
			else
				av->avl_data = me->anext;
			if (m2 && m2 != me->anext)
			{
				free(me);
				av = md_search_pointer(m2->ptr);
	pre = NULL;
	for (me = av->avl_data; me; pre=me,me=me->anext)
		if (me == m2)
			break;
			} else
			{
				free(me);
			}
		}
	}
}
static void md_add_realloc(ml_rec3 *mr, MD_Loc *stack)
{
	MD_Mem *me;
	Avlnode *av;

	if (!mr->orig)
		{
		md_add_malloc(NULL, (ml_rec2 *)mr, stack);
		return;
		}
	if (!(av = md_search_pointer(mr->orig)))
		{
		md_add_error(NULL, stack->addr, mr->orig, mr->addr, mr->size, stack, mr->nstk-1);
		return;
		}
	me = av->avl_data;
	/* this pointer was freed !!! */
	if (me->where_f)
		{
		md_add_error(av, stack->addr, mr->orig, NULL, 0, stack, mr->nstk-1);
		return;
		}
	/* remove this block from total */
	md_nb_mem_used -= me->size_a;

	me->rnext = md_add_malloc(mr->orig == mr->addr ? av : NULL, (ml_rec2 *)mr, stack);
	me->rnext->rprev = me;
}

static MD_Loc *md_expand_stack(int nstk, void **stk)
{
	MD_Loc *ml = malloc(nstk * sizeof(MD_Loc));
	int i;
	stk++;
	nstk--;
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
	ml_info *mi;
	ml_rec *mr;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		perror("open");
		exit(1);
	}
	fstat(fd, &st);
	mi = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);

	/* read the file and generate memory datas */
	mr = (ml_rec *)mi->mi_data;
	while (mr->code) {
		void **stk;
		switch(mr->code) {
		case FREE:
			stk = (void **)(mr+1);
			cstack = md_expand_stack(mr->nstk, stk);
			md_add_free(options, mr, cstack);
			break;
			
		case ALLOC:	{ ml_rec2 *m2 = (ml_rec2 *)mr;
			stk = (void **)(m2+1);
			cstack = md_expand_stack(mr->nstk, stk);
			md_add_malloc(NULL, m2, cstack);
			}
			break;

		case REALLOC: { ml_rec3 *m2 = (ml_rec3 *)mr;
			stk = (void **)(m2+1);
			cstack = md_expand_stack(mr->nstk, stk);
			md_add_realloc(m2, cstack);
			}
			break;
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