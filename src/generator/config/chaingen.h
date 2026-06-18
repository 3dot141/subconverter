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
    bool        landingIsGroupRef = false;   // landing was written as "[]Group"
    std::string landingGroup;                // referenced group name (QuanX policy slot)
    struct LandingNode { std::string tag; std::string server; };
    std::vector<LandingNode> landingNodes;   // resolved landing node set (size==1 for single-node)
    bool        valid = false;
};

// Resolve each chain against the node pool. Invalid chains (landing not unique / group missing) get valid=false + warning.
// proxyGroups is used to look up "[]Group" landing references against custom_proxy_group definitions.
std::vector<ResolvedChain> resolveChains(const ChainConfigs &chains, std::vector<Proxy> &nodes,
                                         const ProxyGroupConfigs &proxyGroups);

// Clash inject (called BEFORE the node loop): set UnderlyingProxy on landing nodes; returns resolved chains via outResolved.
void injectClashChains(const ChainConfigs &chains, std::vector<Proxy> &nodes,
                       const ProxyGroupConfigs &proxyGroups,
                       std::vector<ResolvedChain> &outResolved);

// Clash (called AFTER the node loop): append "<name>-front" helper groups; no longer generates a relay group.
void appendClashFrontGroups(const std::vector<ResolvedChain> &chains, ProxyGroupConfigs &groups);

// QuanX: chain-name policy + front helper policy lines for [policy] section.
std::vector<std::string> quanXFrontPolicies(const std::vector<ResolvedChain> &chains);

// QuanX: backhaul filter lines "ip-cidr, <ip>/32, <front>" or "host, <domain>, <front>" (deduped by landing+front).
std::vector<std::string> quanXBackhaulRules(const std::vector<ResolvedChain> &chains);

// QuanX: write backhaul rules into the chain_filter section (served by /getLanding endpoint).
void writeQuanXChainFilter(INIReader &ini, const std::vector<ResolvedChain> &chains);

#endif // CHAINGEN_H_INCLUDED
