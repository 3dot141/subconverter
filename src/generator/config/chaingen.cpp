#include "chaingen.h"

#include <set>

#include "utils/network.h"
#include "utils/regexp.h"
#include "utils/logger.h"
#include "utils/string.h"

std::vector<ResolvedChain> resolveChains(const ChainConfigs &chains, std::vector<Proxy> &nodes)
{
    std::vector<ResolvedChain> out;
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
        }
        else
        {
            rc.frontIsRef = false;
            rc.frontFilter = c.Front;
            rc.frontGroup = c.Name + "-front";
        }

        // landing: resolve to exactly one node by exact remark, else by regex
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

        rc.landingTag = matched[0]->Remark;
        rc.landingServer = matched[0]->Hostname;
        rc.landingIsIP = isIPv4(rc.landingServer) || isIPv6(rc.landingServer);
        rc.valid = true;
        out.emplace_back(std::move(rc));
    }
    return out;
}

void appendClashChains(const std::vector<ResolvedChain> &chains, ProxyGroupConfigs &groups)
{
    for(const ResolvedChain &c : chains)
    {
        if(!c.valid)
            continue;
        if(!c.frontIsRef)
        {
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
        ProxyGroupConfig relay;
        relay.Name = c.name;
        relay.Type = ProxyGroupType::Relay;
        relay.Proxies.push_back("[]" + c.frontGroup);  // group reference (front hop)
        relay.Proxies.push_back("[]" + c.landingTag);  // exact node by literal (landing)
        groups.push_back(relay);
    }
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
        std::string key = c.landingServer + "|" + c.frontGroup;
        if(seen.count(key))
            continue;
        seen.insert(key);
        if(isIPv4(c.landingServer))
            out.push_back("ip-cidr, " + c.landingServer + "/32, " + c.frontGroup);
        else if(isIPv6(c.landingServer))
            out.push_back("ip6-cidr, " + c.landingServer + "/128, " + c.frontGroup);
        else
            out.push_back("host, " + c.landingServer + ", " + c.frontGroup);
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
                return c.landingTag;
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
