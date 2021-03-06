/*
 * dd_event.c
 *
 * A holding area while we wait for all messages for each event to arrive
 * before distributing them.
 * */

#include "dedup_hash.h"
#include "dedup_event.h"
#include "avltree.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

#include "dedup.h"

#define CBUF_SIZE 10000

static Element cbuf[CBUF_SIZE];
static Element* unused_head = NULL; // ptr to first unused element
static Element* unused_tail = NULL; // ptr to last unused element

static AVL_Tree *tree = NULL;

#define INCREMENT_INDEX(i) \
  i = ((++i) % CBUF_SIZE);

#define REP_IS_EMPTY( e, r ) \
  ( e->reps[r] == NULL )

#define REP_HASH( e, r, c ) \
  dd_sieve_sha1( e->reps[r]->reply.msg.data, e->reps[r]->reply.type, c );

#define SET_REP( e, r, rep ) \
  e->reps[r] = rep;

#define FREE_REP( e, r ) \
  { free( e->reps[r] ); e->reps[r] = NULL; }

#define FPRINTF( e, r) \
  if ( !REP_IS_EMPTY( e, r ) )  { \
    fprintf( e->log_file, "%s\n", e->reps[r]->reply.msg.data );   \
    fflush( e->log_file );        \
    FREE_REP( e, r );             \
  }

//---------------------------------------
// Functions to parse messages for info.
//---------------------------------------

/*
 * Extracts the sequence number for message s.
 * */
static unsigned dd_extract_seq_num(const char* s)
{
  register char c;
  register unsigned n = 0;

  // We know we can skip at least 33 characters (before reaching the colon ':')
  // because the shortest prefix will be something like the following:
  // type=CWD msg=audit(0123456789.012:1):
  s += 33;
  while (*s++ != ':');
  while ((c = *s++) != ')') {
    n = ((n << 3) + (n << 1)) + (c - '0');
  }
  return n;
}

/*
 * Extracts value for field "items". Applicable only for SYSCALL types.
 */
static int dd_extract_items(const char* s)
{
  char* p = strstr((char*)s, "items=") + 6;
  return (*p - '0');
}



//------------------------------------------
// Functions for accessing circular buffer.
//------------------------------------------

int eventCompare( void* v, const Element* e1, const Element* e2 )
{
  //dd_printf("Comparing %lu and %lu\n", e1->seq_num, e2->seq_num);
  if (e1->seq_num < e2->seq_num) return -1;
  if (e1->seq_num == e2->seq_num) return 0;
  return 1;
}

int dd_init_events()
{
  int i;

  tree = AVL_Tree_alloc2(NULL, eventCompare, malloc, free);
  if (tree == NULL) return -1;

  //printf("1: Init %d bufs\n", count_unused_elements());

  // Initialize unused element pointers
  for (i = 0; i < (CBUF_SIZE - 1); ++i)
    cbuf[i].next_unused = &cbuf[i+1];
  cbuf[i].next_unused = NULL;
  unused_head = &cbuf[0];
  unused_tail = &cbuf[CBUF_SIZE - 1];
  return 0;
} 

//-----------------------------------------
// Free List
//-----------------------------------------


int count_unused_elements()
{
  Element* h = unused_head;
  Element* t = unused_tail;
  int i = 0;

  while( h != NULL ) {
    h = h->next_unused;
    i++;
  }
  return i;
}


Element* get_unused_element()
{
  Element* e = unused_head;
  if (unused_head != NULL)
  {
    if (unused_head == unused_tail)
    { // end of list
      unused_head = NULL;
      unused_tail = NULL;
    }
    else
    {
      unused_head = unused_head->next_unused;
    }
  }
  return e;
}


void put_unused_element(Element* e)
{
  if (unused_head == NULL)
  { // empty list
    unused_head = e;
  }

  if (unused_tail != NULL)
  {
    unused_tail->next_unused = e;
  }

  unused_tail = e;
  e->next_unused = NULL;
}



/*
 * Add event data to the end of the circular buffer (for bounding)
 * and to the AVL tree (for searching).
 * */
static Element* dd_add_new_event(
    const unsigned long seq_num,
    FILE* log_file )
{
  Element* e = get_unused_element();
  if (e == NULL) return NULL;

  e->seq_num  = seq_num;
  e->items    = -1;
  //e->hash     = 5381; // initial hash value
  memset( e->sha1, SHA_DIGEST_LENGTH * sizeof(unsigned char), 0x0 );
  e->log_file = log_file;
  SET_REP( e, RT_SYSCALL, NULL );
  SET_REP( e, RT_EXECVE,  NULL );
  SET_REP( e, RT_CWD,     NULL );
  SET_REP( e, RT_PATH1,   NULL );
  SET_REP( e, RT_PATH2,   NULL ) ;

  AVL_insert( e, tree );

  return e;
}


/*
 * Returns pointer to event keyed by seq_num.
 * */
static Element* dd_find_event( const unsigned long seq_num )
{
  Element e;
  e.seq_num = seq_num;
  //dd_printf("Finding %lu\n", e.seq_num);
  return AVL_find( &e, tree );
}


/*
 * Updates hash for COMPLETE event e.
 *
 * This also computes the hash.
 * */
static inline void dd_update_event_hash(
    Element*            e,
    struct audit_reply* reply )
{
  // Computes hash for rep->reply.message. The hash value is
  // compounded onto the input hash.

  int i;
  SHA_CTX c;
  SHA1_Init( &c );
  if ( !REP_IS_EMPTY( e, RT_SYSCALL ) ) REP_HASH( e, RT_SYSCALL, &c );
  if ( !REP_IS_EMPTY( e, RT_EXECVE  ) ) REP_HASH( e, RT_EXECVE, &c );
  if ( !REP_IS_EMPTY( e, RT_CWD     ) ) REP_HASH( e, RT_CWD, &c );
  if ( !REP_IS_EMPTY( e, RT_PATH1   ) ) REP_HASH( e, RT_PATH1, &c );
  if ( !REP_IS_EMPTY( e, RT_PATH2   ) ) REP_HASH( e, RT_PATH2, &c );
  SHA1_Final( e->sha1, &c );
}


/*
 * Sets the corresponding rep in event e.
 *
 * Returns -1 on error.
 * */
static inline int dd_update_event_rep( Element* e,
    struct auditd_reply_list* rep )
{
  switch (rep->reply.type)
  {
    case AUDIT_SYSCALL:
      if ( !REP_IS_EMPTY( e, RT_SYSCALL ) ) return -1;
      SET_REP( e, RT_SYSCALL, rep );
      return 0;
    case AUDIT_EXECVE:
      if ( !REP_IS_EMPTY( e, RT_EXECVE ) ) return -1;
      SET_REP( e, RT_EXECVE, rep );
      return 0;
    case AUDIT_CWD:
      if ( !REP_IS_EMPTY( e, RT_CWD ) ) return -1;
      SET_REP( e, RT_CWD, rep );
      return 0;
    case AUDIT_PATH:
      if ( REP_IS_EMPTY( e, RT_PATH1 ) )
      {
        SET_REP( e, RT_PATH1, rep );
        return 0;
      }
      else if ( REP_IS_EMPTY( e, RT_PATH2 ) )
      {
        SET_REP( e, RT_PATH2, rep );
        return 0;
      }
      else
        return -1;
    default:
      return -1;
  }

  return -1;
}


/*
 * Returns true if the event is complete, meaning we have all the
 * required rep messages.
 * */
static bool dd_is_complete_event(Element* e)
{
  if (e->items == -1) return false;

  // Simply test that all PATHs are present, since these are the
  // tail messages. The number of paths is specified by 'items'.
  if (e->items == 1)
    return !REP_IS_EMPTY( e, RT_PATH1 );
  else if (e->items == 2)
    return ((!REP_IS_EMPTY(e, RT_PATH1 )) &&
        (!REP_IS_EMPTY( e, RT_PATH2 )));

  // 'items' has to be 1 or 2. If it is some other value,
  // something is terribly wrong. In that case, we just return
  // true so that the reps can be distributed and we don't
  // continue to hold on to them.
  return true;
}


/*
 * Returns:
 * -1 on error. This implies rep was not processed successfully
 *    and caller should call distribute_event on it.
 * 0  if there are no errors but event is incomplete.
 * 1  if there are no errors and event is complete.
 * */
int dd_update_event(
    Element**                 e,
    FILE*                     log_file,
    struct auditd_reply_list* rep )
{
  const char* m = rep->reply.msg.data;
  int seq_num = dd_extract_seq_num( m );

  // Find the event for seq_num. If not found, add it.
  *e = dd_find_event( seq_num );
  if (*e == NULL) {
    *e = dd_add_new_event( seq_num, log_file );
    if (*e == NULL) {
      // out of buffer?
      dd_printf( "%d: OUT OF BUFFER\n", seq_num );
      return -1;
    }
  }

  // Update rep
  if (dd_update_event_rep( *e, rep ) == -1)
  {
    dd_fprintf_internal( *e );
    return -1;
  }

  if (rep->reply.type == AUDIT_SYSCALL) {
    (*e)->items = dd_extract_items( m );
    //printf("items=%d\n", (*e)->items);
  }

  // If event is incomplete, hold and return.
  if (dd_is_complete_event( *e ) == false)
    return 0;

  dd_update_event_hash( *e, &(rep->reply) );

  return 1;
}


/*
 * For removing duplicated events.
 * */
void dd_remove_event( Element* e )
{
  AVL_delete(e, tree);
  e->seq_num = 0;
  if (!REP_IS_EMPTY(e, RT_SYSCALL))  FREE_REP( e, RT_SYSCALL );
  if (!REP_IS_EMPTY(e, RT_EXECVE))   FREE_REP( e, RT_EXECVE );
  if (!REP_IS_EMPTY(e, RT_CWD))      FREE_REP( e, RT_CWD );
  if (!REP_IS_EMPTY(e, RT_PATH1))    FREE_REP( e, RT_PATH1 );
  if (!REP_IS_EMPTY(e, RT_PATH2))    FREE_REP( e, RT_PATH2 );
  put_unused_element( e );
}


/*
 * Reset element fields and distribute the reps if they
 * are non-null.
 * */
void dd_fprintf_internal( Element* e )
{
  AVL_delete(e, tree);
  e->seq_num = 0;
  FPRINTF( e, RT_SYSCALL );
  FPRINTF( e, RT_EXECVE  );
  FPRINTF( e, RT_CWD     );
  FPRINTF( e, RT_PATH1   );
  FPRINTF( e, RT_PATH2   );
  put_unused_element( e );
}

int event_destroy_visitor( void* v, Element* e )
{
  dd_fprintf_internal( e );
  return 0;
}

void dd_destroy_events()
{
  dd_printf("%d incompleted events.\n", AVL_Tree_size(tree));
  AVL_visit( NULL, tree, event_destroy_visitor );
  AVL_Tree_free( &tree );
}
