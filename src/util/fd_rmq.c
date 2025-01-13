#include "fd_rmq.h"
#include "log/fd_log.h"

fd_rmq_t* 
fd_rmq_init(const char* host, int port, const char* user, const char* pass) {
  fd_rmq_t* rmq = (fd_rmq_t*)malloc(sizeof(fd_rmq_t));
  if (!rmq) {
    FD_LOG_ERR(("Failed to allocate RMQ context"));
    return NULL;
  }

  rmq->conn = amqp_new_connection();
  rmq->socket = amqp_tcp_socket_new(rmq->conn);
  rmq->channel = 1;
  rmq->is_connected = 0;

  if (!rmq->socket) {
    FD_LOG_ERR(("Failed to create RMQ socket"));
    free(rmq);
    return NULL;
  }

  if (amqp_socket_open(rmq->socket, host, port)) {
    FD_LOG_ERR(("Failed to open RMQ socket"));
    free(rmq);
    return NULL;
  }

  amqp_rpc_reply_t reply = amqp_login(rmq->conn, "/", 0, 131072, 0, 
                                     AMQP_SASL_METHOD_PLAIN, user, pass);
  if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
    FD_LOG_ERR(("RMQ login failed"));
    free(rmq);
    return NULL;
  }

  amqp_channel_open(rmq->conn, rmq->channel);
  reply = amqp_get_rpc_reply(rmq->conn);
  if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
    FD_LOG_ERR(("Failed to open RMQ channel"));
    free(rmq);
    return NULL;
  }

  rmq->is_connected = 1;
  return rmq;
}

int 
fd_rmq_publish(fd_rmq_t* rmq, const char* exchange, const char* routing_key, const char* message) {
  if (!rmq || !rmq->is_connected) return -1;

  return amqp_basic_publish(rmq->conn, rmq->channel,
                           amqp_cstring_bytes(exchange),
                           amqp_cstring_bytes(routing_key),
                           0, 0, NULL,
                           amqp_cstring_bytes(message));
}

void 
fd_rmq_cleanup(fd_rmq_t* rmq) {
  if (!rmq) return;
  
  if (rmq->is_connected) {
    amqp_channel_close(rmq->conn, rmq->channel, AMQP_REPLY_SUCCESS);
    amqp_connection_close(rmq->conn, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(rmq->conn);
  }
  
  free(rmq);
}