
#ifndef sph_queue_h_included
#define sph_queue_h_included

#include <stdlib.h>
#include <inttypes.h>
#include <stddef.h>
/* a fifo queue with the operations enqueue and dequeue that can enqueue custom struct types and a mix of types.
   # example usage
   typedef struct {
     // custom field definitions ...
     sph_queue-_node_t queue_node;
   } element_t;
   element_t e;
   sph_queue_t q;
   sph_queue_init(&q);
   sph_queue_enq(&q, &e.queue_node);
   sph_queue_get(queue_deq(&q), element_t, queue_node); */

#define sph_queue_size_t uint32_t

/** returns a pointer to the enqueued struct based on the offset of the sph_queue_node_t field in the struct.
     because of this, queue nodes do not have to be allocated separate from user data.
     downside is that the same user data object can not be contained multiple times.
     must only be called with a non-null pointer that points to the type */
#define sph_queue_get(node, type, field) ((type*)((((char*)(node)) - offsetof(type, field))))
struct sph_queue_node_t;
typedef struct sph_queue_node_t {
  struct sph_queue_node_t* next;
} sph_queue_node_t;
typedef struct {
  sph_queue_size_t size;
  sph_queue_node_t* first;
  sph_queue_node_t* last;
} sph_queue_t;
#endif
