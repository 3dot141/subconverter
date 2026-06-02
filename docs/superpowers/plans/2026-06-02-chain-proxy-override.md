# Chain Proxy Override Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `chain` override section so users define named relay chains (front airport group → own VPS landing) once and have subconverter emit working chained-proxy config for both Clash (`relay` group) and Quantumult X (`via-interface=%TUN%` + ip-cidr/host backhaul).

**Architecture:** A new `ChainConfig` flows through `ExternalConfig` (override container) → `extra_settings.chains` (emitter carrier). A shared `resolveChains()` resolves the landing node reference to a concrete node + server. The Clash emitter synthesizes `relay` proxy-groups; the QuanX emitter synthesizes a front policy group, a backhaul rule, and rewrites chain-targeted filter rules to append `via-interface=%TUN%`. Domain routing reuses the existing `ruleset=` mechanism (policy name = chain name).

**Tech Stack:** C++17, CMake, rapidjson/yaml-cpp/toml11, custom INIReader. No existing test framework → golden-file integration test via shell.

**Spec:** `docs/superpowers/specs/2026-06-02-chain-proxy-override-design.md`

---

## File Structure

- Create `src/config/chain.h` — `ChainConfig` / `ChainConfigs` structs.
- Modify `src/handler/settings.h` — `ExternalConfig.chains`.
- Modify `src/generator/config/subexport.h` — `extra_settings.chains`; declare `resolveChains` + `applyQuanXChains`.
- Modify `src/config/binding.h` — INI + TOML binders for `ChainConfig`.
- Modify `src/handler/settings.cpp` — read `chain` in INI/TOML/YAML loaders.
- Modify `src/handler/interfaces.cpp` — `ext.chains = extconf.chains;`.
- Create `src/generator/config/chaingen.cpp` + `.h` — `resolveChains`, Clash group synthesis, QuanX synthesis/rewrite.
- Modify `src/generator/config/subexport.cpp` — call chain synthesis in `proxyToClash` / `proxyToQuanX`; add `Relay` case to QuanX policy switch.
- Modify `CMakeLists.txt` — add `chaingen.cpp` to sources.
- Create `test/chain/` — golden fixture + assert script.
- Create `base/config/example_external_config_chain.ini` — documented sample.

---

## Task 1: ChainConfig data model + carriers

**Files:**
- Create: `src/config/chain.h`
- Modify: `src/handler/settings.h:88` (add field to `ExternalConfig`)
- Modify: `src/generator/config/subexport.h:18` (add field to `extra_settings`)

- [ ] **Step 1: Create `src/config/chain.h`**

```cpp
#ifndef CHAIN_H_INCLUDED
#define CHAIN_H_INCLUDED

#include "utils/string.h"

struct ChainConfig
{
    String Name;       // chain name == derived group name == ruleset policy name
    String Front;      // "[]GroupName" reference, or node-remark regex
    String Landing;    // node tag or regex; must resolve to exactly one node
    String FrontType;  // optional helper-group type when Front is a regex; default "select"
};
using ChainConfigs = std::vector<ChainConfig>;

#endif // CHAIN_H_INCLUDED
```

- [ ] **Step 2: Add include + field to `ExternalConfig`**

In `src/handler/settings.h`, add `#include "config/chain.h"` near the other config includes (after line 8 `#include "config/proxygroup.h"`), and inside `struct ExternalConfig` (after line 78 `ProxyGroupConfigs custom_proxy_group;`) add:

```cpp
    ChainConfigs chains;
```

- [ ] **Step 3: Add field to `extra_settings`**

In `src/generator/config/subexport.h`, add `#include "config/chain.h"` after line 10 (`#include "config/proxygroup.h"`), and inside `struct extra_settings` (after line 44 `bool authorized = false;`) add:

```cpp
    ChainConfigs chains;
```

- [ ] **Step 4: Build to verify headers compile**

Run: `cmake --build build -j4 2>&1 | tail -20` (after Task 7 sets up `build/`; if not yet configured, defer this verify to Task 2's build)
Expected: no new errors referencing `chain.h` / `ChainConfig`.

- [ ] **Step 5: Commit**

```bash
git add src/config/chain.h src/handler/settings.h src/generator/config/subexport.h
git commit -m "feat(chain): add ChainConfig model and carriers in ExternalConfig/extra_settings"
```

---

## Task 2: Parse `chain` from INI / TOML / YAML

**Files:**
- Modify: `src/config/binding.h` (add INI + TOML binders, after the `ProxyGroupConfig` binders)
- Modify: `src/handler/settings.cpp` (INI loader ~1224-1288; `loadExternalTOML` ~1147; `loadExternalYAML` ~1073)

- [ ] **Step 1: Add `#include "chain.h"` to binding.h**

In `src/config/binding.h` after line 8 (`#include "proxygroup.h"`):

```cpp
#include "chain.h"
```

- [ ] **Step 2: Add TOML binder for ChainConfig**

In `src/config/binding.h`, inside `namespace toml`, after the `from<ProxyGroupConfig>` specialization (after line 89):

```cpp
    template<>
    struct from<ChainConfig>
    {
        static ChainConfig from_toml(const value& v)
        {
            ChainConfig conf;
            conf.Name = find<String>(v, "name");
            conf.Front = find<String>(v, "front");
            conf.Landing = find<String>(v, "landing");
            conf.FrontType = find_or<String>(v, "front_type", "select");
            return conf;
        }
    };
```

- [ ] **Step 3: Add INI binder for ChainConfig**

In `src/config/binding.h`, inside `namespace INIBinding`, after the `from<ProxyGroupConfig>` specialization (after line 266):

```cpp
    template<>
    struct from<ChainConfig>
    {
        static ChainConfigs from_ini(const StrArray &arr)
        {
            // format: name`front`landing[`front_type]
            ChainConfigs confs;
            for(const String &x : arr)
            {
                StrArray vArray = split(x, "`");
                if(vArray.size() < 3)
                    continue;
                ChainConfig conf;
                conf.Name = vArray[0];
                conf.Front = vArray[1];
                conf.Landing = vArray[2];
                conf.FrontType = vArray.size() > 3 ? vArray[3] : "select";
                confs.emplace_back(std::move(conf));
            }
            return confs;
        }
    };
```

- [ ] **Step 4: Read `chain` in the INI external-config loader**

In `src/handler/settings.cpp`, locate the INI branch of `loadExternalConfig` where `custom_proxy_group` is read via `INIBinding::from<ProxyGroupConfig>::from_ini` (~line 1235). Immediately after the proxy-group block, add:

```cpp
    if(ini.item_prefix_exist("chain"))
    {
        string_array vArray;
        ini.get_all("chain", vArray);
        ext.chains = INIBinding::from<ChainConfig>::from_ini(vArray);
    }
```

> Match the exact INIReader API already used nearby for `custom_proxy_group` (e.g. `get_all`/`get_bool`); mirror that call shape rather than the illustrative names above.

- [ ] **Step 5: Read `[[chain]]` in `loadExternalTOML`**

In `loadExternalTOML` (~line 1147), where `custom_proxy_group` / `custom_groups` are parsed, add:

```cpp
    ext.chains = toml::find_or<ChainConfigs>(root, "chain", {});
```

> Use the same `toml::find`/`find_or` + root-value variable already in scope in that function.

- [ ] **Step 6: Read `chain:` in `loadExternalYAML`**

In `loadExternalYAML` (~line 1073), after the proxy-group parsing block, add:

```cpp
    if(node["chain"].IsDefined())
    {
        for(size_t i = 0; i < node["chain"].size(); i++)
        {
            ChainConfig conf;
            node["chain"][i]["name"] >>= conf.Name;
            node["chain"][i]["front"] >>= conf.Front;
            node["chain"][i]["landing"] >>= conf.Landing;
            if(node["chain"][i]["front_type"].IsDefined())
                node["chain"][i]["front_type"] >>= conf.FrontType;
            else
                conf.FrontType = "select";
            ext.chains.emplace_back(std::move(conf));
        }
    }
```

> Use the `operator>>=` helper from `utils/yamlcpp_extra.h` already used by the surrounding YAML parsing; if that file uses a different accessor pattern, mirror it.

- [ ] **Step 7: Build**

Run: `cmake --build build -j4 2>&1 | tail -20`
Expected: compiles clean.

- [ ] **Step 8: Commit**

```bash
git add src/config/binding.h src/handler/settings.cpp
git commit -m "feat(chain): parse chain section from INI/TOML/YAML external config"
```

---

## Task 3: `resolveChains` helper

**Files:**
- Create: `src/generator/config/chaingen.h`
- Create: `src/generator/config/chaingen.cpp`
- Modify: `CMakeLists.txt` (add `src/generator/config/chaingen.cpp`)

- [ ] **Step 1: Create `chaingen.h`**

```cpp
#ifndef CHAINGEN_H_INCLUDED
#define CHAINGEN_H_INCLUDED

#include <string>
#include <vector>

#include "config/chain.h"
#include "parser/config/proxy.h"
#include "utils/ini_reader/ini_reader.h"

struct ResolvedChain
{
    std::string name;
    bool        frontIsRef = false;  // Front was "[]Group"
    std::string frontGroup;          // referenced group, or generated "<name>-front"
    std::string frontFilter;         // node regex when !frontIsRef
    std::string frontType;           // helper group type
    std::string landingTag;          // resolved unique landing node remark
    std::string landingServer;       // landing node hostname (IP or domain)
    bool        landingIsIP = false;
    bool        valid = false;
};

// Resolve each chain against the node pool. Invalid chains (landing not unique) are returned with valid=false and logged.
std::vector<ResolvedChain> resolveChains(const ChainConfigs &chains, const std::vector<Proxy> &nodes);

#endif // CHAINGEN_H_INCLUDED
```

- [ ] **Step 2: Create `chaingen.cpp` with `resolveChains`**

```cpp
#include "chaingen.h"

#include "utils/network.h"
#include "utils/regexp.h"
#include "utils/logger.h"
#include "utils/string.h"

std::vector<ResolvedChain> resolveChains(const ChainConfigs &chains, const std::vector<Proxy> &nodes)
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
        std::vector<const Proxy*> matched;
        for(const Proxy &n : nodes)
            if(n.Remark == c.Landing)
                matched.push_back(&n);
        if(matched.empty())
            for(const Proxy &n : nodes)
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
```

> `regFind`, `startsWith`, `writeLog`, `isIPv4/isIPv6` are existing utilities (`utils/regexp.h`, `utils/string.h`, `utils/logger.h`, `utils/network.h`). Confirm `regFind` signature (`bool regFind(const std::string&, const std::string&)`) and `LOG_LEVEL_WARNING` constant; adjust to the actual names if they differ.

- [ ] **Step 3: Add to CMake**

In `CMakeLists.txt`, find the source list containing `src/generator/config/subexport.cpp` and add on a new line:

```cmake
    src/generator/config/chaingen.cpp
```

- [ ] **Step 4: Build**

Run: `cmake --build build -j4 2>&1 | tail -20`
Expected: `chaingen.cpp` compiles and links.

- [ ] **Step 5: Commit**

```bash
git add src/generator/config/chaingen.h src/generator/config/chaingen.cpp CMakeLists.txt
git commit -m "feat(chain): add resolveChains to resolve landing node and front type"
```

---

## Task 4: Clash chain synthesis

**Files:**
- Modify: `src/generator/config/chaingen.h` / `.cpp` (add `appendClashChains`)
- Modify: `src/generator/config/subexport.cpp` (call inside `proxyToClash`, after the proxy-group loop ~line 795)

- [ ] **Step 1: Declare `appendClashChains` in `chaingen.h`**

Add the include and declaration:

```cpp
#include "config/proxygroup.h"

// Append a "<name>-front" helper group (when front is a regex) and a relay "<name>" group per valid chain.
void appendClashChains(const std::vector<ResolvedChain> &chains, ProxyGroupConfigs &groups);
```

- [ ] **Step 2: Implement `appendClashChains` in `chaingen.cpp`**

```cpp
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
        relay.Proxies.push_back(c.frontGroup);
        relay.Proxies.push_back("[]" + c.landingTag); // exact node by literal
        groups.push_back(relay);
    }
}
```

> The proxy-group emitter expands members via `groupGenerate`, which treats `[]literal` as an exact remark. Verify this matches `groupGenerate` semantics in `subexport.cpp` (helper ~line 191); if exact-match uses a different prefix, use that.

- [ ] **Step 3: Call from `proxyToClash`**

In `src/generator/config/subexport.cpp`, the YAML `proxyToClash` overload builds groups into a local `ProxyGroupConfigs`-like loop over `extra_proxy_group`. Since `extra_proxy_group` is `const`, instead build a merged list at the top of the function. Find where the group loop begins (`for (const ProxyGroupConfig &x: extra_proxy_group)`, line 733) and replace the loop subject with a local copy augmented by chains. Just before line 733 add:

```cpp
    ProxyGroupConfigs merged_groups = extra_proxy_group;
    appendClashChains(resolveChains(ext.chains, nodes), merged_groups);
```

and change the loop header at line 733 from `extra_proxy_group` to `merged_groups`:

```cpp
    for (const ProxyGroupConfig &x: merged_groups) {
```

Add `#include "chaingen.h"` to the top of `subexport.cpp` if not present.

- [ ] **Step 4: Build**

Run: `cmake --build build -j4 2>&1 | tail -20`
Expected: compiles.

- [ ] **Step 5: Manual smoke (deferred assertion to Task 7)**

Note: full assertion happens in Task 7's golden test. For now confirm build only.

- [ ] **Step 6: Commit**

```bash
git add src/generator/config/chaingen.h src/generator/config/chaingen.cpp src/generator/config/subexport.cpp
git commit -m "feat(chain): emit Clash relay group + front helper per chain"
```

---

## Task 5: QuanX chain synthesis + relay-case fix

**Files:**
- Modify: `src/generator/config/chaingen.h` / `.cpp` (add `applyQuanXChains`)
- Modify: `src/generator/config/subexport.cpp` (`proxyToQuanX`: add front policy + Relay case + call backhaul/rewrite around line 1976-1979)

- [ ] **Step 1: Add `Relay` case to the QuanX policy switch**

In `src/generator/config/subexport.cpp`, in `proxyToQuanX`'s policy switch (lines 1917-1937), add before `default:` (line 1935):

```cpp
            case ProxyGroupType::Relay:
                type = "static";
                break;
```

This stops hand-written relay groups from being silently dropped (degrades to a static list in chain order).

- [ ] **Step 2: Declare `appendQuanXFrontGroups` + `applyQuanXChains` in `chaingen.h`**

```cpp
#include <string>

// Returns "static=<name>-front, server-tag-regex=<filter>, ..." lines for chains whose front is a regex.
std::vector<std::string> quanXFrontPolicies(const std::vector<ResolvedChain> &chains);

// Returns backhaul filter lines: "ip-cidr, <ip>/32, <front>" or "host, <domain>, <front>" (deduped by landing+front).
std::vector<std::string> quanXBackhaulRules(const std::vector<ResolvedChain> &chains);

// Rewrite filter_local lines whose policy == a chain name into
// "<type>, <pattern>, <landingTag>, via-interface=%TUN%".
void rewriteQuanXChainRules(INIReader &ini, const std::vector<ResolvedChain> &chains);
```

- [ ] **Step 2b: Implement in `chaingen.cpp`**

```cpp
#include <set>

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
        if(c.landingIsIP)
            out.push_back("ip-cidr, " + c.landingServer + "/32, " + c.frontGroup);
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
    auto landingOf = [&](const std::string &name) {
        for(const ResolvedChain &c : chains)
            if(c.valid && c.name == name)
                return c.landingTag;
        return std::string();
    };

    string_array items;
    ini.set_current_section("filter_local");
    ini.get_all("{NONAME}", items); // mirror the actual accessor used to read filter lines
    // Rebuild: replace policy field on chain-targeted rules.
    std::vector<std::string> rebuilt;
    for(std::string line : items)
    {
        string_array parts = split(line, ",");
        if(parts.size() >= 3)
        {
            std::string policy = trim(parts[parts.size() - 1]);
            if(chainNames.count(policy))
            {
                parts[parts.size() - 1] = " " + landingOf(policy);
                line = join(parts, ",") + ", via-interface=%TUN%";
            }
        }
        rebuilt.push_back(line);
    }
    ini.erase_section();
    for(const std::string &l : rebuilt)
        ini.set("{NONAME}", l);
}
```

> The exact INIReader read/erase/set API for `filter_local` must match how `rulesetToSurge` writes it. Inspect `rulesetToSurge` (mode `-1`) and the INIReader methods (`get_all`, `get_items`, `set`, `erase_section`) and align these three functions to that real API. The logic (split on comma, last field = policy, append via-interface) stays the same.

- [ ] **Step 3: Wire into `proxyToQuanX`**

In `subexport.cpp`, `proxyToQuanX`, resolve chains once after the policy loop and before `rulesetToSurge` (line 1978). Insert at line 1976-1977:

```cpp
    auto resolved_chains = resolveChains(ext.chains, nodes);
    for(const std::string &p : quanXFrontPolicies(resolved_chains))
        ini.set("{NONAME}", p); // into current "policy" section
```

Then after `rulesetToSurge(...)` (line 1979), add:

```cpp
    if (ext.enable_rule_generator && !resolved_chains.empty())
    {
        ini.set_current_section("filter_local");
        for(const std::string &r : quanXBackhaulRules(resolved_chains))
            ini.set("{NONAME}", r);
        rewriteQuanXChainRules(ini, resolved_chains);
    }
```

> Confirm the section the policy lines belong to (the function uses `ini.set("{NONAME}", ...)` after `set_current_section("policy")` earlier). Ensure front-policy lines land in `policy` and backhaul lines in `filter_local`.

- [ ] **Step 4: Build**

Run: `cmake --build build -j4 2>&1 | tail -20`
Expected: compiles.

- [ ] **Step 5: Commit**

```bash
git add src/generator/config/chaingen.h src/generator/config/chaingen.cpp src/generator/config/subexport.cpp
git commit -m "feat(chain): emit QuanX front policy + backhaul + via-interface rewrite; fix relay drop"
```

---

## Task 6: Thread chains from ExternalConfig into ext

**Files:**
- Modify: `src/handler/interfaces.cpp` (~line 540, where `lCustomProxyGroups = extconf.custom_proxy_group;`)

- [ ] **Step 1: Assign chains when external config is present**

In `src/handler/interfaces.cpp`, find the block (~line 538-546) that sets `lCustomProxyGroups = extconf.custom_proxy_group;`. Immediately after it add:

```cpp
                    ext.chains = extconf.chains;
```

(match the surrounding indentation/scope exactly — same `if` branch that assigns the proxy groups).

- [ ] **Step 2: Build**

Run: `cmake --build build -j4 2>&1 | tail -20`
Expected: compiles.

- [ ] **Step 3: Commit**

```bash
git add src/handler/interfaces.cpp
git commit -m "feat(chain): thread ExternalConfig.chains into extra_settings"
```

---

## Task 7: Golden integration test + build setup + sample config

**Files:**
- Create: `test/chain/nodes.txt` (node share-links incl. a VPS landing)
- Create: `test/chain/external_chain.ini` (chain + ruleset)
- Create: `test/chain/run.sh` (build if needed, run conversion, assert)
- Create: `base/config/example_external_config_chain.ini`

- [ ] **Step 1: Configure + build the binary (first-time)**

Run:
```bash
cd /Users/yes365/AI/subconverter-explore
cmake -B build -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -15
cmake --build build -j4 2>&1 | tail -20
ls -la build/subconverter 2>&1
```
Expected: `build/subconverter` exists. If dependencies are missing, resolve via the repo's documented build (see `scripts/` / `CMakeLists.txt`) before continuing.

- [ ] **Step 2: Create fixture `test/chain/external_chain.ini`**

```ini
[custom]
chain=JP-Chain`(JP|日本)`my-jp-vps
ruleset=JP-Chain,[]DOMAIN-SUFFIX,google.com
ruleset=DIRECT,[]GEOIP,CN
ruleset=JP-Chain,[]FINAL
```

> The `[custom]` section name and key style must match what `loadExternalConfig` expects for an INI external config (mirror `base/config/example_external_config.ini`). Adjust section header if that example uses a different one.

- [ ] **Step 3: Create fixture `test/chain/nodes.txt`**

Two share-links: one ordinary JP airport node named `日本01`, one VPS landing named `my-jp-vps` with a literal IP host. Use valid SS links:

```
ss://YWVzLTI1Ni1nY206cGFzcw==@198.51.100.7:8388#日本01
ss://YWVzLTI1Ni1nY206cGFzcw==@203.0.113.9:8388#my-jp-vps
```

- [ ] **Step 4: Create `test/chain/run.sh`**

```bash
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="$ROOT/build/subconverter"
NODES="$(paste -sd'|' "$HERE/nodes.txt")"

# Clash
CLASH=$("$BIN" -g --artifact "" 2>/dev/null || true)  # placeholder; real invocation below
# NOTE: subconverter is normally driven via HTTP. Use generate mode or curl against a started instance.
```

> subconverter has no simple one-shot CLI for `target=clash` with inline config; it runs as an HTTP server. The real test must: start `build/subconverter`, then `curl "http://127.0.0.1:25500/sub?target=clash&url=<urlencoded nodes>&config=<file://test/chain/external_chain.ini>"`, capture output, assert. Implement `run.sh` to (1) start the server in background on a temp port, (2) curl both `target=clash` and `target=quanx`, (3) assert, (4) kill server. Reference `base/pref.example.ini` for enabling local file config (`config=` may require `base_path`/allowlist). Confirm against `interfaces.cpp` arg handling for `config=` (line 393 `getUrlArg("config")`).

- [ ] **Step 5: Implement real `run.sh` assertions**

```bash
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="$ROOT/build/subconverter"
PORT=25599
NODES=$(python3 -c "import urllib.parse,sys;print(urllib.parse.quote(open('$HERE/nodes.txt').read().strip().replace(chr(10),'|')))")
CFG=$(python3 -c "import urllib.parse;print(urllib.parse.quote('$HERE/external_chain.ini'))")

cd "$ROOT"
"$BIN" >/tmp/subconv.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1

CLASH=$(curl -s "http://127.0.0.1:25500/sub?target=clash&url=$NODES&config=$CFG")
QUANX=$(curl -s "http://127.0.0.1:25500/sub?target=quanx&url=$NODES&config=$CFG")

fail=0
echo "$CLASH" | grep -q "type: relay" || { echo "FAIL: clash relay group missing"; fail=1; }
echo "$CLASH" | grep -q "my-jp-vps" || { echo "FAIL: clash landing missing"; fail=1; }
echo "$QUANX" | grep -q "ip-cidr, 203.0.113.9/32" || { echo "FAIL: quanx backhaul missing"; fail=1; }
echo "$QUANX" | grep -q "via-interface=%TUN%" || { echo "FAIL: quanx via-interface missing"; fail=1; }
[ $fail -eq 0 ] && echo "PASS" || exit 1
```

> Port/host must match the server's configured `pref.ini` listen settings (default `127.0.0.1:25500`). Enabling local-file `config=` may need `base_path` allowlisting in `pref.ini`; if blocked, host the fixture via a `file://` form the server permits, or place fixtures under the allowed base dir. Adjust until both curls return non-empty config.

- [ ] **Step 6: Run the golden test**

Run: `bash test/chain/run.sh`
Expected: `PASS`

- [ ] **Step 7: Create documented sample `base/config/example_external_config_chain.ini`**

```ini
; Chain proxy override example.
; Define named chains (front airport group -> your own VPS landing); route domains via ruleset.
[custom]
; chain = name ` front ` landing [ ` front_type ]
;   front:   "[]GroupName" to reference an existing policy group, OR a node-remark regex
;   landing: node tag or regex; MUST resolve to exactly one node (your VPS)
chain=JP-Chain`(JP|日本)`my-jp-vps
chain=US-Chain`[]🇺🇸 US`my-us-vps`url-test

; route domains to a chain (policy name == chain name) using the normal ruleset mechanism
ruleset=JP-Chain,[]DOMAIN-SUFFIX,google.com
ruleset=US-Chain,https://raw.githubusercontent.com/.../US.list
```

- [ ] **Step 8: Commit**

```bash
git add test/chain base/config/example_external_config_chain.ini
git commit -m "test(chain): golden integration test + documented sample config"
```

---

## Task 8: Docs + final verification

**Files:**
- Modify: `README.md` (or relevant docs) — short "Chain proxy override" section.

- [ ] **Step 1: Add README section**

Add a concise section documenting the `chain` key (INI/TOML/YAML forms), the front/landing/front_type semantics, the QuanX-requires-IP-or-host note, and the "landing must resolve to one node" rule. Link the sample config and spec.

- [ ] **Step 2: Full build + test**

Run:
```bash
cmake --build build -j4 2>&1 | tail -5
bash test/chain/run.sh
```
Expected: build clean; `PASS`.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs(chain): document chain proxy override usage"
```

---

## Self-Review Notes

- **Spec coverage:** §3 schema → Task 2; §4 model → Task 1; §5.1 resolve → Task 3; §5.2 Clash → Task 4; §5.3 QuanX (front/backhaul/rewrite/relay-fix) → Task 5; §6 threading → Task 6; §7 testing → Task 7; §8 edge cases → handled in Task 3 (landing non-unique skip+warn), Task 5 (dedup, IP-vs-host), Task 4/2 (name override via existing replace). All covered.
- **Carrier:** `extra_settings.chains` avoids changing 8 emitter signatures; only Clash/QuanX read it.
- **Known adaptation points (flagged inline with `>`):** exact INIReader accessor names, `groupGenerate` literal-prefix semantics, `regFind`/`writeLog` signatures, the `config=` local-file allowlist for the test server. These are real-API confirmations to make during execution; logic is fixed.
