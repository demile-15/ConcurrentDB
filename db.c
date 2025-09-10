#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "./db.h"
#include "./comm.h"

#define MAXLEN 256

// The root node of the binary tree is never freed (it's allocated in the data region).
node_t head = {"", "", 0, 0, PTHREAD_RWLOCK_INITIALIZER};

//------------------------------------------------------------------------------------------------
// Constructor, destructor, and cleanup methods

/* Constructs a new node */
node_t *node_constructor(char *arg_key, char *arg_value, node_t *arg_left,
                         node_t *arg_right) {
    size_t key_len = strlen(arg_key);
    size_t val_len = strlen(arg_value);

    if (key_len > MAXLEN || val_len > MAXLEN)
        return 0;

    node_t *new_node = (node_t *)malloc(sizeof(node_t));

    if (new_node == NULL)
        return 0;

    if ((new_node->key = (char *)malloc(key_len + 1)) == NULL) {
        free(new_node);
        return 0;
    }
    if ((new_node->value = (char *)malloc(val_len + 1)) == NULL) {
        free(new_node->key);
        free(new_node);
        return 0;
    }

    if ((snprintf(new_node->key, MAXLEN, "%s", arg_key)) < 0) {
        free(new_node->value);
        free(new_node->key);
        free(new_node);
        return 0;
    }
    if ((snprintf(new_node->value, MAXLEN, "%s", arg_value)) < 0) {
        free(new_node->value);
        free(new_node->key);
        free(new_node);
        return 0;
    }

    new_node->lchild = arg_left;
    new_node->rchild = arg_right;

    int err;
    if ((err = pthread_rwlock_init(&new_node->lock, NULL))) {
        handle_error_en(err, "pthread_rwlock_init");
    }

    return new_node;
}

/* Destroys a node and frees up its allocated memory */
void node_destructor(node_t *node) {
    // Destroy the rwlock
    pthread_rwlock_destroy(&node->lock);

    // Free the node's resources
    if (node->key != NULL)
        free(node->key);
    if (node->value != NULL)
        free(node->value);
    free(node);
}

/* Recursively destroys node and all its children. */
void db_cleanup_recurs(node_t *node) {
    if (node == NULL) {
        return;
    }

    db_cleanup_recurs(node->lchild);
    db_cleanup_recurs(node->rchild);

    node_destructor(node);
}

void db_cleanup() {
    db_cleanup_recurs(head.lchild);
    db_cleanup_recurs(head.rchild);
}

//------------------------------------------------------------------------------------------------
// Database modifiers and accessors

node_t *search(char *key, node_t *parent, node_t **parentpp, enum locktype lt) {
    // parent is locked on entry
    node_t *next;
    if (strcmp(key, parent->key) < 0) {
        next = parent->lchild;
    } else {
        next = parent->rchild;
    }

    node_t *result;
    if (next == NULL) {
        result = NULL;
    } else {
        lock(lt, &next->lock);
        if (strcmp(key, next->key) == 0) {
            result = next;
        } else {
            pthread_rwlock_unlock(&parent->lock);
            return search(key, next, parentpp, lt);
        }
    }

    // At the end of the recursive function
    if (parentpp != NULL) {
        // parent remains locked
        *parentpp = parent;
    } else {
        // unlock the parent if the function doesn't care about it
        pthread_rwlock_unlock(&parent->lock);
    }

    return result;
}

void db_query(char *key, char *result, int len) {
    /*
     * Part 2: Make this thread safe!
     */
    lock(l_read, &head.lock);
    node_t *target = search(key, &head, NULL, l_read);
    if (target == NULL) {
        snprintf(result, len, "not found");
    } else {
        snprintf(result, len, "%s", target->value);
        pthread_rwlock_unlock(&target->lock);
    }
}

int db_add(char *key, char *value) {
    /*
     * Part 2: Make this thread safe!
     */
    node_t *parent;
    node_t *target;

    lock(l_write, &head.lock);

    // First, find the key in the bst. If it already exists, return 0.
    // The parent is saved to the parent ptr.
    if ((target = search(key, &head, &parent, l_write)) != NULL) {
        pthread_rwlock_unlock(&target->lock);
        pthread_rwlock_unlock(&parent->lock);
        return 0;
    }
    // Else, create a new node and attach it to the left/right of the parent.
    // The parent is currently locked.
    node_t *newnode = node_constructor(key, value, NULL, NULL);
    if (strcmp(key, parent->key) < 0)
        parent->lchild = newnode;
    else
        parent->rchild = newnode;
    pthread_rwlock_unlock(&parent->lock);

    return 1;
}

