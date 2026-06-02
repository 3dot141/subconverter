#ifndef CHAINGEN_H_INCLUDED
#define CHAINGEN_H_INCLUDED

#include <string>
#include <vector>

#include "config/chain.h"
#include "config/proxygroup.h"
#include "parser/config/proxy.h"
#include "utils/ini_reader/ini_reader.h"

struct ResolvedChain
{
    std::string name;
    bool        frontIsRef = false;  // Front was "[]Group"
    std::string frontGroup;          // referenced group, or generated "<name>-front"
    std::string frontFilter;         // node regex when !frontIsRef
    std::string frontType;           // helper group type ("select"/"url-test")
    std::string landingTag;          // resolved unique landing node remark
    std::string landingServer;       // landing node hostname (IP or domain)
    bool        landingIsIP = false;
    bool        valid = false;
};

// Resolve each chain against the node pool. Invalid chains (landing not unique) get valid=false and a warning.
std::vector<ResolvedChain> resolveChains(const ChainConfigs &chains, std::vector<Proxy> &nodes);

// Clash: append a "<name>-front" helper group (when front is a regex) and a relay "<name>" group per valid chain.
void appendClashChains(const std::vector<ResolvedChain> &chains, ProxyGroupConfigs &groups);

// QuanX: "static=<name>-front, server-tag-regex=<filter>" lines for chains whose front is a regex.
std::vector<std::string> quanXFrontPolicies(const std::vector<ResolvedChain> &chains);

// QuanX: backhaul filter lines "ip-cidr, <ip>/32, <front>" or "host, <domain>, <front>" (deduped by landing+front).
std::vector<std::string> quanXBackhaulRules(const std::vector<ResolvedChain> &chains);

// QuanX: rewrite filter_local lines whose policy field == a chain name into
// "<type>, <pattern>, <landingTag>, ..., via-interface=%TUN%".
void rewriteQuanXChainRules(INIReader &ini, const std::vector<ResolvedChain> &chains);

#endif // CHAINGEN_H_INCLUDED
