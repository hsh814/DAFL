//
// Created by root on 3/11/24.
//

#ifndef DAFL_AFL_FUZZ_H
#define DAFL_AFL_FUZZ_H
#include "types.h"
#include "debug.h"

// For interval tree: should be power of 2
#define INTERVAL_SIZE 1024
#define MAX_SCHEDULER_NUM 16
#define MAX_QUEUE_U32_SIZE 12

struct proximity_score {
  u64 original;
  double adjusted;
  u32 covered;
  u32 *dfg_count_map; // Sparse map: [count]
  u32 *dfg_dense_map; // Dense map: [index, count]
};

struct dfg_node_info {
  u32 idx;
  u32 score;
  u32 max_paths;
};

enum AddQueueMode {
  ADD_QUEUE_DEFAULT = 0, // default: found new branch coverage
  ADD_QUEUE_UNIQUE_VAL = 1,
  ADD_QUEUE_UNIQUE_VAL_PER_PATH = 2,
  ADD_QUEUE_ALL = 3, // unique_val_per_path + default
  ADD_QUEUE_NONE = 4,
  ADD_QUEUE_UNIQUE_VAL_PER_PATH_IN_VER = 5,
  ADD_QUEUE_UNIQUE_VAL_PER_PATH_IN_VER_PLUS_DEF = 6,
};

enum ParetoStatus {
  PARETO_UNINITIALIZED = 0,
  PARETO_FRONTIER = 1,
  PARETO_DOMINATED = 2,
  PARETO_NEWLY_ADDED = 3,
  PARETO_RECYCLED = 4,
};

struct pareto_info {
  enum ParetoStatus status;
  u32 index;
};

void pareto_info_set(struct pareto_info *info, enum ParetoStatus status, u32 index) {
  info->status = status;
  info->index = index;
}

struct queue_entry {

  u8* fname;                          /* File name for the test case      */
  u32 len;                            /* Input length                     */

  u8  cal_failed,                     /* Calibration failed?              */
  trim_done,                      /* Trimmed?                         */
  was_fuzzed,                     /* Had any fuzzing done yet?        */
  handled_in_cycle,               /* Was handled in current cycle?    */
  passed_det,                     /* Deterministic stages passed?     */
  has_new_cov,                    /* Triggers new coverage?           */
  var_behavior,                   /* Variable behavior?               */
  favored,                        /* Currently favored?               */
  fs_redundant,                   /* Marked as redundant in the fs?   */
  removed,                        /* Removed from queue?              */
  base_crash_seed;                /* Part of the initial test case?   */

  u32 bitmap_size,                    /* Number of bits set in bitmap     */
  exec_cksum,                     /* Checksum of the execution trace  */
  dfg_cksum;
  s32 last_location;

  struct proximity_score prox_score;  /* Proximity score of the test case */
  u32 entry_id;                       /* The ID assigned to the test case */
  s32 rank_moo,                           /* Pareto rank of the test case     */
      rank_explore;                   /* Pareto rank for explore mode */
  u32 selection_count;                /* Number of times selected         */

  u64 exec_us,                        /* Execution time (us)              */
  handicap,                       /* Number of queue cycles behind    */
  depth;                          /* Path depth                       */

  u8* trace_mini;                     /* Trace bytes, if kept             */
  u32 tc_ref;                         /* Trace bytes ref count            */

  struct queue_entry *next;           /* Next element, if any             */

  struct pareto_info moo_info;        /* Pareto info for MOO mode */
  struct pareto_info explore_info;   /* Pareto info for explore mode */

};

u32 quantize_location(double loc) {
  return (u32)(loc * INTERVAL_SIZE);
}

// Binary tree
struct interval_node {
  u32 start;
  u32 end;
  u64 count;
  u64 score;
  struct interval_node *left;
  struct interval_node *right;
};

struct interval_tree {
  u64 count[INTERVAL_SIZE];
  u64 score[INTERVAL_SIZE];
  struct interval_node *root;
};

struct interval_node *interval_node_create(u32 start, u32 end);

void interval_node_free(struct interval_node *node);

double interval_tree_query(struct interval_tree *tree, struct interval_node *node);

double interval_node_ratio(struct interval_node *node);

struct interval_node *interval_node_insert(struct interval_tree *tree, struct interval_node *node, u32 key, u32 value);

struct interval_tree *interval_tree_create();

void interval_tree_free(struct interval_tree *tree);

void interval_tree_insert(struct interval_tree *tree, u32 key, u32 value);

u32 interval_node_select(struct interval_node *node);

u32 interval_tree_select(struct interval_tree *tree);

// Define the vector structure
struct vector {
  struct queue_entry **data;
  size_t size;     // Number of elements currently in the vector
  size_t capacity; // Capacity of the vector (allocated memory size)
};

// Function to initialize a new vector
struct vector* vector_create(void) {
  struct vector* vec = ck_alloc(sizeof(struct vector));
  if (vec == NULL) {
    printf("Memory allocation failed.\n");
    exit(EXIT_FAILURE);
  }
  vec->size = 0;
  vec->capacity = 0;
  vec->data = NULL;
  return vec;
}

struct vector* vector_clone(struct vector *vec) {
  struct vector *new_vec = vector_create();
  if (vec->size == 0) return new_vec;
  new_vec->size = vec->size;
  new_vec->capacity = vec->size + 1;
  new_vec->data = ck_alloc(new_vec->capacity * sizeof(struct queue_entry*));
  memcpy(new_vec->data, vec->data, vec->size * sizeof(struct queue_entry*));
  return new_vec;
}

void vector_clear(struct vector *vec) {
  vec->size = 0;
  memset(vec->data, 0, vec->capacity * sizeof(struct queue_entry*));
}

void vector_reduce(struct vector *vec) {
  size_t new_index = 0;
  for (u32 i = 0; i < vec->size; i++) {
    if (vec->data[i] != NULL) {
      vec->data[new_index] = vec->data[i];
      new_index++;
    }
  }
  vec->size = new_index;
}

// Function to add an element to the end of the vector
void push_back(struct vector* vec, struct queue_entry* element) {
  if (vec->size >= vec->capacity) {
    // Increase capacity by doubling it
    vec->capacity = (vec->capacity == 0) ? 8 : vec->capacity * 2;
    vec->data = (struct queue_entry**)ck_realloc(vec->data, vec->capacity * sizeof(struct queue_entry*));
    if (vec->data == NULL) {
      printf("Memory allocation failed.\n");
      exit(EXIT_FAILURE);
    }
  }
  vec->data[vec->size++] = element;
}

void vector_push_front(struct vector *vec, struct queue_entry *element) {
  push_back(vec, element);
  for (u32 i = vec->size - 1; i > 0; i--) {
    vec->data[i] = vec->data[i - 1];
  }
  vec->data[0] = element;
}

struct queue_entry * vector_pop_back(struct vector *vec) {
  if (vec->size == 0) return NULL;
  struct queue_entry *entry = vec->data[vec->size - 1];
  vec->size--;
  vec->data[vec->size] = NULL;
  return entry;
}

struct queue_entry *vector_pop(struct vector *vec, u32 index) {
  if (index >= vec->size) return NULL;
  if (index == vec->size - 1) return vector_pop_back(vec);
  struct queue_entry *entry = vec->data[index];
  for (u32 i = index; i < vec->size - 1; i++) {
    vec->data[i] = vec->data[i + 1];
  }
  vec->size--;
  return entry;
}

struct queue_entry * vector_pop_front(struct vector *vec) {
  return vector_pop(vec, 0);
}

void vector_free(struct vector* vec) {
  ck_free(vec->data);
  ck_free(vec);
}

struct queue_entry* vector_get(struct vector* vec, u32 index) {
  if (index >= vec->size) {
    return NULL;
  }
  return vec->data[index];
}

void vector_set(struct vector* vec, u32 index, struct queue_entry* element) {
  if (index < vec->size) {
    vec->data[index] = element;
  }
}

u32 vector_size(struct vector* vec) {
  return vec->size;
}

// Hashmap
struct key_value_pair {
  u32 key;
  void* value;
  struct key_value_pair* next;
};

struct hashmap {
  u32 size;
  u32 table_size;
  struct key_value_pair** table;
};

typedef void (*hashmap_iterate_fn)(u32 key, void* value);

struct hashmap* hashmap_create(u32 table_size) {
  struct hashmap* map = ck_alloc(sizeof(struct hashmap));
  if (map == NULL) {
    printf("Memory allocation failed.\n");
    exit(EXIT_FAILURE);
  }
  map->size = 0;
  map->table_size = table_size;
  map->table = ck_alloc(table_size * sizeof(struct key_value_pair*));
  if (map->table == NULL) {
    printf("Memory allocation failed.\n");
    exit(EXIT_FAILURE);
  }
  for (u32 i = 0; i < table_size; i++) {
    map->table[i] = NULL;
  }
  return map;
}

static u32 hashmap_fit(u32 key, u32 table_size) {
  return key % table_size;
}

static void hashmap_resize(struct hashmap *map) {

  u32 new_table_size = map->table_size * 2;
  struct key_value_pair **new_table = ck_alloc(new_table_size * sizeof(struct key_value_pair*));
  if (new_table == NULL) {
    printf("Memory allocation failed.\n");
    exit(EXIT_FAILURE);
  }
  for (int i = 0; i < map->table_size; i++) {
    struct key_value_pair* pair = map->table[i];
    while (pair != NULL) {
      struct key_value_pair *next = pair->next;
      u32 index = hashmap_fit(pair->key, new_table_size);
      pair->next = new_table[index];
      new_table[index] = pair;
      pair = next;
    }
  }
  ck_free(map->table);
  map->table = new_table;
  map->table_size = new_table_size;

}

u32 hashmap_size(struct hashmap* map) {
  return map->size;
}

// Function to insert a key-value pair into the hash map
void hashmap_insert(struct hashmap* map, u32 key, void* value) {
  u32 index = hashmap_fit(key, map->table_size);
  struct key_value_pair* newPair = ck_alloc(sizeof(struct key_value_pair));
  if (newPair == NULL) {
    printf("Memory allocation failed.\n");
    exit(EXIT_FAILURE);
  }
  newPair->key = key;
  newPair->value = value;
  newPair->next = map->table[index];
  map->table[index] = newPair;
  map->size++;
  if (map->size > map->table_size / 2) {
    hashmap_resize(map);
  }
}

void hashmap_remove(struct hashmap *map, u32 key) {
  u32 index = hashmap_fit(key, map->table_size);
  struct key_value_pair* pair = map->table[index];
  struct key_value_pair* prev = NULL;
  while (pair != NULL) {
    if (pair->key == key) {
      if (!prev) {
        map->table[index] = pair->next;
      } else {
        prev->next = pair->next;
      }
      map->size--;
      ck_free(pair);
      return;
    }
    prev = pair;
    pair = pair->next;
  }
}

struct key_value_pair* hashmap_get(struct hashmap* map, u32 key) {
  u32 index = hashmap_fit(key, map->table_size);
  struct key_value_pair* pair = map->table[index];
  while (pair != NULL) {
    if (pair->key == key) {
      return pair;
    }
    pair = pair->next;
  }
  return NULL;
}

void hashmap_iterate(struct hashmap *map, hashmap_iterate_fn func) {
  for (u32 i = 0; i < map->table_size; i++) {
    struct key_value_pair *pair = map->table[i];
    while (pair != NULL) {
      func(pair->key, pair->value);
      pair = pair->next;
    }
  }
}

void hashmap_free(struct hashmap* map) {
  for (u32 i = 0; i < map->table_size; i++) {
    struct key_value_pair* pair = map->table[i];
    while (pair != NULL) {
      struct key_value_pair* next = pair->next;
      ck_free(pair);
      pair = next;
    }
  }
  ck_free(map->table);
  ck_free(map);
}

enum VerticalMode {
  M_HOR = 0,    // Horizontal mode
  M_VER = 1,    // Vertical mode
  M_EXP = 2,    // Exploration mode
};

struct queue_u32 {
  u32 size;
  u32 front;
  u32 rear;
  u32 data[MAX_QUEUE_U32_SIZE];
};

struct queue_u32 *queue_u32_create() {
  struct queue_u32 *queue = ck_alloc(sizeof(struct queue_u32));
  queue->size = 0;
  queue->front = 0;
  queue->rear = 0;
  return queue;
}

void queue_u32_free(struct queue_u32 *queue) {
  ck_free(queue);
}

u32 queue_u32_dequeue(struct queue_u32 *queue) {
  if (queue->size == 0) {
    return 0;
  }
  u32 value = queue->data[queue->front];
  queue->front = (queue->front + 1) % 12;
  queue->size--;
  return value;
}

void queue_u32_enqueue(struct queue_u32 *queue, u32 value) {
  if (queue->size == MAX_QUEUE_U32_SIZE) {
    // Dequeue the front element
    queue_u32_dequeue(queue);
  }
  queue->data[queue->rear] = value;
  queue->rear = (queue->rear + 1) % MAX_QUEUE_U32_SIZE;
  queue->size++;
}

u32 queue_u32_size(struct queue_u32 *queue) {
  return queue->size;
}

u32 queue_u32_peek(struct queue_u32 *queue, u32 index) {
  if (index >= queue->size) {
    return 0;
  }
  return queue->data[(queue->front + index) % MAX_QUEUE_U32_SIZE];
}

double queue_u32_gradient(struct queue_u32 *queue, u32 last) {
  if (queue->size == 0)
    return 0.0;
  u32 front = queue_u32_peek(queue, 0);
  last = last > queue->size ? queue->size : last;
  u32 rear = queue_u32_peek(queue, last - 1);
  return (double)(rear - front) / (double)(last);
}

struct stride_scheduler {
  u32 stride_values[MAX_SCHEDULER_NUM];
  u32 stride_count[MAX_SCHEDULER_NUM];
  u32 found_count[MAX_SCHEDULER_NUM]; // Update if found new dug path, valuation.
  struct queue_u32 queue[MAX_SCHEDULER_NUM]; // Store the last N found_count
  u32 stride_size;
  u64 last_update;
  enum VerticalMode previous;
  enum VerticalMode current;
  u32 count_consecutive; // Count the number of consecutive selection of current
};

struct stride_scheduler *stride_scheduler_create();
enum VerticalMode stride_scheduler_get_mode(struct stride_scheduler *stride);
void stride_scheduler_update(struct stride_scheduler *stride, enum VerticalMode selected);
void stride_scheduler_reset(struct stride_scheduler *stride);
u8 stride_scheduler_check_update(struct stride_scheduler *stride);
enum VerticalMode stride_scheduler_get_mode_adaptive(struct stride_scheduler *stride);
u32 stride_scheduler_update_found_count(struct stride_scheduler *stride, u32 found);

struct vertical_entry {
  u32 hash;                   // dfg path hash
  u32 use_count;
  struct vector *entries;
  struct vector *old_entries;
  struct vertical_entry *next;
  struct hashmap *value_map;  // valuation hash
};

struct vertical_manager {
  struct hashmap *map; // path -> vertical_entry
  struct vertical_entry *head;
  struct vertical_entry *old;
  struct interval_tree *tree;

  u64 start_time;
  u8 dynamic_mode;
  u8 use_vertical;
};

struct vertical_entry *vertical_entry_create(u32 hash);

void vertical_entry_sorted_insert(struct vertical_manager *manager, struct vertical_entry *entry, u8 update);

void vertical_entry_add(struct vertical_manager *manager, struct vertical_entry *entry, struct queue_entry *q, struct key_value_pair *kvp);

struct vertical_manager *vertical_manager_create();

struct vertical_entry *vertical_manager_select_entry(struct vertical_manager *manager);

// Warning: This function has side effect
enum VerticalMode vertical_manager_select_mode(struct vertical_manager *manager);

// Same as above, but without side effect
enum VerticalMode vertical_manager_get_mode(struct vertical_manager *manager);

void vertical_manager_set_mode(struct vertical_manager *manager, u8 use_vertical) {
  manager->use_vertical = use_vertical;
}

void vertical_manager_insert_to_old(struct vertical_manager *manager, struct vertical_entry *entry, struct queue_entry *q) {
  if (manager->old == NULL) {
    manager->old = entry;
  } else {
    struct vertical_entry *ve = manager->old;
    while (ve->next != NULL) {
      ve = ve->next;
    }
    ve->next = entry;
  }
  push_back(entry->old_entries, q);
}

void vertical_manager_free(struct vertical_manager *manager) {
  if (manager == NULL) return;
  hashmap_free(manager->map);
  struct vertical_entry *entry = manager->head;
  while (entry != NULL) {
    struct vertical_entry *next = entry->next;
    vector_free(entry->entries);
    hashmap_free(entry->value_map);
    ck_free(entry);
    entry = next;
  }
  entry = manager->old;
  while (entry != NULL) {
    struct vertical_entry *next = entry->next;
    vector_free(entry->entries);
    hashmap_free(entry->value_map);
    ck_free(entry);
    entry = next;
  }
  interval_tree_free(manager->tree);
  ck_free(manager);
}


struct pareto_scheduler {
  // moo
  struct vector *moo_pareto_frontier;
  struct vector *moo_dominated;
  struct vector *moo_newly_added;
  struct vector *moo_recycled;
  // explore
  struct hashmap *count_dfg_path;
  struct vector *explore_pareto_frontier;
  struct vector *explore_dominated;
  struct vecotr *explore_newly_added;
  struct vector *explore_recycled;
};

struct pareto_scheduler *pareto_scheduler_create();

void pareto_scheduler_free(struct pareto_scheduler *scheduler);

struct queue_entry *pareto_scheduler_moo_pop(struct pareto_scheduler *scheduler);

void pareto_scheduler_moo_push(struct pareto_scheduler *scheduler, struct queue_entry *entry);

void pareto_scheduler_moo_remove(struct pareto_scheduler *scheduler, struct queue_entry *entry);

struct queue_entry *pareto_scheduler_explore_pop(struct pareto_scheduler *scheduler);

void pareto_scheduler_explore_push(struct pareto_scheduler *scheduler, struct queue_entry *entry);

void pareto_scheduler_explore_remove(struct pareto_scheduler *scheduler, struct queue_entry *entry);

void pareto_scheduler_update_dfg_count(struct pareto_scheduler *scheduler, u32 dfg_path) {
  if (!scheduler) return;
  struct key_value_pair *kvp = hashmap_get(scheduler->count_dfg_path, dfg_path);
  if (kvp) {
    kvp->value = (void*)((u64)kvp->value + 1);
  } else {
    hashmap_insert(scheduler->count_dfg_path, dfg_path, (void*)1);
  }
}

u64 pareto_scheduler_get_dfg_count(struct pareto_scheduler *scheduler, u32 dfg_path) {
  struct key_value_pair *kvp = hashmap_get(scheduler->count_dfg_path, dfg_path);
  if (kvp) {
    return (u64)kvp->value;
  }
  return 0;
}

void pareto_scheduler_push(struct pareto_scheduler *scheduler, struct queue_entry *entry);

#endif //DAFL_AFL_FUZZ_H
