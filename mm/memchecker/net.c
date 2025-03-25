#include <nuttx/config.h>
#include <nuttx/mm/memchecker.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include "net.h"


void memchecker_net_initialize(void)
{
  struct sockaddr_in sockaddr;
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0)
    {
      syslog(LOG_ERR, "memchecker_net_initialize: socket failed");
      close(sockfd);
      return;
    }
  
  memset(&sockaddr, 0x00, sizeof(sockaddr));
  sockaddr.sin_family = AF_INET;
  sockaddr.sin_port = htons(9999);
  sockaddr.sin_addr.s_addr = inet_addr("192.168.101.14");

  if (connect(sockfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) < 0)
    {
      syslog(LOG_INFO, "Failed to connect to server socket");
      close(sockfd);
      return;
    }
  
  send(sockfd, "Hello, world!", 13, 0);
  close(sockfd);
}
