#include "io.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>

#include "common.h"
#include "event_loop.h"

#ifndef _WIN32
/* This function is actually not declared in standard POSIX, so declare it. */
extern int usleep(useconds_t usec);
#endif

int bind_inet_sock(const int port, bool shall_listen) {
  struct sockaddr_in name;
  int socket_fd = socket(PF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    LOG_ERROR("socket() failed for port %d.", port);
    return -1;
  }
  name.sin_family = AF_INET;
  name.sin_port = htons(port);
  name.sin_addr.s_addr = htonl(INADDR_ANY);
  int on = 1;
  /* TODO(pcm): http://stackoverflow.com/q/1150635 */
  if (ioctl(socket_fd, FIONBIO, (char *) &on) < 0) {
    LOG_ERROR("ioctl failed");
    close(socket_fd);
    return -1;
  }
  int *const pon = (int *const) & on;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, pon, sizeof(on)) < 0) {
    LOG_ERROR("setsockopt failed for port %d", port);
    close(socket_fd);
    return -1;
  }
  if (bind(socket_fd, (struct sockaddr *) &name, sizeof(name)) < 0) {
    LOG_ERROR("Bind failed for port %d", port);
    close(socket_fd);
    return -1;
  }
  if (shall_listen && listen(socket_fd, 128) == -1) {
    LOG_ERROR("Could not listen to socket %d", port);
    close(socket_fd);
    return -1;
  }
  return socket_fd;
}

int bind_ipc_sock(const char *socket_pathname, bool shall_listen) {
  struct sockaddr_un socket_address;
  int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    LOG_ERROR("socket() failed for pathname %s.", socket_pathname);
    return -1;
  }
  /* Tell the system to allow the port to be reused. */
  int on = 1;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (char *) &on,
                 sizeof(on)) < 0) {
    LOG_ERROR("setsockopt failed for pathname %s", socket_pathname);
    close(socket_fd);
    return -1;
  }

  unlink(socket_pathname);
  memset(&socket_address, 0, sizeof(socket_address));
  socket_address.sun_family = AF_UNIX;
  if (strlen(socket_pathname) + 1 > sizeof(socket_address.sun_path)) {
    LOG_ERROR("Socket pathname is too long.");
    close(socket_fd);
    return -1;
  }
  strncpy(socket_address.sun_path, socket_pathname,
          strlen(socket_pathname) + 1);

  if (bind(socket_fd, (struct sockaddr *) &socket_address,
           sizeof(socket_address)) != 0) {
    LOG_ERROR("Bind failed for pathname %s.", socket_pathname);
    close(socket_fd);
    return -1;
  }
  if (shall_listen && listen(socket_fd, 128) == -1) {
    LOG_ERROR("Could not listen to socket %s", socket_pathname);
    close(socket_fd);
    return -1;
  }
  return socket_fd;
}

int connect_ipc_sock_retry(const char *socket_pathname,
                           int num_retries,
                           int64_t timeout) {
  /* Pick the default values if the user did not specify. */
  if (num_retries < 0) {
    num_retries = NUM_CONNECT_ATTEMPTS;
  }
  if (timeout < 0) {
    timeout = CONNECT_TIMEOUT_MS;
  }

  CHECK(socket_pathname);
  int fd = -1;
  for (int num_attempts = 0; num_attempts < num_retries; ++num_attempts) {
    fd = connect_ipc_sock(socket_pathname);
    if (fd >= 0) {
      break;
    }
    if (num_attempts == 0) {
      LOG_ERROR("Connection to socket failed for pathname %s.",
                socket_pathname);
    }
    /* Sleep for timeout milliseconds. */
    usleep(timeout * 1000);
  }
  /* If we could not connect to the socket, exit. */
  if (fd == -1) {
    LOG_FATAL("Could not connect to socket %s", socket_pathname);
  }
  return fd;
}

int connect_ipc_sock(const char *socket_pathname) {
  struct sockaddr_un socket_address;
  int socket_fd;

  socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    LOG_ERROR("socket() failed for pathname %s.", socket_pathname);
    return -1;
  }

  memset(&socket_address, 0, sizeof(socket_address));
  socket_address.sun_family = AF_UNIX;
  if (strlen(socket_pathname) + 1 > sizeof(socket_address.sun_path)) {
    LOG_ERROR("Socket pathname is too long.");
    return -1;
  }
  strncpy(socket_address.sun_path, socket_pathname,
          strlen(socket_pathname) + 1);

  if (connect(socket_fd, (struct sockaddr *) &socket_address,
              sizeof(socket_address)) != 0) {
    close(socket_fd);
    return -1;
  }

  return socket_fd;
}

int connect_inet_sock_retry(const char *ip_addr,
                            int port,
                            int num_retries,
                            int64_t timeout) {
  /* Pick the default values if the user did not specify. */
  if (num_retries < 0) {
    num_retries = NUM_CONNECT_ATTEMPTS;
  }
  if (timeout < 0) {
    timeout = CONNECT_TIMEOUT_MS;
  }

  CHECK(ip_addr);
  int fd = -1;
  for (int num_attempts = 0; num_attempts < num_retries; ++num_attempts) {
    fd = connect_inet_sock(ip_addr, port);
    if (fd >= 0) {
      break;
    }
    if (num_attempts == 0) {
      LOG_ERROR("Connection to socket failed for address %s:%d.", ip_addr,
                port);
    }
    /* Sleep for timeout milliseconds. */
    usleep(timeout * 1000);
  }
  /* If we could not connect to the socket, exit. */
  if (fd == -1) {
    LOG_FATAL("Could not connect to address %s:%d", ip_addr, port);
  }
  return fd;
}

int connect_inet_sock(const char *ip_addr, int port) {
  int fd = socket(PF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG_ERROR("socket() failed for address %s:%d.", ip_addr, port);
    return -1;
  }

  struct hostent *manager = gethostbyname(ip_addr); /* TODO(pcm): cache this */
  if (!manager) {
    LOG_ERROR("Failed to get hostname from address %s:%d.", ip_addr, port);
    close(fd);
    return -1;
  }

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  memcpy(&addr.sin_addr.s_addr, manager->h_addr_list[0], manager->h_length);
  addr.sin_port = htons(port);

  if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int accept_client(int socket_fd) {
  int client_fd = accept(socket_fd, NULL, NULL);
  if (client_fd < 0) {
    LOG_ERROR("Error reading from socket.");
    return -1;
  }
  return client_fd;
}

int write_bytes(int fd, uint8_t *cursor, size_t length) {
  ssize_t nbytes = 0;
  size_t bytesleft = length;
  size_t offset = 0;
  while (bytesleft > 0) {
    /* While we haven't written the whole message, write to the file
     * descriptor, advance the cursor, and decrease the amount left to write. */
    nbytes = write(fd, cursor + offset, bytesleft);
    if (nbytes < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        continue;
      }
      return -1; /* Errno will be set. */
    } else if (0 == nbytes) {
      /* Encountered early EOF. */
      return -1;
    }
    CHECK(nbytes > 0);
    bytesleft -= nbytes;
    offset += nbytes;
  }

  return 0;
}

int write_message(int fd, int64_t type, int64_t length, uint8_t *bytes) {
  int64_t version = RAY_PROTOCOL_VERSION;
  int closed;
  closed = write_bytes(fd, (uint8_t *) &version, sizeof(version));
  if (closed) {
    return closed;
  }
  closed = write_bytes(fd, (uint8_t *) &type, sizeof(type));
  if (closed) {
    return closed;
  }
  closed = write_bytes(fd, (uint8_t *) &length, sizeof(length));
  if (closed) {
    return closed;
  }
  closed = write_bytes(fd, bytes, length * sizeof(char));
  if (closed) {
    return closed;
  }
  return 0;
}

int read_bytes(int fd, uint8_t *cursor, size_t length) {
  ssize_t nbytes = 0;
  /* Termination condition: EOF or read 'length' bytes total. */
  size_t bytesleft = length;
  size_t offset = 0;
  while (bytesleft > 0) {
    nbytes = read(fd, cursor + offset, bytesleft);
    if (nbytes < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        continue;
      }
      return -1; /* Errno will be set. */
    } else if (0 == nbytes) {
      /* Encountered early EOF. */
      return -1;
    }
    CHECK(nbytes > 0);
    bytesleft -= nbytes;
    offset += nbytes;
  }

  return 0;
}

void read_message(int fd, int64_t *type, int64_t *length, uint8_t **bytes) {
  int64_t version;
  int closed = read_bytes(fd, (uint8_t *) &version, sizeof(version));
  if (closed) {
    goto disconnected;
  }
  CHECK(version == RAY_PROTOCOL_VERSION);
  closed = read_bytes(fd, (uint8_t *) type, sizeof(*type));
  if (closed) {
    goto disconnected;
  }
  closed = read_bytes(fd, (uint8_t *) length, sizeof(*length));
  if (closed) {
    goto disconnected;
  }
  *bytes = (uint8_t *) malloc(*length * sizeof(uint8_t));
  closed = read_bytes(fd, *bytes, *length);
  if (closed) {
    free(*bytes);
    goto disconnected;
  }
  return;

disconnected:
  /* Handle the case in which the socket is closed. */
  *type = DISCONNECT_CLIENT;
  *length = 0;
  *bytes = NULL;
  return;
}

uint8_t *read_message_async(event_loop *loop, int sock) {
  int64_t size;
  int error = read_bytes(sock, (uint8_t *) &size, sizeof(int64_t));
  if (error < 0) {
    /* The other side has closed the socket. */
    LOG_DEBUG("Socket has been closed, or some other error has occurred.");
    if (loop != NULL) {
      event_loop_remove_file(loop, sock);
    }
    close(sock);
    return NULL;
  }
  uint8_t *message = (uint8_t *) malloc(size);
  error = read_bytes(sock, message, size);
  if (error < 0) {
    /* The other side has closed the socket. */
    LOG_DEBUG("Socket has been closed, or some other error has occurred.");
    if (loop != NULL) {
      event_loop_remove_file(loop, sock);
    }
    close(sock);
    return NULL;
  }
  return message;
}

int64_t read_vector(int fd, int64_t *type, std::vector<uint8_t> &buffer) {
  int64_t version;
  int closed = read_bytes(fd, (uint8_t *) &version, sizeof(version));
  if (closed) {
    goto disconnected;
  }
  CHECK(version == RAY_PROTOCOL_VERSION);
  int64_t length;
  closed = read_bytes(fd, (uint8_t *) type, sizeof(*type));
  if (closed) {
    goto disconnected;
  }
  closed = read_bytes(fd, (uint8_t *) &length, sizeof(length));
  if (closed) {
    goto disconnected;
  }
  if (length > buffer.size()) {
    buffer.resize(length);
  }
  closed = read_bytes(fd, buffer.data(), length);
  if (closed) {
    goto disconnected;
  }
  return length;
disconnected:
  /* Handle the case in which the socket is closed. */
  *type = DISCONNECT_CLIENT;
  return 0;
}

void write_log_message(int fd, const char *message) {
  /* Account for the \0 at the end of the string. */
  write_message(fd, LOG_MESSAGE, strlen(message) + 1, (uint8_t *) message);
}

char *read_log_message(int fd) {
  uint8_t *bytes;
  int64_t type;
  int64_t length;
  read_message(fd, &type, &length, &bytes);
  CHECK(type == LOG_MESSAGE);
  return (char *) bytes;
}
