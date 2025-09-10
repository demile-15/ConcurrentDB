#ifndef DB_H_
#define DB_H_

#include <pthread.h>

// Represent database as a binary tree
typedef struct node {
    char *key;
    char *value;
    struct node *lchild;
    struct node *rchild;
    pthread_rwlock_t lock;
} node_t;

extern node_t head;

enum locktype { l_read, l_write };

#define lock(lt, lk) \
    ((lt) == l_read) ? pthread_rwlock_rdlock(lk) : pthread_rwlock_wrlock(lk)

/**
 * Searches the database tree for a node containing the given key. 
 * Returns that node if it's found, otherwise NULL.
 */
node_t *search(char *key, node_t *parent, node_t **parentp, enum locktype lt);

/**
 * Retrieves the value of the node associated with the given key. 
 * If found, returns the value stored in that node in the given result buffer 
 * of the given size. Otherwise, result is filled with "not found".
 */
void db_query(char *key, char *result, int len);

/**
 * Adds a new node with the given key and value to the database if it hasn't
 * existed. Returns 1 on success and 0 on failure.
 */
int db_add(char *key, char *value);

/**
 * Retrieves and deletes the node with the given key.
 */
int db_remove(char *key);

/**
 * Gets called by the server to interpret a command from a client, 
 * call database functions, and store the response.
 */
void interpret_command(char *command, char *response, int resp_capacity);

/** Print the tree to a file. */
int db_print(char *filename);

/** Frees all dynamically-allocated nodes in the database before exiting. */
void db_cleanup(void);

#endif  // DB_H_
