#if defined(WIN32) || defined(UNDER_CE)
#include <ws2tcpip.h>
// for ifaddrs.h
#include <iphlpapi.h>
#define MSG_CONFIRM 0

#elif defined(LINUX) || defined(linux) || defined(__APPLE__)
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include "MACAddressUtility.h"
#include "common.h"
#include "errmsg.h"
#include "udpsocket.h"
#include <errno.h>

#include <stdio.h>
#include <string.h>
#include <string>
#include <strings.h>

#if defined(__linux__)
#include <linux/wireless.h>
#endif

#ifdef __APPLE__
#include "MACAddressUtility.h"
#define MSG_CONFIRM 0
#endif

#define LISTEN_BACKLOG 512

const size_t
    pingbufsize(HEADERLEN +
                sizeof(std::chrono::high_resolution_clock::time_point) +
                sizeof(endpoint_t) + 1);

udpsocket_t::udpsocket_t() : tx_bytes(0), rx_bytes(0)
{
  // linux part, sets value pointed to by &serv_addr to 0 value:
  // bzero((char*)&serv_addr, sizeof(serv_addr));
  // windows:
  memset((char*)&serv_addr, 0, sizeof(serv_addr));
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if(sockfd < 0)
    throw ErrMsg("Opening socket failed: ", errno);
#ifndef __APPLE__
  int priority = 5;
  // IPTOS_CLASS_CS6 defined in netinet/ip.h 0xc0
  // int iptos = IPTOS_CLASS_CS6;
  int iptos = 0xc0;

  // gnu SO_PRIORITY
  // IP_TOS defined in ws2tcpip.h
#if defined(WIN32) || defined(UNDER_CE)
  // setsockopt defined in winsock2.h 3rd parameter const char*
  // windows (cast):
  setsockopt(sockfd, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&iptos),
             sizeof(iptos));
  // no documentation on what SO_PRIORITY does, optname, level in GNU socket
  setsockopt(sockfd, SOL_SOCKET, SO_GROUP_PRIORITY,
             reinterpret_cast<const char*>(&priority), sizeof(priority));
#else
  // on linux:
  setsockopt(sockfd, IPPROTO_IP, IP_TOS, &iptos, sizeof(iptos));
  setsockopt(sockfd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority));
#endif
#endif
  isopen = true;
}

udpsocket_t::~udpsocket_t()
{
  close();
}

void udpsocket_t::set_timeout_usec(int usec)
{
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = usec;
#if defined(WIN32) || defined(UNDER_CE)
  // windows (cast):
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void udpsocket_t::close()
{
  if(isopen)
#if defined(WIN32) || defined(UNDER_CE)
    ::closesocket(sockfd);
#else
    ::close(sockfd);
#endif
  isopen = false;
}

void udpsocket_t::destination(const char* host)
{
  struct hostent* server;
  server = gethostbyname(host);
  if(server == NULL)
#if defined(WIN32) || defined(UNDER_CE)
    // windows:
    throw ErrMsg("No such host: " + std::to_string(WSAGetLastError()));
#else
    throw ErrMsg("No such host: " + std::string(hstrerror(h_errno)));
#endif
#if defined(WIN32) || defined(UNDER_CE)
  // windows:
  memset((char*)&serv_addr, 0, sizeof(serv_addr));
#else
  bzero((char*)&serv_addr, sizeof(serv_addr));
#endif
  serv_addr.sin_family = AF_INET;
  // bcopy((char*)server->h_addr, (char*)&serv_addr.sin_addr.s_addr,
  //      server->h_length);
  // windows:
  memcpy((char*)&serv_addr.sin_addr.s_addr, (char*)server->h_addr,
         server->h_length);
}

port_t udpsocket_t::bind(port_t port, bool loopback)
{
  int optval = 1;
  // setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval,
  //           sizeof(int));
  // windows (cast):
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval,
             sizeof(int));

  endpoint_t my_addr;
  memset(&my_addr, 0, sizeof(endpoint_t));
  /* Clear structure */
  my_addr.sin_family = AF_INET;
  my_addr.sin_port = htons((unsigned short)port);
  if(loopback) {
    my_addr.sin_addr.s_addr = 0x0100007f;
  }
  if(::bind(sockfd, (struct sockaddr*)&my_addr, sizeof(endpoint_t)) == -1)
    throw ErrMsg("Binding the socket to port " + std::to_string(port) +
                     " failed: ",
                 errno);
  socklen_t addrlen(sizeof(endpoint_t));
  getsockname(sockfd, (struct sockaddr*)&my_addr, &addrlen);
  return ntohs(my_addr.sin_port);
}

endpoint_t udpsocket_t::getsockep()
{
  endpoint_t my_addr;
  memset(&my_addr, 0, sizeof(endpoint_t));
  socklen_t addrlen(sizeof(endpoint_t));
  getsockname(sockfd, (struct sockaddr*)&my_addr, &addrlen);
  return my_addr;
}

ssize_t udpsocket_t::send(const char* buf, size_t len, int portno)
{
  if(portno == 0)
    return len;
  serv_addr.sin_port = htons(portno);
  ssize_t tx(sendto(sockfd, buf, len, MSG_CONFIRM, (struct sockaddr*)&serv_addr,
                    sizeof(serv_addr)));
  if(tx > 0)
    tx_bytes += tx;
  return tx;
}

ssize_t udpsocket_t::send(const char* buf, size_t len, const endpoint_t& ep)
{
  ssize_t tx(
      sendto(sockfd, buf, len, MSG_CONFIRM, (struct sockaddr*)&ep, sizeof(ep)));
  if(tx > 0)
    tx_bytes += tx;
  return tx;
}

ssize_t udpsocket_t::recvfrom(char* buf, size_t len, endpoint_t& addr)
{
  memset(&addr, 0, sizeof(endpoint_t));
  addr.sin_family = AF_INET;
  socklen_t socklen(sizeof(endpoint_t));
  ssize_t rx(
      ::recvfrom(sockfd, buf, len, 0, (struct sockaddr*)&addr, &socklen));
  if(rx > 0)
    rx_bytes += rx;
  return rx;
}

std::string addr2str(const struct in_addr& addr)
{
  return std::to_string(addr.s_addr & 0xff) + "." +
         std::to_string((addr.s_addr >> 8) & 0xff) + "." +
         std::to_string((addr.s_addr >> 16) & 0xff) + "." +
         std::to_string((addr.s_addr >> 24) & 0xff);
}

std::string ep2str(const endpoint_t& ep)
{
  return addr2str(ep.sin_addr) + "/" + std::to_string(ntohs(ep.sin_port));
}

std::string ep2ipstr(const endpoint_t& ep)
{
  return addr2str(ep.sin_addr);
}

ovbox_udpsocket_t::ovbox_udpsocket_t(secret_t secret) : secret(secret) {}

void ovbox_udpsocket_t::send_ping(stage_device_id_t cid, const endpoint_t& ep)
{
  if(cid >= MAXEP)
    return;
  char buffer[pingbufsize];
  std::chrono::high_resolution_clock::time_point t1(
      std::chrono::high_resolution_clock::now());
  size_t n = packmsg(buffer, pingbufsize, secret, cid, PORT_PING, 0,
                     (const char*)(&t1), sizeof(t1));
  n = addmsg(buffer, pingbufsize, n, (char*)(&ep), sizeof(ep));
  send(buffer, n, ep);
}

void ovbox_udpsocket_t::send_registration(stage_device_id_t cid, epmode_t mode,
                                          port_t port,
                                          const endpoint_t& localep)
{
  std::string rver(OVBOXVERSION);
  {
    size_t buflen(HEADERLEN + rver.size() + 1);
    char buffer[buflen];
    size_t n(packmsg(buffer, buflen, secret, cid, PORT_REGISTER, mode,
                     rver.c_str(), rver.size() + 1));
    send(buffer, n, port);
  }
  {
    size_t buflen(HEADERLEN + sizeof(endpoint_t));
    char buffer[buflen];
    size_t n(packmsg(buffer, buflen, secret, cid, PORT_SETLOCALIP, 0,
                     (const char*)(&localep), sizeof(endpoint_t)));
    send(buffer, n, port);
  }
}

char* ovbox_udpsocket_t::recv_sec_msg(char* inputbuf, size_t& ilen, size_t& len,
                                      stage_device_id_t& cid, port_t& destport,
                                      sequence_t& seq, endpoint_t& addr)
{
  ssize_t ilens = recvfrom(inputbuf, ilen, addr);
  if(ilens < 0) {
    ilen = 0;
    return NULL;
  }
  ilen = ilens;
  if(ilen < HEADERLEN)
    return NULL;
  // check secret:
  if(msg_secret(inputbuf) != secret) {
    // log( 0, "invalid secret "+std::to_string(msg_secret(inputbuf)) +" from
    // "+ep2str(addr));
    return NULL;
  }
  cid = msg_callerid(inputbuf);
  destport = msg_port(inputbuf);
  seq = msg_seq(inputbuf);
  len = ilen - HEADERLEN;
  return &(inputbuf[HEADERLEN]);
}

#if defined(__linux__)
std::string getmacaddr()
{
  std::string retv;
  struct ifreq ifr;
  struct ifconf ifc;
  char buf[1024];
  int success = 0;
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if(sock == -1) { /* handle error*/
    return retv;
  };
  ifc.ifc_len = sizeof(buf);
  ifc.ifc_buf = buf;
  if(ioctl(sock, SIOCGIFCONF, &ifc) == -1) { /* handle error */
    return retv;
  }
  struct ifreq* it = ifc.ifc_req;
  const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));
  for(; it != end; ++it) {
    strcpy(ifr.ifr_name, it->ifr_name);
    // ioctl(sock, SIOCGIWNAME, &ifr);
    if(ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
      if(!(ifr.ifr_flags & IFF_LOOPBACK)) { // don't count loopback
        if(ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
          // exclude virtual docker devices:
          if((ifr.ifr_hwaddr.sa_data[0] != 0x02) ||
             (ifr.ifr_hwaddr.sa_data[1] != 0x42)) {
            success = 1;
            break;
          }
        }
      }
    } else { /* handle error */
      return retv;
    }
  }
  unsigned char mac_address[6];
  if(success) {
    memcpy(mac_address, ifr.ifr_hwaddr.sa_data, 6);
    char ctmp[1024];
    sprintf(ctmp, "%02x%02x%02x%02x%02x%02x", mac_address[0], mac_address[1],
            mac_address[2], mac_address[3], mac_address[4], mac_address[5]);
    retv = ctmp;
  }
  return retv;
}
#else
std::string getmacaddr()
{
  std::string retv;
  unsigned char mac_address[6];
  char ctmp[1024];
  if(MACAddressUtility::GetMACAddress(mac_address) == 0) {
    sprintf(ctmp, "%02x%02x%02x%02x%02x%02x", mac_address[0], mac_address[1],
            mac_address[2], mac_address[3], mac_address[4], mac_address[5]);
    retv = ctmp;
  }
  return retv;
}
#endif

endpoint_t getipaddr()
{
  endpoint_t my_addr;
  memset(&my_addr, 0, sizeof(endpoint_t));
#if defined(WIN32) || defined(UNDER_CE)
  DWORD rv, size;
  PIP_ADAPTER_ADDRESSES adapter_addresses, aa;
  PIP_ADAPTER_UNICAST_ADDRESS ua;

  rv = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, NULL,
                            &size);
  if(rv != ERROR_BUFFER_OVERFLOW) {
    fprintf(stderr, "GetAdaptersAddresses() failed...");
    return my_addr;
  }
  adapter_addresses = (PIP_ADAPTER_ADDRESSES)malloc(size);

  rv = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL,
                            adapter_addresses, &size);
  if(rv == ERROR_SUCCESS) {
    for(aa = adapter_addresses; aa != NULL; aa = aa->Next) {
      for(ua = aa->FirstUnicastAddress; ua != NULL; ua = ua->Next) {
        // my_addr = ua->Address.lpSockaddr;
        memcpy(&my_addr, ua->Address.lpSockaddr, sizeof(endpoint_t));
        free(adapter_addresses);
        return my_addr;
      }
    }
  }
  free(adapter_addresses);
#else
  struct ifaddrs* addrs;
  getifaddrs(&addrs);
  struct ifaddrs* tmp = addrs;
  while(tmp) {
    if(tmp->ifa_addr && tmp->ifa_addr->sa_family == AF_INET &&
       (!(tmp->ifa_flags & IFF_LOOPBACK))) {
      memcpy(&my_addr, tmp->ifa_addr, sizeof(endpoint_t));
      return my_addr;
    }
    tmp = tmp->ifa_next;
  }
  freeifaddrs(addrs);
#endif
  return my_addr;
}

/*
 * Local Variables:
 * mode: c++
 * compile-command: "make -C .."
 * End:
 */
