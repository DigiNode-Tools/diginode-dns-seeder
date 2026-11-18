# DigiNode DNS Seeder

A DNS seeder for several blockchain networks. It crawls a network's nodes, keeps track of which ones are reliable and up to date, and serves a list of them from its own small DNS server. New nodes use DNS seeds like this to find their first peers.

It currently supports **DigiByte**, **Nito** and **Litecoin**, mainnet and testnet, chosen with `--chain`. One server can run a seeder for each, side by side.

It's part of [DigiNode Tools](https://diginode.tools), and is the seeder that [DigiNode Seeder](https://github.com/DigiNode-Tools/diginode-seeder-setup) installs. It's based on Pieter Wuille's [bitcoin-seeder](https://github.com/sipa/bitcoin-seeder), by way of DigiByte's seeder.

## The easy way: DigiNode Seeder

To set up seeders on a Linux server with one command, use **DigiNode Seeder**:

```
curl -sSL https://seedsetup.diginode.tools | sudo bash
```

It builds this seeder, runs a seeder for each network you choose as a hardened systemd service, routes each seed name to the right seeder on port 53, checks your DNS records, and keeps everything up to date. See the [DigiNode Seeder guide](https://diginode.tools/diginode-seeder/).

DigiNode Seeder sets up DigiByte (mainnet and testnet), Nito (mainnet) and Litecoin (mainnet and testnet) seeders. For anything else, such as Nito testnet, follow the manual steps below.

## Supported networks

| `--chain` | Network | P2P port | Good nodes must have |
| --- | --- | --- | --- |
| `digibyte` (default) | DigiByte mainnet | 12024 | protocol 70017 or later (DigiByte Core v8/v9), block height 18,701,000 or more |
| `digibyte-testnet` | DigiByte testnet (v9, testnet26) | 12033 | protocol 70017 or later |
| `nito` | Nito mainnet | 8820 | protocol 70017 and the `NITO_PRIVATE` service bit (Nito Core v4), block height 1,100,000 or more |
| `nito-testnet` | Nito testnet (testnet3) | 8840 | protocol 70017 and `NITO_PRIVATE` |
| `litecoin` | Litecoin mainnet | 9333 | protocol 70017 and the `MWEB` service bit (Litecoin Core v0.21), block height 3,150,000 or more |
| `litecoin-testnet` | Litecoin testnet (testnet4) | 19335 | protocol 70017 and `MWEB` |

`--testnet` switches to the chain's testnet, so `--chain nito --testnet` is the same as `--chain nito-testnet`.

Litecoin Core prefers peers that support MWEB (its privacy extension) and asks seeds for `x1000009.<seed>` (network, witness and MWEB nodes), so the Litecoin seeder lists only MWEB nodes. MWEB uses the same service bit as Nito's private transactions, each on its own network.

Nito Core v4 disconnects peers that aren't v4 once the v4 hard fork activates (mainnet block 1,165,432), so the Nito seeder lists only v4 nodes. It also advertises `NITO_PRIVATE` itself, so v4 nodes keep talking to it. Nito Core asks seeds for `x1000009.<seed>` (network, witness and private-transaction nodes), which the seeder answers.

Nito's testnet has no working DNS seeds or fixed nodes at the moment, so a `nito-testnet` seeder needs at least one node to start from: add it with `--seed <ip or hostname>`.

## Features

- Revisits known nodes regularly to check they're still reachable and up to date.
- Bans nodes after too many failures or bad behaviour.
- Keeps reliability statistics over 2 hours, 8 hours, 1 day, 1 week and 30 days, and lists only good nodes.
- Answers service-filter queries such as `x9.<seed>`, so nodes can ask only for peers with the features they need.
- Answers A, AAAA, NS and SOA queries for its own name, and refuses everything else.
- Keeps its node list in `dnsseed.dat`, so a restart doesn't start from scratch.
- Very light: a few tens of megabytes of memory and little CPU. By default 96 crawlers run in parallel.

## Build

On Debian or Ubuntu:

```
sudo apt-get update
sudo apt-get install build-essential libboost-dev libssl-dev git
git clone https://github.com/DigiNode-Tools/diginode-dns-seeder
cd diginode-dns-seeder
make
```

This builds the `dnsseed` program.

## DNS records

To run a seed at `seed.example.com`, you need two records in `example.com`'s DNS:

| Type | Name | Points to |
| --- | --- | --- |
| A | `server.example.com` | your server's IP address (add an AAAA too if it has IPv6) |
| NS | `seed.example.com` | `server.example.com` |

The NS record hands `seed.example.com` to your seeder. If you use Cloudflare, the A record must be **DNS only** (grey cloud), not proxied. Several seeds (e.g. `seed.example.com` and `nitoseed.example.com`) can point at the same server.

Check them with:

```
dig NS seed.example.com +short
dig A server.example.com +short
```

## Run

```
./dnsseed --chain nito -h seed.example.com -n server.example.com -m you.example.com -a 203.0.113.10
```

- `-h` is the seed's name (the NS record's name).
- `-n` is the server's name (what the NS record points to).
- `-m` is a contact email for the SOA record, with the `@` changed to a dot.
- `-a` is the address to listen on. Use the server's own public address: on Ubuntu, systemd-resolved already listens on port 53 at 127.0.0.53, and binding to all addresses clashes with it.
- Leave out `--chain` for DigiByte mainnet.

It starts crawling straight away. After a minute or two the status line's available count should climb. Test it with `dig @203.0.113.10 seed.example.com A +short`, then from anywhere with `dig seed.example.com A +short`.

It writes `dnsseed.dat` (its node list), `dnsseed.dump` (a readable table of nodes it knows, with versions and reliability) and `dnsstats.log` into the folder it runs in. Give each seeder its own folder.

### Port 53

Listening on port 53 needs root, or the right capability. To run it as an ordinary user:

```
sudo setcap 'cap_net_bind_service=+ep' ./dnsseed
```

Open UDP port 53 in your firewall (`sudo ufw allow 53/udp`, plus any firewall at your hosting provider). The seeder answers over UDP only.

### Several seeders on one server

Only one program can listen on port 53 for a given address. To run more than one seeder (say DigiByte mainnet, DigiByte testnet and Nito) on a server with one IP address, run each on its own local port, e.g. `-a 127.0.0.1 -p 5301`, and put a DNS front end on port 53 that sends each seed name to its seeder. DigiNode Seeder does this with dnsdist, which also answers over TCP and limits how fast any one address can query.

### Keep it running

Run it as a systemd service so it starts at boot and restarts if it stops. Set `WorkingDirectory` to the seeder's own folder, so it finds its `dnsseed.dat` again. DigiNode Seeder sets this up for you.

## Options

```
Usage: ./dnsseed -h <host> -n <ns> [-m <mbox>] [-t <threads>] [-p <port>]

Options:
-h <host>       Hostname of the DNS seed
-n <ns>         Hostname of the nameserver
-m <mbox>       E-Mail address reported in SOA records
-t <threads>    Number of crawlers to run in parallel (default 96)
-d <threads>    Number of DNS server threads (default 4)
-a <address>    Address to listen on (default ::)
-p <port>       UDP port to listen on (default 53)
-o <ip:port>    Tor proxy IP/Port
-i <ip:port>    IPV4 SOCKS5 proxy IP/Port
-k <ip:port>    IPV6 SOCKS5 proxy IP/Port
-w f1,f2,...    Allow these flag combinations as filters
--chain <id>    Network to seed (default digibyte):
                digibyte, digibyte-testnet, nito, nito-testnet
--testnet       Use the chain's testnet
--wipeban       Wipe list of banned nodes
--wipeignore    Wipe list of ignored nodes

These override the chain's own settings:
--p2pport <n>   P2P port nodes must use
--magic <hex>   Network message start, 8 hex digits
--minheight <n> Lowest block height a node may report
--minversion <n>   Lowest protocol version a node may use
--services <bits>  Service bits a node must advertise (e.g. 0x1000000)
--localservices <bits>  Service bits the seeder advertises
--useragent <s> User agent the seeder sends
--seed <host>   Extra DNS seed or node to start crawling from
                (can be given more than once)
-?, --help      Show this text
```

## Getting your seed used

A working seed only helps once the network's node software knows about it. When your seed has been running reliably for a while, ask that network's developers to add it to the list of DNS seeds in their node software (`chainparams.cpp` in DigiByte Core and Nito Core). DigiNode Seeder's status page shows whether your seed is in DigiByte Core yet, and the exact line to add.

## Adding a network

Each network is one entry in the `kChains` table at the top of `main.cpp`: its message start (magic), P2P port, minimum block height and protocol version, service bits, and the DNS seeds and fixed nodes to start crawling from. Nothing else in the program is network-specific, so a Bitcoin-based network can usually be added there alone.

## Licence

DigiNode Tools' changes are under the MIT licence. The code from Bitcoin keeps its MIT licence, and the crawler and DNS server come from Pieter Wuille's bitcoin-seeder, which doesn't state a licence. [LICENSE](LICENSE) has the details.

DigiNode Seeder, the setup script, has its own licence: see its [repository](https://github.com/DigiNode-Tools/diginode-seeder-setup).
