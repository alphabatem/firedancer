#ifndef FD_RMQ_H
#define FD_RMQ_H

#include <amqp.h>
#include <amqp_tcp_socket.h>
#include "fd_util.h"

typedef struct {
  amqp_connection_state_t conn;
  amqp_socket_t *socket;
  int channel;
  int is_connected;
} fd_rmq_t;

// Initialize RabbitMQ connection
fd_rmq_t* fd_rmq_init(const char* host, int port, const char* user, const char* pass);

// Publish message
int fd_rmq_publish(fd_rmq_t* rmq, const char* exchange, const char* routing_key, const char* message);

// Cleanup
void fd_rmq_cleanup(fd_rmq_t* rmq);

#endif