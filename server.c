#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "./server.h"
#include "./comm.h"
#include "./db.h"

#define RESLEN 256
#define COMMAND_LEN 64
#define MAX_TOKENS 32

// Initialize global variables
client_t *thread_list_head = NULL;
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;
client_control_t client_control = {PTHREAD_MUTEX_INITIALIZER,
                                   PTHREAD_COND_INITIALIZER, 0};
server_control_t server_control = {PTHREAD_MUTEX_INITIALIZER,
                                   PTHREAD_COND_INITIALIZER, 0};
// Structure to manage the state of the server for accepting new clients
typedef struct server_accept {
    int state;  // state = 1 (accepting)
    pthread_mutex_t mutex;
} server_accept_t;
server_accept_t server_accept = {1, PTHREAD_MUTEX_INITIALIZER};

// A wrapper around pthread_mutex_unlock for pthread_cleanup_push
void cleanup_unlock_mutex(void *mutex) {
    pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

//------------------------------------------------------------------------------------------------
// Client threads' constructor and main method

/**
 * Called by listener (in comm.c) to create a new client thread
 * Param: cxstr, a file stream to initialize the cxstr field of the client
 * Return: void
 */
void client_constructor(FILE *cxstr) {
    client_t *client;
    pthread_t tid;
    int err;
    // Allocate memory for a new client_t struct
    if ((client = malloc(sizeof(client_t))) == 0) {
        perror("Unable to malloc space for a client");
        exit(1);
    }
    // Initialize client's fields
    client->cxstr = cxstr;
    client->next = NULL;
    client->prev = NULL;

    // Create a new client thread that runs `run_client`
    if ((err = pthread_create(&tid, 0, run_client, client))) {
        free(client);
        handle_error_en(err, "client pthread_create");
    }
    client->thread = tid;

    // Detach the new client thread
    if ((err = pthread_detach(tid))) {
        handle_error_en(err, "client pthread_detach");
    }
}

/**
 * Code executed by a client thread (to handle requests from/response to client)
 * Param: arg, a void * that is actually a pointer to the client struct
 * Return: void
 */
void *run_client(void *arg) {
    client_t *client = (client_t *)arg;

    // Check if the server still accepts clients before adding a client
    pthread_mutex_lock(&server_accept.mutex);
    if (server_accept.state == 0) {
        // If not, destroy the passed-in client and return
        client_destructor(client);
        pthread_mutex_unlock(&server_accept.mutex);
        return NULL;
    }
    pthread_mutex_unlock(&server_accept.mutex);

    // Initialize buffers for server's response and client's command
    char response[RESLEN];
    char command[COMMAND_LEN];
    memset(&response, 0, RESLEN);
    memset(&command, 0, COMMAND_LEN);

    // Safely add client to the beginning of the client list
    pthread_mutex_lock(&thread_list_mutex);
    if (thread_list_head == NULL) {
        thread_list_head = client;
    } else {
        client->next = thread_list_head;
        thread_list_head->prev = client;
        thread_list_head = client;
    }
    pthread_mutex_unlock(&thread_list_mutex);

    // Safely increment the number of active threads
    pthread_mutex_lock(&server_control.server_mutex);
    server_control.num_client_threads++;
    pthread_mutex_unlock(&server_control.server_mutex);

    // Push `thread_cleanup` as a cleanup_handler
    pthread_cleanup_push(thread_cleanup, client);
    // Loop to output the previous response and read in the client's next
    // command, until the client disconnects
    while (comm_serve(client->cxstr, response, command) != -1) {
        // Stop the client thread while the server is stopped
        client_control_wait();
        // Execute the command
        interpret_command(command, response, COMMAND_LEN);
    }
    pthread_cleanup_pop(1);  // Pop `thread_cleanup` when the user disconnects

    return NULL;
}

//------------------------------------------------------------------------------------------------
// Methods for client thread cleanup, destruction, and cancellation

/**
 * Called by thread_cleanup to free and close all resources associated with a
 * client
 * Param: client, a pointer to the passed-in client struct
 * Return: void
 */
void client_destructor(client_t *client) {
    // Close the client's file stream
    comm_shutdown(client->cxstr);
    // Free the client struct
    free(client);
}

/**
 * Cleanup routine for client threads, called on cancels and exit.
 * Param: arg, a void *, should be cast to client_t * that represents the passed
 * in client
 * Return: void
 */
void thread_cleanup(void *arg) {
    client_t *client = (client_t *)arg;

    // Remove the passed-in client from the client list
    pthread_mutex_lock(&thread_list_mutex);
    if (client->next != NULL) {
        client->next->prev = client->prev;
    }
    if (client->prev != NULL) {
        client->prev->next = client->next;
    }
    if (client == thread_list_head) {
        thread_list_head = client->next;
    }
    pthread_mutex_unlock(&thread_list_mutex);

    // Decrement the number of active threads
    pthread_mutex_lock(&server_control.server_mutex);
    server_control.num_client_threads--;
    // Wake up the listener thread waiting on server_cond if there are no active
    // clients left
    if (server_control.num_client_threads == 0) {
        pthread_cond_broadcast(&server_control.server_cond);
    }
    pthread_mutex_unlock(&server_control.server_mutex);

    // Destroy the passed-in client
    client_destructor(client);
}

/**
 * Cancel every thread in the client thread list with using `pthread_cancel`
 */
void delete_all() {
    int err;
    // Iterate through the client list and cancel each client thread
    pthread_mutex_lock(&thread_list_mutex);
    client_t *current = thread_list_head;
    while (current != NULL) {
        if ((err = pthread_cancel(current->thread))) {
            handle_error_en(err, "pthread_cancel");
        }
        current = current->next;
    }
    pthread_mutex_unlock(&thread_list_mutex);
}

//------------------------------------------------------------------------------------------------
/**
 * Parses command line input
 * Params:
 *      - buffer: a buffer that stores the string to be parsed
 *      - tokens: a pointer to an array that stores the parsed tokens
 * Return: void
 */
void parse(char buffer[COMMAND_LEN], char *tokens[MAX_TOKENS]) {
    char *buf = buffer;
    char *token;
    int i = 0;  // to index into tokens
    while ((token = strtok(buf, " \t\n")) != NULL) {
        tokens[i++] = token;
        buf = NULL;
    }
    tokens[i] = NULL;  // Null-terminate the tokens array
}

//------------------------------------------------------------------------------------------------
// Methods for stop/go server commands

// Called by client threads to wait until progress is permitted
void client_control_wait() {
    // Lock the mutex to check the state of client_control conditional variable
    pthread_mutex_lock(&client_control.go_mutex);

    // Push the unlock_mutex wrapper to ensure the mutex is unlocked safely
    pthread_cleanup_push(cleanup_unlock_mutex, &client_control.go_mutex);

    while (client_control.stopped) {
        // Block the calling thread until `client_control_release` is called
        pthread_cond_wait(&client_control.go, &client_control.go_mutex);
    }
    // Pop and execute cleanup handler to release the lock in case the thread
    // got canceled in cond_wait
    pthread_cleanup_pop(1);
}

// Called by main thread to stop client threads
void client_control_stop() {
    /*
     * Update the cond_var to ensure that the next time client threads call
     * `client_control_wait` in `run_client`, they will block.
     */
    pthread_mutex_lock(&client_control.go_mutex);
    client_control.stopped = 1;
    pthread_mutex_unlock(&client_control.go_mutex);
}

// Called by main thread to resume client threads
void client_control_release() {
    // Release the cond_var, allowing blocked clients to continue
    pthread_mutex_lock(&client_control.go_mutex);
    client_control.stopped = 0;
    pthread_cond_broadcast(&client_control.go);
    pthread_mutex_unlock(&client_control.go_mutex);
}

//------------------------------------------------------------------------------------------------
// SIGINT signal handling

// Code executed by the signal handler thread. 'man 7 signal' and 'man sigwait'
// are both helpful for implementing this function.
// All of the server's client threads should terminate on SIGINT; the server
// (this includes the listener thread), however, should not!

/**
 * Routine to wait for SIGINT and cancel all threads
 * Param: arg, pointer to the sig_handler_t struct
 * Return: NULL
 */
void *monitor_signal(void *arg) {
    sig_handler_t *sig = (sig_handler_t *)arg;
    int err, sig_number;
    // Continually wait for a SIGINT to be sent to the server process
    while (1) {
        if ((err = sigwait(&sig->set, &sig_number))) {
            handle_error_en(err, "sigwait");
        }
        // When receives SIGINT, print a message
        if (printf("SIGINT received, cancelling all clients\n") < 0) {
            fprintf(stderr, "error printing SIGINT message\n");
            exit(1);
        }
        // Cancel all client threads
        delete_all();
    }

    return NULL;
}

/**
 * Creates a thread to handle SIGINT
 */
sig_handler_t *sig_handler_constructor() {
    int err;
    pthread_t tid;
    sigset_t set;

    // Allocate memory for the sig_handler_t
    sig_handler_t *sig_handler = malloc(sizeof(sig_handler_t));
    if (sig_handler == NULL) {
        perror("malloc");
        exit(1);
    }

    // Initialize the sigset_t and the sig_handler struct
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sig_handler->set = set;

    // Create the signal handling thread and store its ID
    if ((err = pthread_create(&tid, 0, monitor_signal, sig_handler))) {
        free(sig_handler);  // Clean up in case of error
        handle_error_en(err, "pthread_create");
    }
    sig_handler->thread = tid;

    return sig_handler;  // Return the allocated and initialized sig_handler
}

/**
 * Destroys the SIGINT handling thread
 */
void sig_handler_destructor(sig_handler_t *sighandler) {
    int err;
    // Cancel and join with the signal handler's thread
    if ((err = pthread_cancel(sighandler->thread))) {
        handle_error_en(err, "pthread_cancel");
    }
    if ((err = pthread_join(sighandler->thread, 0))) {
        handle_error_en(err, "pthread_join");
    }
    // Free the allocated sighandler struct
    free(sighandler);
}

//------------------------------------------------------------------------------------------------
// Main function

// The arguments to the server should be the port number.
int main(int argc, char *argv[]) {
    // Parse args
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port> \n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    int err;

    // Block SIGPIPE signal so that the server does not abort when a client
    // disconnects
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    sigaddset(&set, SIGINT);

    if ((err = pthread_sigmask(SIG_BLOCK, &set, 0))) {
        handle_error_en(err, "pthread_sigmask");
    }

    // Create a SIGINT signal handler
    sig_handler_t *sig_handler = sig_handler_constructor();

    // Start a listener thread for clients
    pthread_t listener_thread = start_listener(port, client_constructor);

    // Loop for command line input ("p", "s", "g" commands)
    char buf[COMMAND_LEN];
    char *tokens[MAX_TOKENS] = {NULL};
    memset(&buf, 0, COMMAND_LEN);
    while (1) {
        ssize_t n = 0;
        if ((n = read(0, buf, COMMAND_LEN)) == 1)
            continue;  // Ignore single '\n'
        else if (n == 0) {
            break;  // Break when user hits "Ctrl-D" (EOF)
        } else if (n < 0) {
            perror("read");
            exit(1);
        }
        buf[n] = '\0';  // Null terminate the buffer

        // Parse input
        parse(buf, tokens);

        // Reprompt if no valid input is given
        if (tokens[0] == NULL) {
            continue;
        }
        if (strcmp("p", tokens[0]) == 0) {
            db_print(tokens[1]);
        } else if (strcmp("s", tokens[0]) == 0) {
            if (printf("stopping all clients\n") < 0) {
                fprintf(stderr, "unable to print stop message\n");
            }
            client_control_stop();
        } else if (strcmp("g", tokens[0]) == 0) {
            if (printf("releasing all clients\n") < 0) {
                fprintf(stderr, "unable to print go message\n");
            }
            client_control_release();
        }
    }
    // Destroy the SIGINT handling thread
    sig_handler_destructor(sig_handler);

    // Update the server to no longer accept clients
    pthread_mutex_lock(&server_accept.mutex);
    server_accept.state = 0;
    pthread_mutex_unlock(&server_accept.mutex);

    // Cancel all client threads
    delete_all();

    // Stop the listener thread until there are no active clients left
    pthread_mutex_lock(&server_control.server_mutex);
    while (server_control.num_client_threads != 0) {
        pthread_cond_wait(&server_control.server_cond,
                          &server_control.server_mutex);
    }
    pthread_mutex_unlock(&server_control.server_mutex);

    // Make sure the thread list is empty
    assert(thread_list_head == NULL);
    assert(server_control.num_client_threads == 0);

    // Clean up the database
    db_cleanup();

    // Cancel the listener thread
    if ((err = pthread_cancel(listener_thread))) {
        handle_error_en(err, "pthread_cancel");
    }

    // Join with the listener thread (wait for it to terminate)
    if ((err = pthread_join(listener_thread, 0))) {
        handle_error_en(err, "pthread_join");
    }
    // Exit
    pthread_exit(0);

    return 0;
}