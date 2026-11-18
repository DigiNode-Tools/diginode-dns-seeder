#include <algorithm>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <atomic>
#include <errno.h>
#include <string.h>
#include <string>
#include <vector>

#include "bitcoin.h"
#include "db.h"

using namespace std;

#define SEEDER_VERSION "0.2.0"

// Network settings (declared in protocol.h and db.h). main() fills them in
// from the chosen chain, then from any options that override it.
unsigned short nP2PPort = 0;
uint64_t nLocalServices = 0;
std::string strUserAgent = "/diginode-dns-seeder:" SEEDER_VERSION "/";
int nRequireHeight = 0;
int nRequireVersion = 0;
uint64_t nRequireServices = 0;

// Service bits that aren't in Bitcoin's list (protocol.h).
static const uint64_t NODE_NITO_PRIVATE = (1 << 24); // Nito v4 private transactions

// Everything the seeder needs to know about one network. To add a chain,
// add an entry to kChains: nothing else here is chain-specific.
struct ChainParams {
  const char *id;               // --chain value
  const char *name;             // shown in the log
  const char *testnet_id;       // chain --testnet switches to, or NULL
  const char *magic;            // P2P message start, 8 hex digits
  unsigned short p2p_port;      // port nodes must listen on
  int min_height;               // lowest block height a good node may report
  int min_version;              // lowest protocol version a good node may use
  uint64_t require_services;    // service bits a good node must advertise
  uint64_t local_services;      // service bits we advertise (some chains drop
                                // peers that don't advertise their own bits)
  uint64_t filter_services;     // extra bits added to the default x... filters
  std::vector<std::string> seeds;  // DNS seeds to start crawling from
  std::vector<std::string> nodes;  // fixed IP addresses to start crawling from
};

static const std::vector<ChainParams> kChains = {
  // DigiByte Core v8/v9 mainnet. Seeds as in DigiByte Core v9.26.7.
  {"digibyte", "DigiByte", "digibyte-testnet", "fac3b6da", 12024, 18701000, 70017, 0, 0, 0,
   {"seed.digibyte.io", "seed.diginode.tools", "seed.digibyte.link", "seed.aroundtheblock.app", "seed.tuyul.cc"},
   {"81.153.89.168"}},
  // DigiByte Core v9 testnet (testnet26), a new chain started again from
  // block 0. v8's testnet used fdc8bddd on port 12026 (minimum height 2755000).
  {"digibyte-testnet", "DigiByte testnet", NULL, "fec6b9e7", 12033, 0, 70017, 0, 0, 0,
   {"testnetseed.digibyte.io", "testnetseed.digibyte.link", "testnetseed.digibyte.services"},
   {}},
  // Nito Core v4 mainnet. From the v4 hard fork (block 1,165,432) v4 nodes
  // drop peers that don't use protocol 70017 and advertise NITO_PRIVATE, and
  // Nito Core asks seeds for x1000009 (NETWORK | WITNESS | NITO_PRIVATE). So
  // only v4 nodes are listed, and the seeder advertises NITO_PRIVATE itself.
  {"nito", "Nito", "nito-testnet", "b6c0c0b6", 8820, 1100000, 70017, NODE_NITO_PRIVATE, NODE_NITO_PRIVATE, NODE_NITO_PRIVATE,
   {"seed.nito.network", "seed.nitopool.online"},
   {"88.198.163.206", "217.160.149.211", "87.106.216.17"}},
  // Nito Core v4 testnet3 (private transactions active from block 0).
  {"nito-testnet", "Nito testnet", NULL, "cd9b721a", 8840, 0, 70017, NODE_NITO_PRIVATE, NODE_NITO_PRIVATE, NODE_NITO_PRIVATE,
   {"t1.testnet.nito.network", "t2.testnet.nito.network", "t3.testnet.nito.network", "testnet-seed.nito.network"},
   {}},
};

static const ChainParams *FindChain(const char *id) {
  for (const ChainParams &c : kChains) {
    if (strcmp(c.id, id) == 0) return &c;
  }
  return NULL;
}

// Parses a whole number, decimal or 0x... hex. Returns false if it isn't one.
static bool ParseU64(const char *s, uint64_t &out) {
  char *end;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 0);
  if (errno || end == s || *end != 0 || *s == '-') return false;
  out = v;
  return true;
}

// Parses 8 hex digits (e.g. fec6b9e7) into the 4 message-start bytes.
static bool ParseMagic(const char *hex, unsigned char out[4]) {
  if (strlen(hex) != 8) return false;
  for (int i = 0; i < 4; i++) {
    char byte[3] = { hex[i*2], hex[i*2+1], 0 };
    char *end;
    unsigned long v = strtoul(byte, &end, 16);
    if (*end != 0) return false;
    out[i] = (unsigned char)v;
  }
  return true;
}

class CDnsSeedOpts {
public:
  int nThreads;
  int nPort;
  int nDnsThreads;
  int fUseTestNet;
  int fWipeBan;
  int fWipeIgnore;
  const char *mbox;
  const char *ns;
  const char *host;
  const char *tor;
  const char *ip_addr;
  const char *ipv4_proxy;
  const char *ipv6_proxy;
  const char *magic;
  const char *chain;
  const char *useragent;
  int nP2PPortOpt;
  int nMinHeightOpt;
  int nMinVersionOpt;
  int fRequireServicesSet;
  uint64_t nRequireServicesOpt;
  int fLocalServicesSet;
  uint64_t nLocalServicesOpt;
  std::vector<std::string> extra_seeds;
  std::set<uint64_t> filter_whitelist;

  CDnsSeedOpts() : nThreads(96), nDnsThreads(4), ip_addr("::"), nPort(53), mbox(NULL), ns(NULL), host(NULL), tor(NULL), fUseTestNet(false), fWipeBan(false), fWipeIgnore(false), ipv4_proxy(NULL), ipv6_proxy(NULL), magic(NULL), chain("digibyte"), useragent(NULL), nP2PPortOpt(0), nMinHeightOpt(-1), nMinVersionOpt(-1), fRequireServicesSet(0), nRequireServicesOpt(0), fLocalServicesSet(0), nLocalServicesOpt(0) {}

  void ParseCommandLine(int argc, char **argv) {
    static const char *help = "DigiNode DNS Seeder " SEEDER_VERSION "\n"
                              "Usage: %s -h <host> -n <ns> [-m <mbox>] [-t <threads>] [-p <port>]\n"
                              "\n"
                              "Options:\n"
                              "-h <host>       Hostname of the DNS seed\n"
                              "-n <ns>         Hostname of the nameserver\n"
                              "-m <mbox>       E-Mail address reported in SOA records\n"
                              "-t <threads>    Number of crawlers to run in parallel (default 96)\n"
                              "-d <threads>    Number of DNS server threads (default 4)\n"
                              "-a <address>    Address to listen on (default ::)\n"
                              "-p <port>       UDP port to listen on (default 53)\n"
                              "-o <ip:port>    Tor proxy IP/Port\n"
                              "-i <ip:port>    IPV4 SOCKS5 proxy IP/Port\n"
                              "-k <ip:port>    IPV6 SOCKS5 proxy IP/Port\n"
                              "-w f1,f2,...    Allow these flag combinations as filters\n"
                              "--chain <id>    Network to seed (default digibyte):\n"
                              "                digibyte, digibyte-testnet, nito, nito-testnet\n"
                              "--testnet       Use the chain's testnet\n"
                              "--wipeban       Wipe list of banned nodes\n"
                              "--wipeignore    Wipe list of ignored nodes\n"
                              "\n"
                              "These override the chain's own settings:\n"
                              "--p2pport <n>   P2P port nodes must use\n"
                              "--magic <hex>   Network message start, 8 hex digits\n"
                              "--minheight <n> Lowest block height a node may report\n"
                              "--minversion <n>   Lowest protocol version a node may use\n"
                              "--services <bits>  Service bits a node must advertise (e.g. 0x1000000)\n"
                              "--localservices <bits>  Service bits the seeder advertises\n"
                              "--useragent <s> User agent the seeder sends\n"
                              "--seed <host>   Extra DNS seed or node to start crawling from\n"
                              "                (can be given more than once)\n"
                              "-?, --help      Show this text\n"
                              "\n";
    bool showHelp = false;

    while(1) {
      static struct option long_options[] = {
        {"host", required_argument, 0, 'h'},
        {"ns",   required_argument, 0, 'n'},
        {"mbox", required_argument, 0, 'm'},
        {"threads", required_argument, 0, 't'},
        {"dnsthreads", required_argument, 0, 'd'},
        {"address", required_argument, 0, 'a'},
        {"port", required_argument, 0, 'p'},
        {"onion", required_argument, 0, 'o'},
        {"proxyipv4", required_argument, 0, 'i'},
        {"proxyipv6", required_argument, 0, 'k'},
        {"filter", required_argument, 0, 'w'},
        {"testnet", no_argument, &fUseTestNet, 1},
        {"wipeban", no_argument, &fWipeBan, 1},
        {"wipeignore", no_argument, &fWipeIgnore, 1},
        {"p2pport", required_argument, 0, 1001},
        {"magic", required_argument, 0, 1002},
        {"minheight", required_argument, 0, 1003},
        {"seed", required_argument, 0, 1004},
        {"chain", required_argument, 0, 1005},
        {"minversion", required_argument, 0, 1006},
        {"services", required_argument, 0, 1007},
        {"localservices", required_argument, 0, 1008},
        {"useragent", required_argument, 0, 1009},
        {"help", no_argument, 0, '?'},
        {0, 0, 0, 0}
      };
      int option_index = 0;
      int c = getopt_long(argc, argv, "h:n:m:t:a:p:d:o:i:k:w:", long_options, &option_index);
      if (c == -1) break;
      switch (c) {
        case 'h': {
          host = optarg;
          break;
        }
        
        case 'm': {
          mbox = optarg;
          break;
        }
        
        case 'n': {
          ns = optarg;
          break;
        }
        
        case 't': {
          int n = strtol(optarg, NULL, 10);
          if (n > 0 && n < 1000) nThreads = n;
          break;
        }

        case 'd': {
          int n = strtol(optarg, NULL, 10);
          if (n > 0 && n < 1000) nDnsThreads = n;
          break;
        }

        case 'a': {
          if (strchr(optarg, ':')==NULL) {
            char* ip4_addr = (char*) malloc(strlen(optarg)+8);
            strcpy(ip4_addr, "::FFFF:");
            strcat(ip4_addr, optarg);
            ip_addr = ip4_addr;
          } else {
            ip_addr = optarg;
          }
          break;
        }

        case 'p': {
          int p = strtol(optarg, NULL, 10);
          if (p > 0 && p < 65536) nPort = p;
          break;
        }

        case 'o': {
          tor = optarg;
          break;
        }

        case 1001: {
          int p = strtol(optarg, NULL, 10);
          if (p > 0 && p < 65536) {
            nP2PPortOpt = p;
          } else {
            fprintf(stderr, "Invalid --p2pport: %s\n", optarg);
            exit(1);
          }
          break;
        }

        case 1002: {
          magic = optarg;
          break;
        }

        case 1003: {
          char *end;
          long h = strtol(optarg, &end, 10);
          if (*end == 0 && h >= 0) {
            nMinHeightOpt = (int)h;
          } else {
            fprintf(stderr, "Invalid --minheight: %s\n", optarg);
            exit(1);
          }
          break;
        }

        case 1004: {
          extra_seeds.push_back(optarg);
          break;
        }

        case 1005: {
          chain = optarg;
          break;
        }

        case 1006: {
          uint64_t v;
          if (!ParseU64(optarg, v) || v > 0x7fffffff) {
            fprintf(stderr, "Invalid --minversion: %s\n", optarg);
            exit(1);
          }
          nMinVersionOpt = (int)v;
          break;
        }

        case 1007: {
          if (!ParseU64(optarg, nRequireServicesOpt)) {
            fprintf(stderr, "Invalid --services: %s\n", optarg);
            exit(1);
          }
          fRequireServicesSet = 1;
          break;
        }

        case 1008: {
          if (!ParseU64(optarg, nLocalServicesOpt)) {
            fprintf(stderr, "Invalid --localservices: %s\n", optarg);
            exit(1);
          }
          fLocalServicesSet = 1;
          break;
        }

        case 1009: {
          useragent = optarg;
          break;
        }

        case 'i': {
          ipv4_proxy = optarg;
          break;
        }

        case 'k': {
          ipv6_proxy = optarg;
          break;
        }

        case 'w': {
          char* ptr = optarg;
          while (*ptr != 0) {
            unsigned long l = strtoul(ptr, &ptr, 0);
            if (*ptr == ',') {
                ptr++;
            } else if (*ptr != 0) {
                break;
            }
            filter_whitelist.insert(l);
          }
          break;
        }

        case '?': {
          showHelp = true;
          break;
        }
      }
    }
    if (host != NULL && ns == NULL) showHelp = true;
    if (showHelp) {
        fprintf(stderr, help, argv[0]);
        exit(0);
    }
  }
};

// The x... filters answered when -w isn't given: Bitcoin's usual set, plus
// each of them with the chain's own extra bits (e.g. x1000009 for Nito).
static void AddDefaultFilters(std::set<uint64_t> &filters, uint64_t extra) {
  static const uint64_t base[] = {
    NODE_NETWORK, // x1
    NODE_NETWORK | NODE_BLOOM, // x5
    NODE_NETWORK | NODE_WITNESS, // x9
    NODE_NETWORK | NODE_WITNESS | NODE_COMPACT_FILTERS, // x49
    NODE_NETWORK | NODE_WITNESS | NODE_BLOOM, // xd
    NODE_NETWORK_LIMITED, // x400
    NODE_NETWORK_LIMITED | NODE_BLOOM, // x404
    NODE_NETWORK_LIMITED | NODE_WITNESS, // x408
    NODE_NETWORK_LIMITED | NODE_NETWORK | NODE_WITNESS, // x409
    NODE_NETWORK_LIMITED | NODE_WITNESS | NODE_COMPACT_FILTERS, // x448
    NODE_NETWORK_LIMITED | NODE_WITNESS | NODE_BLOOM, // x40c
    NODE_NETWORK_LIMITED | NODE_NETWORK | NODE_WITNESS | NODE_BLOOM, // x40d
  };
  for (uint64_t f : base) {
    filters.insert(f);
    if (extra) filters.insert(f | extra);
  }
}

#include "dns.h"

CAddrDb db;

extern "C" void* ThreadCrawler(void* data) {
  int *nThreads=(int*)data;
  do {
    std::vector<CServiceResult> ips;
    int wait = 5;
    db.GetMany(ips, 16, wait);
    int64 now = time(NULL);
    if (ips.empty()) {
      wait *= 1000;
      wait += rand() % (500 * *nThreads);
      Sleep(wait);
      continue;
    }
    vector<CAddress> addr;
    for (int i=0; i<ips.size(); i++) {
      CServiceResult &res = ips[i];
      res.nBanTime = 0;
      res.nClientV = 0;
      res.nHeight = 0;
      res.strClientV = "";
      res.services = 0;
      bool getaddr = res.ourLastSuccess + 86400 < now;
      res.fGood = TestNode(res.service,res.nBanTime,res.nClientV,res.strClientV,res.nHeight,getaddr ? &addr : NULL, res.services);
    }
    db.ResultMany(ips);
    db.Add(addr);
  } while(1);
  return nullptr;
}

extern "C" int GetIPList(void *thread, char *requestedHostname, addr_t *addr, int max, int ipv4, int ipv6);

class CDnsThread {
public:
  struct FlagSpecificData {
      int nIPv4, nIPv6;
      std::vector<addr_t> cache;
      time_t cacheTime;
      unsigned int cacheHits;
      FlagSpecificData() : nIPv4(0), nIPv6(0), cacheTime(0), cacheHits(0) {}
  };

  dns_opt_t dns_opt; // must be first
  const int id;
  std::map<uint64_t, FlagSpecificData> perflag;
  std::atomic<uint64_t> dbQueries;
  std::set<uint64_t> filterWhitelist;

  void cacheHit(uint64_t requestedFlags, bool force = false) {
    static bool nets[NET_MAX] = {};
    if (!nets[NET_IPV4]) {
        nets[NET_IPV4] = true;
        nets[NET_IPV6] = true;
    }
    time_t now = time(NULL);
    FlagSpecificData& thisflag = perflag[requestedFlags];
    thisflag.cacheHits++;
    if (force || thisflag.cacheHits * 400 > (thisflag.cache.size()*thisflag.cache.size()) || (thisflag.cacheHits*thisflag.cacheHits * 20 > thisflag.cache.size() && (now - thisflag.cacheTime > 5))) {
      set<CNetAddr> ips;
      db.GetIPs(ips, requestedFlags, 1000, nets);
      dbQueries++;
      thisflag.cache.clear();
      thisflag.nIPv4 = 0;
      thisflag.nIPv6 = 0;
      thisflag.cache.reserve(ips.size());
      for (set<CNetAddr>::iterator it = ips.begin(); it != ips.end(); it++) {
        struct in_addr addr;
        struct in6_addr addr6;
        if ((*it).GetInAddr(&addr)) {
          addr_t a;
          a.v = 4;
          memcpy(&a.data.v4, &addr, 4);
          thisflag.cache.push_back(a);
          thisflag.nIPv4++;
        } else if ((*it).GetIn6Addr(&addr6)) {
          addr_t a;
          a.v = 6;
          memcpy(&a.data.v6, &addr6, 16);
          thisflag.cache.push_back(a);
          thisflag.nIPv6++;
        }
      }
      thisflag.cacheHits = 0;
      thisflag.cacheTime = now;
    }
  }

  CDnsThread(CDnsSeedOpts* opts, int idIn) : id(idIn) {
    dns_opt.host = opts->host;
    dns_opt.ns = opts->ns;
    dns_opt.mbox = opts->mbox;
    dns_opt.datattl = 3600;
    dns_opt.nsttl = 40000;
    dns_opt.cb = GetIPList;
    dns_opt.addr = opts->ip_addr;
    dns_opt.port = opts->nPort;
    dns_opt.nRequests = 0;
    dbQueries = 0;
    perflag.clear();
    filterWhitelist = opts->filter_whitelist;
  }

  void run() {
    dnsserver(&dns_opt);
  }
};

extern "C" int GetIPList(void *data, char *requestedHostname, addr_t* addr, int max, int ipv4, int ipv6) {
  CDnsThread *thread = (CDnsThread*)data;

  uint64_t requestedFlags = 0;
  int hostlen = strlen(requestedHostname);
  if (hostlen > 1 && requestedHostname[0] == 'x' && requestedHostname[1] != '0') {
    char *pEnd;
    uint64_t flags = (uint64_t)strtoull(requestedHostname+1, &pEnd, 16);
    if (*pEnd == '.' && pEnd <= requestedHostname+17 && std::find(thread->filterWhitelist.begin(), thread->filterWhitelist.end(), flags) != thread->filterWhitelist.end())
      requestedFlags = flags;
    else
      return 0;
  }
  else if (strcasecmp(requestedHostname, thread->dns_opt.host))
    return 0;
  thread->cacheHit(requestedFlags);
  auto& thisflag = thread->perflag[requestedFlags];
  unsigned int size = thisflag.cache.size();
  unsigned int maxmax = (ipv4 ? thisflag.nIPv4 : 0) + (ipv6 ? thisflag.nIPv6 : 0);
  if (max > size)
    max = size;
  if (max > maxmax)
    max = maxmax;
  int i=0;
  while (i<max) {
    int j = i + (rand() % (size - i));
    do {
        bool ok = (ipv4 && thisflag.cache[j].v == 4) ||
                  (ipv6 && thisflag.cache[j].v == 6);
        if (ok) break;
        j++;
        if (j==size)
            j=i;
    } while(1);
    addr[i] = thisflag.cache[j];
    thisflag.cache[j] = thisflag.cache[i];
    thisflag.cache[i] = addr[i];
    i++;
  }
  return max;
}

vector<CDnsThread*> dnsThread;

extern "C" void* ThreadDNS(void* arg) {
  CDnsThread *thread = (CDnsThread*)arg;
  thread->run();
  return nullptr;
}

int StatCompare(const CAddrReport& a, const CAddrReport& b) {
  if (a.uptime[4] == b.uptime[4]) {
    if (a.uptime[3] == b.uptime[3]) {
      return a.clientVersion > b.clientVersion;
    } else {
      return a.uptime[3] > b.uptime[3];
    }
  } else {
    return a.uptime[4] > b.uptime[4];
  }
}

extern "C" void* ThreadDumper(void*) {
  int count = 0;
  do {
    Sleep(100000 << count); // First 100s, than 200s, 400s, 800s, 1600s, and then 3200s forever
    if (count < 5)
        count++;
    {
      vector<CAddrReport> v = db.GetAll();
      sort(v.begin(), v.end(), StatCompare);
      FILE *f = fopen("dnsseed.dat.new","w+");
      if (f) {
        {
          CAutoFile cf(f);
          cf << db;
        }
        rename("dnsseed.dat.new", "dnsseed.dat");
      }
      FILE *d = fopen("dnsseed.dump", "w");
      fprintf(d, "# address                                        good  lastSuccess    %%(2h)   %%(8h)   %%(1d)   %%(7d)  %%(30d)  blocks      svcs  version\n");
      double stat[5]={0,0,0,0,0};
      for (vector<CAddrReport>::const_iterator it = v.begin(); it < v.end(); it++) {
        CAddrReport rep = *it;
        fprintf(d, "%-47s  %4d  %11" PRId64 "  %6.2f%% %6.2f%% %6.2f%% %6.2f%% %6.2f%%  %6i  %08" PRIx64 "  %5i \"%s\"\n", rep.ip.ToString().c_str(), (int)rep.fGood, rep.lastSuccess, 100.0*rep.uptime[0], 100.0*rep.uptime[1], 100.0*rep.uptime[2], 100.0*rep.uptime[3], 100.0*rep.uptime[4], rep.blocks, rep.services, rep.clientVersion, rep.clientSubVersion.c_str());
        stat[0] += rep.uptime[0];
        stat[1] += rep.uptime[1];
        stat[2] += rep.uptime[2];
        stat[3] += rep.uptime[3];
        stat[4] += rep.uptime[4];
      }
      fclose(d);
      FILE *ff = fopen("dnsstats.log", "a");
      fprintf(ff, "%llu %g %g %g %g %g\n", (unsigned long long)(time(NULL)), stat[0], stat[1], stat[2], stat[3], stat[4]);
      fclose(ff);
    }
  } while(1);
  return nullptr;
}

extern "C" void* ThreadStats(void*) {
  // In a terminal: one line, redrawn every second. Otherwise (e.g. a systemd
  // service, whose output goes to the journal): a plain line every 10 minutes.
  const bool tty = isatty(fileno(stdout));
  bool first = true;
  do {
    char c[256];
    time_t tim = time(NULL);
    struct tm *tmp = localtime(&tim);
    strftime(c, 256, "[%y-%m-%d %H:%M:%S]", tmp);
    CAddrDbStats stats;
    db.GetStats(stats);
    if (tty) {
      if (first)
      {
        first = false;
        printf("\n\n\n\x1b[3A");
      }
      else
        printf("\x1b[2K\x1b[u");
      printf("\x1b[s");
    }
    uint64_t requests = 0;
    uint64_t queries = 0;
    for (unsigned int i=0; i<dnsThread.size(); i++) {
      requests += dnsThread[i]->dns_opt.nRequests;
      queries += dnsThread[i]->dbQueries;
    }
    printf("%s %i/%i available (%i tried in %is, %i new, %i active), %i banned; %llu DNS requests, %llu db queries%s", c, stats.nGood, stats.nAvail, stats.nTracked, stats.nAge, stats.nNew, stats.nAvail - stats.nTracked - stats.nNew, stats.nBanned, (unsigned long long)requests, (unsigned long long)queries, tty ? "" : "\n");
    Sleep(tty ? 1000 : 600000);
  } while(1);
  return nullptr;
}

// The chain being seeded, and extra seeds from --seed.
static const ChainParams *chain = NULL;
static std::vector<std::string> extra_seeds;

extern "C" void* ThreadSeeder(void*) {
  for (const std::string &ip : chain->nodes) {
    db.Add(CService(ip.c_str(), GetDefaultPort()), true);
  }
  do {
    std::vector<std::string> all(chain->seeds);
    all.insert(all.end(), extra_seeds.begin(), extra_seeds.end());
    for (size_t i = 0; i < all.size(); i++) {
      vector<CNetAddr> ips;
      LookupHost(all[i].c_str(), ips);
      for (vector<CNetAddr>::iterator it = ips.begin(); it != ips.end(); it++) {
        db.Add(CService(*it, GetDefaultPort()), true);
      }
    }
    Sleep(1800000);
  } while(1);
  return nullptr;
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  setbuf(stdout, NULL);
  CDnsSeedOpts opts;
  opts.ParseCommandLine(argc, argv);

  chain = FindChain(opts.chain);
  if (!chain) {
    fprintf(stderr, "Unknown --chain: %s (expected one of:", opts.chain);
    for (const ChainParams &c : kChains) fprintf(stderr, " %s", c.id);
    fprintf(stderr, ")\n");
    exit(1);
  }
  if (opts.fUseTestNet) {
    if (!chain->testnet_id) {
      fprintf(stderr, "--testnet: %s is already a testnet\n", chain->id);
      exit(1);
    }
    chain = FindChain(chain->testnet_id);
  }
  if (!ParseMagic(chain->magic, pchMessageStart)) abort();
  nP2PPort = chain->p2p_port;
  nRequireHeight = chain->min_height;
  nRequireVersion = chain->min_version;
  nRequireServices = chain->require_services;
  nLocalServices = chain->local_services;
  if (opts.magic && !ParseMagic(opts.magic, pchMessageStart)) {
    fprintf(stderr, "Invalid --magic: %s (expected 8 hex digits)\n", opts.magic);
    exit(1);
  }
  if (opts.nP2PPortOpt) nP2PPort = opts.nP2PPortOpt;
  if (opts.nMinHeightOpt >= 0) nRequireHeight = opts.nMinHeightOpt;
  if (opts.nMinVersionOpt >= 0) nRequireVersion = opts.nMinVersionOpt;
  if (opts.fRequireServicesSet) nRequireServices = opts.nRequireServicesOpt;
  if (opts.fLocalServicesSet) nLocalServices = opts.nLocalServicesOpt;
  if (opts.useragent) strUserAgent = opts.useragent;
  if (opts.filter_whitelist.empty()) AddDefaultFilters(opts.filter_whitelist, chain->filter_services);
  extra_seeds = opts.extra_seeds;

  printf("Chain: %s\n", chain->name);
  printf("Network: magic %02x%02x%02x%02x, P2P port %u, minimum height %i\n",
         pchMessageStart[0], pchMessageStart[1], pchMessageStart[2], pchMessageStart[3],
         (unsigned)GetDefaultPort(), GetRequireHeight());
  printf("Good nodes: protocol %i or later, services 0x%" PRIx64 " (we advertise 0x%" PRIx64 ", user agent %s)\n",
         nRequireVersion, nRequireServices, nLocalServices, strUserAgent.c_str());
  printf("Supporting whitelisted filters: ");
  for (std::set<uint64_t>::const_iterator it = opts.filter_whitelist.begin(); it != opts.filter_whitelist.end(); it++) {
      if (it != opts.filter_whitelist.begin()) {
          printf(",");
      }
      printf("0x%lx", (unsigned long)*it);
  }
  printf("\n");
  if (opts.tor) {
    CService service(opts.tor, 9050);
    if (service.IsValid()) {
      printf("Using Tor proxy at %s\n", service.ToStringIPPort().c_str());
      SetProxy(NET_TOR, service);
    }
  }
  if (opts.ipv4_proxy) {
    CService service(opts.ipv4_proxy, 9050);
    if (service.IsValid()) {
      printf("Using IPv4 proxy at %s\n", service.ToStringIPPort().c_str());
      SetProxy(NET_IPV4, service);
    }
  }
  if (opts.ipv6_proxy) {
    CService service(opts.ipv6_proxy, 9050);
    if (service.IsValid()) {
      printf("Using IPv6 proxy at %s\n", service.ToStringIPPort().c_str());
      SetProxy(NET_IPV6, service);
    }
  }
  bool fDNS = true;
  if (!opts.ns) {
    printf("No nameserver set. Not starting DNS server.\n");
    fDNS = false;
  }
  if (fDNS && !opts.host) {
    fprintf(stderr, "No hostname set. Please use -h.\n");
    exit(1);
  }
  if (fDNS && !opts.mbox) {
    fprintf(stderr, "No e-mail address set. Please use -m.\n");
    exit(1);
  }
  FILE *f = fopen("dnsseed.dat","r");
  if (f) {
    printf("Loading dnsseed.dat...");
    CAutoFile cf(f);
    cf >> db;
    if (opts.fWipeBan)
        db.banned.clear();
    if (opts.fWipeIgnore)
        db.ResetIgnores();
    printf("done\n");
  }
  pthread_t threadDns, threadSeed, threadDump, threadStats;
  if (fDNS) {
    printf("Starting %i DNS threads for %s on %s (port %i)...", opts.nDnsThreads, opts.host, opts.ns, opts.nPort);
    dnsThread.clear();
    for (int i=0; i<opts.nDnsThreads; i++) {
      dnsThread.push_back(new CDnsThread(&opts, i));
      pthread_create(&threadDns, NULL, ThreadDNS, dnsThread[i]);
      printf(".");
      Sleep(20);
    }
    printf("done\n");
  }
  printf("Starting seeder...");
  pthread_create(&threadSeed, NULL, ThreadSeeder, NULL);
  printf("done\n");
  printf("Starting %i crawler threads...", opts.nThreads);
  pthread_attr_t attr_crawler;
  pthread_attr_init(&attr_crawler);
  pthread_attr_setstacksize(&attr_crawler, 0x20000);
  for (int i=0; i<opts.nThreads; i++) {
    pthread_t thread;
    pthread_create(&thread, &attr_crawler, ThreadCrawler, &opts.nThreads);
  }
  pthread_attr_destroy(&attr_crawler);
  printf("done\n");
  pthread_create(&threadStats, NULL, ThreadStats, NULL);
  pthread_create(&threadDump, NULL, ThreadDumper, NULL);
  void* res;
  pthread_join(threadDump, &res);
  return 0;
}
