#include "chaingen.h"

#include <set>

#include "utils/network.h"
#include "utils/regexp.h"
#include "utils/logger.h"
#include "utils/string.h"

std::vector<ResolvedChain> resolveChains(const ChainConfigs &chains, std::vector<Proxy> &nodes,
                                         const ProxyGroupConfigs &proxyGroups)
{
    std::vector<ResolvedChain> out;
    writeLog(0, "[chain-debug] resolveChains called with " + std::to_string(chains.size()) + " chain(s), "
                + std::to_string(nodes.size()) + " node(s), " + std::to_string(proxyGroups.size()) + " group(s)", LOG_LEVEL_INFO);
    for(const ChainConfig &c : chains)
    {
        ResolvedChain rc;
        rc.name = c.Name;
        rc.frontType = c.FrontType.empty() ? "select" : c.FrontType;

        // front: "[]Group" reference vs node regex
        if(startsWith(c.Front, "[]"))
        {
            rc.frontIsRef = true;
            rc.frontGroup = c.Front.substr(2);

            // existence hint only: front may reference a base-template group not in proxyGroups; warn, don't invalidate
            bool frontFound = false;
            for(const ProxyGroupConfig &g : proxyGroups)
                if(g.Name == rc.frontGroup)
                {
                    frontFound = true;
                    break;
                }
            if(!frontFound)
                writeLog(0, "Chain '" + c.Name + "' front group '" + rc.frontGroup + "' not found in proxy groups; dialer-proxy may reference a base-template group.", LOG_LEVEL_WARNING);
        }
        else
        {
            rc.frontIsRef = false;
            rc.frontFilter = c.Front;
            rc.frontGroup = c.Name + "-front";
        }

        // landing: "[]Group" reference (a set of VPS) vs a single node (legacy)
        if(startsWith(c.Landing, "[]"))
        {
            rc.landingIsGroupRef = true;
            rc.landingGroup = c.Landing.substr(2);

            // locate the group in custom_proxy_group
            const ProxyGroupConfig *grp = nullptr;
            for(const ProxyGroupConfig &g : proxyGroups)
                if(g.Name == rc.landingGroup)
                {
                    grp = &g;
                    break;
                }
            if(grp == nullptr)
            {
                writeLog(0, "Chain '" + c.Name + "' landing group '" + rc.landingGroup + "' not found; skipping.", LOG_LEVEL_WARNING);
                rc.valid = false;
                out.emplace_back(std::move(rc));
                continue;
            }

            // only support "plain remark-regex filtered" groups: reject groups with []/script:/!! entries
            bool bad = false;
            for(const std::string &rule : grp->Proxies)
                if(startsWith(rule, "[]") || startsWith(rule, "script:") || startsWith(rule, "!!"))
                {
                    bad = true;
                    break;
                }
            if(bad)
            {
                writeLog(0, "Chain '" + c.Name + "' landing group '" + rc.landingGroup + "' not regex-filtered; skipping.", LOG_LEVEL_WARNING);
                rc.valid = false;
                out.emplace_back(std::move(rc));
                continue;
            }

            // union of the group's regex rules against the node pool
            for(Proxy &n : nodes)
                for(const std::string &rule : grp->Proxies)
                    if(regFind(n.Remark, rule))
                    {
                        rc.landingNodes.push_back({n.Remark, n.Hostname});
                        break;
                    }
        }
        else
        {
            // single-node landing (legacy): exact remark first, else regex, require uniqueness
            std::vector<Proxy*> matched;
            for(Proxy &n : nodes)
                if(n.Remark == c.Landing)
                    matched.push_back(&n);
            if(matched.empty())
                for(Proxy &n : nodes)
                    if(regFind(n.Remark, c.Landing))
                        matched.push_back(&n);

            if(matched.size() != 1)
            {
                writeLog(0, "Chain '" + c.Name + "' landing '" + c.Landing + "' resolved to "
                            + std::to_string(matched.size()) + " nodes; skipping.", LOG_LEVEL_WARNING);
                rc.valid = false;
                out.emplace_back(std::move(rc));
                continue;
            }
            rc.landingNodes.push_back({matched[0]->Remark, matched[0]->Hostname});
        }

        if(rc.landingNodes.empty())
        {
            writeLog(0, "Chain '" + c.Name + "' landing has no matching node; skipping.", LOG_LEVEL_WARNING);
            rc.valid = false;
            out.emplace_back(std::move(rc));
            continue;
        }

        rc.valid = true;
        writeLog(0, "[chain-debug] chain '" + rc.name + "' resolved OK: front=" + rc.frontGroup
                    + " landingNodes=" + std::to_string(rc.landingNodes.size())
                    + (rc.landingIsGroupRef ? " (group:" + rc.landingGroup + ")" : " (single)"), LOG_LEVEL_INFO);
        out.emplace_back(std::move(rc));
    }
    return out;
}

void injectClashChains(const ChainConfigs &chains, std::vector<Proxy> &nodes,
                       const ProxyGroupConfigs &proxyGroups,
                       std::vector<ResolvedChain> &outResolved)
{
    outResolved = resolveChains(chains, nodes, proxyGroups);
    for(const ResolvedChain &c : outResolved)
    {
        if(!c.valid)
            continue;
        for(const auto &ln : c.landingNodes)
        {
            bool found = false;
            for(Proxy &n : nodes)
                if(n.Remark == ln.tag)
                {
                    if(!n.UnderlyingProxy.empty())
                        writeLog(0, "Node '" + n.Remark + "' already used as landing by another chain; overwriting dialer-proxy with '" + c.frontGroup + "'.", LOG_LEVEL_WARNING);
                    n.UnderlyingProxy = c.frontGroup;  // node loop reads nodes -> emits dialer-proxy
                    writeLog(0, "[chain-debug] injected dialer-proxy='" + c.frontGroup + "' into node '" + n.Remark + "'", LOG_LEVEL_INFO);
                    found = true;
                    break;
                }
            if(!found)
                writeLog(0, "Chain '" + c.name + "' landing node '" + ln.tag + "' not found in node list; skipping injection.", LOG_LEVEL_WARNING);
        }
    }
}

void appendClashFrontGroups(const std::vector<ResolvedChain> &chains, ProxyGroupConfigs &groups)
{
    for(const ResolvedChain &c : chains)
    {
        if(!c.valid || c.frontIsRef)
            continue;
        ProxyGroupConfig front;
        front.Name = c.frontGroup;
        front.Type = c.frontType == "url-test" ? ProxyGroupType::URLTest : ProxyGroupType::Select;
        if(front.Type == ProxyGroupType::URLTest)
        {
            front.Url = "http://www.gstatic.com/generate_204";
            front.Interval = 300;
        }
        front.Proxies.push_back(c.frontFilter);
        groups.push_back(front);
    }
    // relay group retired (Clash now uses per-node dialer-proxy)
}

std::vector<std::string> quanXFrontPolicies(const std::vector<ResolvedChain> &chains)
{
    std::vector<std::string> out;
    for(const ResolvedChain &c : chains)
    {
        if(!c.valid || c.frontIsRef)
            continue;
        std::string t = c.frontType == "url-test" ? "url-latency-benchmark" : "static";
        std::string line = t + "=" + c.frontGroup + ", server-tag-regex=" + c.frontFilter;
        if(t != "static")
            line += ", check-interval=300";
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> quanXBackhaulRules(const std::vector<ResolvedChain> &chains)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    for(const ResolvedChain &c : chains)
    {
        if(!c.valid)
            continue;
        for(const auto &ln : c.landingNodes)
        {
            // strip port if present (Hostname should be bare, but defend against host:port)
            std::string server = ln.server;
            auto colon = server.rfind(':');
            if(colon != std::string::npos && server.find('.') != std::string::npos && server.find('[') == std::string::npos)
            {
                std::string maybeHost = server.substr(0, colon);
                if(isIPv4(maybeHost))
                    server = maybeHost;
            }

            std::string key = server + "|" + c.frontGroup;
            if(seen.count(key))
                continue;
            seen.insert(key);
            writeLog(0, "[chain-debug] quanX backhaul: server=" + server + " → front=" + c.frontGroup, LOG_LEVEL_INFO);
            if(isIPv4(server))
                out.push_back("ip-cidr, " + server + "/32, " + c.frontGroup);
            else if(isIPv6(server))
                out.push_back("ip6-cidr, " + server + "/128, " + c.frontGroup);
            else
                out.push_back("host, " + server + ", " + c.frontGroup);
        }
    }
    return out;
}

void rewriteQuanXChainRules(INIReader &ini, const std::vector<ResolvedChain> &chains)
{
    std::set<std::string> chainNames;
    for(const ResolvedChain &c : chains)
        if(c.valid)
            chainNames.insert(c.name);
    if(chainNames.empty())
        return;

    auto landingOf = [&](const std::string &name) -> std::string {
        for(const ResolvedChain &c : chains)
            if(c.valid && c.name == name)
                return c.landingIsGroupRef ? c.landingGroup : c.landingNodes[0].tag;
        return std::string();
    };

    string_array items;
    ini.set_current_section("filter_local");
    ini.get_all("{NONAME}", items);

    std::vector<std::string> rebuilt;
    for(std::string line : items)
    {
        string_array parts = split(line, ",");
        int policyIdx = -1;
        for(size_t i = 0; i < parts.size(); i++)
        {
            if(chainNames.count(trim(parts[i])))
            {
                policyIdx = static_cast<int>(i);
                break;
            }
        }
        if(policyIdx >= 0)
        {
            parts[policyIdx] = " " + landingOf(trim(parts[policyIdx]));
            line = join(parts, ",") + ", via-interface=%TUN%";
        }
        rebuilt.push_back(line);
    }

    ini.erase_section();
    // backhaul rules first (first-match priority: connections to the landing must hit the front),
    // then the (rewritten) ruleset rules.
    for(const std::string &r : quanXBackhaulRules(chains))
        ini.set("{NONAME}", r);
    for(const std::string &l : rebuilt)
        ini.set("{NONAME}", l);
}
