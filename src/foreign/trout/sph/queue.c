
#ifndef sph_queue_c_included
#define sph_queue_c_included

/** initialize a queue */
void sph_queue_init(sph_queue_t* a) {
  a->first = 0;
  a->last = 0;
  a->size = 0;
}

/** enqueue a node. the node must not already be in the queue. the node must not be null */
void sph_queue_enq(sph_queue_t* a, sph_queue_node_t* node) {
  if (a->first) {
    a->last->next = node;
  } else {
    a->first = node;
  };
  a->last = node;
  node->next = 0;
  a->size = (1 + a->size);
}

/** must only be called when the queue is not empty. a.size can be used to check if the queue is empty */
sph_queue_node_t* sph_queue_deq(sph_queue_t* a) {
  sph_queue_node_t* n;
  n = a->first;
  if (!n->next) {
    a->last = 0;
  };
  a->first = n->next;
  a->size = (a->size - 1);
  return (n);
}
#endif
