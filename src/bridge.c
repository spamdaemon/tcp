#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>

/** Includes needed for the sockets */
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/select.h>

static int verbose = 0;
static const int INFO = 1;
static const int DEBUG = 2;
static const int TRACE = 3;

static const int SOCKET_TYPE = SOCK_STREAM;
static const int SOCKET_PROTOCOL = IPPROTO_TCP;

typedef struct
{
   char* addressport;
   struct sockaddr* address;
   int domain;
   int type;
   int protocol;
   int addressLen;
} SocketAddress;

static SocketAddress* newSocketAddress()
{
   SocketAddress *addr = (SocketAddress*) malloc(sizeof(SocketAddress));
   addr->addressport = NULL;
   addr->address = NULL;
   addr->domain = 0;
   addr->addressLen = 0;
   return addr;
}

static void freeSocketAddress(SocketAddress* addr)
{
   if (addr != NULL) {
      free(addr->addressport);
      free(addr->address);
      free(addr);
   }
}

/**
 * Parse an address and port of the form : <ipv4address>[:[<port>]]
 *
 * @param addressport
 * @param addr structure to be filled out
 * @return 0 on error, 1 on success
 */
static SocketAddress* parse_address(const char* addressport)
{
   SocketAddress* result = NULL;
   char* rawAddress = NULL;
   char* address = NULL;
   char* service = NULL;
   const char* separators = "/:";
   struct addrinfo* addresses = NULL;
   struct sockaddr* ip_addr = NULL;

   rawAddress = strdup(addressport);
   address = rawAddress;

   // split out address and service
   if (rawAddress[0] == '[') {
      // ipv6 address
      address = rawAddress + 1;
      char* x = rindex(rawAddress, ']');
      if (x != NULL) {
         *x = '\0';
         x = rindex(x + 1, ':');
         if (x != NULL) {
            *x = '\0';
            service = x + 1;
         }
      }
   }
   else {
      for (const char* sep = separators; *sep != '\0'; ++sep) {
         char* x = rindex(address, *sep);
         if (x != NULL) {
            *x = '\0';
            service = x + 1;
            break;
         }
      }
   }

   if (service == NULL || address == NULL) {
      fprintf(stderr, "Missing service or address\n");
      goto CLEANUP;
   }

   struct addrinfo hints = {
   AI_ALL,
   AF_UNSPEC, SOCKET_TYPE, SOCKET_PROTOCOL };

   int error = getaddrinfo(address, service, &hints, &addresses);

   if (error) {
      if (addressport == NULL) {
         fprintf(stderr, "Something unexpected happened : %s\n", gai_strerror(error));
      }
      else {
         fprintf(stderr, "Failed to parse address %s : %s\n", addressport, gai_strerror(error));
      }
      goto CLEANUP;
   }

   for (struct addrinfo* a = addresses; a != NULL; a = a->ai_next) {
      if (a->ai_family == AF_INET || a->ai_family == AF_INET6) {
         struct sockaddr* ip_addr = (struct sockaddr*) malloc(a->ai_addrlen);
         if (ip_addr != NULL) {
            result = newSocketAddress();
            result->addressport = strdup(addressport);
            result->address = ip_addr;
            result->protocol = a->ai_protocol;
            result->type = a->ai_socktype;
            result->domain = a->ai_family;
            result->addressLen = a->ai_addrlen;
            memcpy(result->address, a->ai_addr, a->ai_addrlen);
            break;
         }
      }
   }

   CLEANUP: if (result != NULL) {
      if (verbose >= INFO) {
         fprintf(stderr, "Found an address for %s/%s\n", address, service);
      }
   }
   else {
      fprintf(stderr, "No address found for %s/%s\n", address, service);
   }
   free(rawAddress);
   freeaddrinfo(addresses);
   return result;
}

static int createSocket(SocketAddress* addr)
{
   return socket(addr->domain, addr->type, addr->protocol);
}

static int bindSocket(int s, SocketAddress* addr)
{
   return bind(s, addr->address, addr->addressLen);
}

static int connectSocket(int s, SocketAddress* addr)
{
   return connect(s, addr->address, addr->addressLen);
}

static int bindAny(int s, int domain)
{
   int result = -1;
   if (domain == AF_INET) {
      struct sockaddr_in addr;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = 0;
      addr.sin_family = domain;
      result = bind(s, (struct sockaddr*) &addr, sizeof(addr));
   }
   else if (domain == AF_INET6) {
      struct sockaddr_in6 addr;
      addr.sin6_addr = in6addr_any;
      addr.sin6_port = 0;
      addr.sin6_flowinfo = 0;
      addr.sin6_scope_id = 0;
      addr.sin6_family = domain;
      result = bind(s, (struct sockaddr*) &addr, sizeof(addr));
   }
   return result;
}

static int set_SO_REUSEADDR(int s)
{
   int value = 1;
   return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
}

static int set_NonBlocking(int s)
{
   int flags = fcntl(s, F_GETFL);
   if (flags < 0) {
      return -1;
   }
   int status = fcntl(s, F_SETFL, flags | O_NONBLOCK);
   return status;
}

struct XferBufferStruct;
typedef struct XferBufferStruct XferBuffer;

typedef void (*XferBufferTransform)(XferBuffer* buf);

static int maxFD(int a, int b)
{
   return a < b ? b : a;
}

typedef struct
{
   int headersize;
   int haveMessage;
   int (*tx)(int);
} TransformState;

struct XferBufferStruct
{
   int readfd;
   int writefd;
   int allowWrite;
   int allowRead;
   size_t writeMark;
   size_t readMark;
   size_t endMark;
   size_t buffersize;
   char* buffer;
   int is_pipe;
   int pipe_fd[2];
   XferBufferTransform transform;
   void* transformData;
};

static XferBuffer* newTransferBuffer(int reader, int writer)
{
   XferBuffer* buf = (XferBuffer*) malloc(sizeof(XferBuffer));
   buf->readfd = reader;
   buf->writefd = writer;
   buf->buffersize = 16 * 1024;
   buf->buffer = malloc(buf->buffersize);
   buf->allowWrite = 1;
   buf->allowRead = 1;
   buf->readMark = 0;
   buf->writeMark = 0;
   buf->endMark = buf->buffersize;
   buf->is_pipe = 0;
   buf->pipe_fd[0] = buf->pipe_fd[1] = -1;
   buf->transform = NULL;
   buf->transformData = NULL;
   return buf;
}

static void freeXferBuffer(XferBuffer* buf)
{
   if (buf != NULL) {
      if (buf->pipe_fd[0] >= 0) {
         close(buf->pipe_fd[0]);
      }
      if (buf->pipe_fd[1] >= 0) {
         close(buf->pipe_fd[1]);
      }
      free(buf->buffer);
      free(buf);
   }
}

static int prepareXferBufferSelection(XferBuffer* buf, fd_set* readfds, fd_set* writefds)
{
   if (buf->transform == NULL) {
      buf->is_pipe = 1;
   }
   if (buf->is_pipe) {

      if (buf->pipe_fd[0] == buf->pipe_fd[1]) {
         // create a pipe on the fly
         if (pipe(buf->pipe_fd) < 0) {
            perror("Failed to a create pipe");
            return 0;
         }
         set_NonBlocking(buf->pipe_fd[0]);
         set_NonBlocking(buf->pipe_fd[1]);
      }

      FD_SET(buf->readfd, readfds);
      FD_SET(buf->pipe_fd[0], readfds);
      FD_SET(buf->writefd, writefds);
      FD_SET(buf->pipe_fd[1], writefds);
   }
   else {
      FD_CLR(buf->readfd, readfds);
      if (buf->endMark > buf->readMark && buf->allowRead) {
         FD_SET(buf->readfd, readfds);
      }
      FD_CLR(buf->writefd, writefds);
      if (buf->endMark > buf->writeMark && buf->allowWrite) {
         FD_SET(buf->writefd, writefds);
      }
   }
   return -1;
}

// fill the buffer by reading from the associated file descriptor
// return the amount of data that can still be read to fill the buffer
static ssize_t fillXferBuffer(XferBuffer* buf, fd_set* readfds)
{
   if (readfds == NULL || FD_ISSET(buf->readfd, readfds)) {
      if (buf->readMark < buf->endMark && buf->allowRead) {
         if (readfds != NULL) {
            FD_CLR(buf->readfd, readfds);
         }
         ssize_t n = 0;
         if (buf->is_pipe) {
            n = splice(buf->readfd, NULL, buf->pipe_fd[1], NULL, 16 * 1024, SPLICE_F_NONBLOCK);
            if (n > 0) {
               // the buffer is full
               buf->readMark = 0;
               buf->endMark = n;
            }
         }
         else {
            n = read(buf->readfd, buf->buffer + buf->readMark, buf->endMark - buf->readMark);
         }
         if (n > 0) {
            if (verbose >= TRACE) {
               if (buf->is_pipe) {
                  fprintf(stderr, "Spliced %d bytes from %d\n", n, buf->readfd);
               }
               else {
                  fprintf(stderr, "Read %d bytes from %d\n", n, buf->readfd);
               }
            }
            buf->readMark += n;
         }
         else if (n == 0) {
            return -1;
         }
         else {
            perror("Failed to fill transfer buffer");
            return -1;
         }
      }
   }
   return buf->endMark - buf->readMark;
}

// empty the buffer by writing to the associated file descriptor
// return the amount of data that can still be written before the readMark is reached.
static ssize_t emptyXferBuffer(XferBuffer* buf, fd_set* writefds)
{
   if (writefds == NULL || FD_ISSET(buf->writefd, writefds)) {
      if (buf->writeMark < buf->readMark && buf->allowWrite) {
         if (writefds != NULL) {
            FD_CLR(buf->writefd, writefds);
         }
         ssize_t n = 0;
         if (buf->is_pipe) {
            n = splice(buf->pipe_fd[0], NULL, buf->writefd, NULL, buf->readMark - buf->writeMark, SPLICE_F_NONBLOCK);

         }
         else {
            n = write(buf->writefd, buf->buffer + buf->writeMark, buf->readMark - buf->writeMark);
         }
         if (n > 0) {
            buf->writeMark += n;
            if (verbose >= TRACE) {
               if (buf->is_pipe) {
                  fprintf(stderr, "Spliced %d bytes to %d\n", n, buf->writefd);
               }
               else {
                  fprintf(stderr, "Wrote %d bytes to %d\n", n, buf->writefd);
               }
            }
            if (buf->writeMark == buf->readMark) {
               // reset the buffer
               buf->readMark = buf->writeMark = 0;
               buf->endMark = buf->buffersize;
               buf->allowRead = 1;
            }
         }
         else if (n == 0) {
            return -1;
         }
         else {
            perror("Failed to empty transfer buffer");
            return -1;
         }
      }
   }
   return buf->readMark - buf->writeMark;
}

static void usage(int argc, char* const * argv)
{
   fprintf(stderr, "Usage: %s [-h] [-v] [<server>:<serverport>] [<destination>:<destinationport>]\n", argv[0]);
   fprintf(stderr, " Create a bridge between a local port and a destination port\n");
   fprintf(stderr, " Ports can be separated by a / or : from the address. If the address is a IPv6 address,\n");
   fprintf(stderr, " then it must be enclosed in [] if the : separator is used\n");
   fprintf(stderr, "Options:\n");
   fprintf(stderr, "   -h     this message\n");
   fprintf(stderr, "   -v     increase verbosity (use multiple times for more details)\n");
   fprintf(stderr, "Examples:\n");
   fprintf(stderr, " %s 0.0.0.0:12345 localhost:23456\n", argv[0]);
   exit(1);
}

void transformXferBuffer(XferBuffer* buf)
{
   TransformState* state = (TransformState*) buf->transformData;

   // reset the have message flag, if necessary
   if (buf->readMark == buf->writeMark) {
      buf->readMark = buf->writeMark = 0;
      buf->endMark = state->headersize;
      state->haveMessage = 0;
   }

   if (!state->haveMessage && buf->readMark == buf->endMark) {
      // we've got a message header, so update the endmark to read an entire message
      if (buf->endMark == state->headersize) {
         // parse the header
         // and update the endmark, if necessary
      }

      if (buf->readMark == buf->endMark) {
         // we've got a full message, which we can now transform
         state->haveMessage = 1;

         // our transform function, but you can do whatever you want here
         buf->buffer[0] = state->tx(buf->buffer[0]);
      }
   }

   buf->allowWrite = state->haveMessage;
   buf->allowRead = !buf->allowWrite;
}

int main(int argc, char* const * argv)
{
   char* serverAddressPort;
   char* destAddressPort;

   optind = 1;
   for (int opt; (opt = getopt(argc, argv, "hv")) != -1;) {
      switch (opt) {
         case 'v':
            verbose++;
            break;
         default:
            usage(argc, argv);
            break;
      }
   }
   int argPos = optind;

   if (argPos == argc) {
      fprintf(stderr, "Missing server address\n");
      usage(argc, argv);
   }
   serverAddressPort = argv[argPos++];
   if (argPos == argc) {
      fprintf(stderr, "Missing destination address\n");
      usage(argc, argv);
   }
   destAddressPort = argv[argPos++];
   if (argPos != argc) {
      fprintf(stderr, "Found unexpected arguments\n");
      usage(argc, argv);
   }
   int server = -1; // server socket
   int dest = -1; // the destination for the bridge
   int client = -1;
   const char* errorMessage = NULL;
   XferBuffer* inbound = NULL;
   XferBuffer* outbound = NULL;

   SocketAddress* serverAddr = parse_address(serverAddressPort);
   SocketAddress* destAddr = parse_address(destAddressPort);

   if (serverAddr == NULL) {
      errorMessage = "Failed to parse the server address and/or port";
      goto CLEANUP;
   }
   if (destAddr == NULL) {
      errorMessage = "Failed to parse the dest address and/or port";
      goto CLEANUP;
   }

   // open the server and destination sockets
   server = createSocket(serverAddr);
   if (server < 0) {
      errorMessage = "Failed to create a server socket";
      goto CLEANUP;
   }
   dest = createSocket(destAddr);
   if (dest < 0) {
      errorMessage = "Failed to create a destination socket";
      goto CLEANUP;
   }

   if (set_SO_REUSEADDR(server) != 0) {
      errorMessage = "Failed to enable reuse-addr on server socket";
      goto CLEANUP;
   }

   if (bindSocket(server, serverAddr) != 0) {
      errorMessage = "Failed to create to bind server socket";
      goto CLEANUP;
   }

   // connect to the destination first, and then open the server socket
   if (verbose >= INFO) {
      fprintf(stderr, "Connecting to destination %s\n", destAddr->addressport);
   }
   while (connectSocket(dest, destAddr) != 0) {
      sleep(1);
   }
   if (verbose >= INFO) {
      fprintf(stderr, "Connected to destination\n");
   }

   // now, we start listening of connection attempts to the server socket
   listen(server, 1);

   client = accept(server, NULL, 0);
   if (client < 0) {
      perror("Failed to send message");
      errorMessage = "Accept connection";
      goto CLEANUP;
   }
   if (verbose >= INFO) {
      fprintf(stderr, "Accepted a connection\n");
   }
   fd_set readfds[1], writefds[1], errorfds[1];
   FD_ZERO(readfds);
   FD_ZERO(writefds);
   FD_ZERO(errorfds);
   int nfds = maxFD(client, dest);

   inbound = newTransferBuffer(dest, client);
   outbound = newTransferBuffer(client, dest);

   /**
    * Set your own transforms here
    */
   TransformState istate;
   istate.headersize = 1;
   istate.tx = toupper;
   inbound->transform = transformXferBuffer;
   inbound->transformData = &istate;

   if (inbound->transform != NULL) {
      inbound->transform(inbound);
   }
   if (outbound->transform != NULL) {
      outbound->transform(outbound);
   }

   if (set_NonBlocking(client) < 0) {
      errorMessage = "Failed to make destination socket non-blocking";
   }

   if (set_NonBlocking(dest) < 0) {
      errorMessage = "Failed to make destination socket non-blocking";
   }

   for (;;) {
      prepareXferBufferSelection(inbound, readfds, writefds);
      prepareXferBufferSelection(outbound, readfds, writefds);
      if (inbound->is_pipe) {
         nfds = maxFD(nfds, inbound->pipe_fd[0]);
         nfds = maxFD(nfds, inbound->pipe_fd[1]);
      }
      if (outbound->is_pipe) {
         nfds = maxFD(nfds, outbound->pipe_fd[0]);
         nfds = maxFD(nfds, outbound->pipe_fd[1]);
      }
      FD_SET(client, errorfds);
      FD_SET(dest, errorfds);
      int status = select(nfds + 1, readfds, writefds, errorfds, NULL);
      if (status < 0) {
         perror("Failed to send message");
         errorMessage = "Select failed";
         break;
      }
      if (FD_ISSET(client, errorfds)) {
         errorMessage = "client encountered some kind of error";
         break;
      }
      if (FD_ISSET(dest, errorfds)) {
         errorMessage = "destination encountered some kind of error";
         break;
      }
      if (status != 0) {
         // process the inbound buffer
         {
            // first, empty the previously read data
            if (emptyXferBuffer(inbound, writefds) < 0) {
               break;
            }
            if (fillXferBuffer(inbound, readfds) < 0) {
               break;
            }
            if (inbound->transform != NULL) {
               inbound->transform(inbound);
            }
            if (emptyXferBuffer(inbound, writefds) < 0) {
               break;
            }
         }

         // process the outbound buffer
         {
            if (emptyXferBuffer(outbound, writefds) < 0) {
               break;
            }
            if (fillXferBuffer(outbound, readfds) < 0) {
               break;
            }
            if (outbound->transform != NULL) {
               outbound->transform(outbound);
            }
            // if we've not written to the socket and the ready for write is still set,
            // then write immediately
            if (emptyXferBuffer(outbound, writefds) < 0) {
               break;
            }
         }
      }
   }

   CLEANUP: /** cleanup handlers and termination */

   freeXferBuffer(outbound);
   freeXferBuffer(inbound);
   freeSocketAddress(serverAddr);
   freeSocketAddress(destAddr);
   if (dest >= 0) {
      close(dest);
   }
   if (server >= 0) {
      close(server);
   }
   return 0;
}
