# QuanX anytls 输出 + chain 多节点落地 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 subconverter 的 QuanX 端正确输出 anytls 节点,并让 chain 的 landing 支持引用一个 policy 组(一组 VPS),Clash 用 dialer-proxy、QuanX 用多条 backhaul 实现链式。

**Architecture:** 协议层在 `proxyToQuanX` 加 anytls 分支;链式层把 `resolveChains` 的 landing 从单节点升级为节点集(`landingNodes[]`,支持 `[]组名` 引用),Clash 端把落地注入提前到节点循环前写 `UnderlyingProxy`→输出 `dialer-proxy`(relay 退役),QuanX 端对每个落地 server 生成 backhaul。

**Tech Stack:** C++17,CMake 构建,golden 集成测试(`test/chain/run.sh`,curl + grep 断言),无单元测试框架——TDD 形态为"改 golden 断言/fixture → 编译 → 跑 run.sh 看失败 → 改代码 → 编译 → 跑通过 → commit"。

**设计依据:** `docs/superpowers/specs/3dot141/260603-quanx-anytls-chain-landing-design.md`(BF1-BF4、方案选型 Q1-Q6)。

---

## File Structure

| 文件 | 职责 | 本计划改动 |
|---|---|---|
| `src/generator/config/subexport.cpp` | 各 target emitter | proxyToQuanX anytls 分支(已写入);proxyToClash dialer-proxy 输出 + 注入提前;chain 调用点改参 |
| `src/generator/config/chaingen.h` | chain 数据结构 + 函数声明 | ResolvedChain 改 landingNodes;resolveChains/appendClashChains 签名 |
| `src/generator/config/chaingen.cpp` | chain 解析与两端生成 | 组引用解析、注入拆分、backhaul 多节点 |
| `test/chain/run.sh` | golden 断言 | relay→dialer-proxy 断言;新增 anytls + 多节点 landing 断言 |
| `test/chain/external_chain.ini` | golden fixture(chain 配置) | 加 usa-landing 组引用 chain |
| `test/chain/nodes.txt` | golden fixture(节点) | 加 anytls 节点 + 一组 LD-US 落地节点 |

> `src/parser/config/proxy.h`、`src/handler/interfaces.cpp` **不改**(复用 `UnderlyingProxy:145`;interfaces 已传 lCustomProxyGroups)。

---

## Task 0: 建立编译 + golden baseline

**Files:**
- Build: `build/`(cmake 产物)
- Test: `test/chain/run.sh`

- [ ] **Step 1: 配置并编译**

Run:
```bash
cd /Users/yes365/AI/subconverter-explore-feature_quanx-anytls-chain
mkdir -p build && cd build && cmake .. && make -j
```
Expected: 编译成功,产出 `build/subconverter`。若 cmake 缺失先 `brew install cmake`。**这是 anytls emitter 改动(已写入工作区)的首次编译验证**——若 `subexport.cpp:1888-1916` 的 anytls case 有语法错,这里会暴露,修正后再继续。

- [ ] **Step 2: 跑现有 golden 确认起点干净**

Run:
```bash
cd /Users/yes365/AI/subconverter-explore-feature_quanx-anytls-chain
bash test/chain/run.sh build/subconverter
```
Expected: `PASS`(现有 5 条断言:relay group / my-jp-vps / JP-Chain-front / ip-cidr,203.0.113.9/32 / via-interface)。确认改动前基线通过。

- [ ] **Step 3: Commit baseline(若编译触发了 anytls 语法修正)**

```bash
git add -A && git commit -m "build(chain): verify anytls emitter compiles; golden baseline green" || echo "无改动跳过"
```

---

## Task 1: anytls 节点输出为 QuanX 行(BF1 验证)

代码已写入 `subexport.cpp:1888-1902`,本任务补 golden 断言锁定行为。

**Files:**
- Modify: `test/chain/nodes.txt`(加 anytls 节点)
- Modify: `test/chain/run.sh`(加 anytls 断言)

- [ ] **Step 1: 加 anytls 节点到 fixture**

在 `test/chain/nodes.txt` 末尾追加一行(anytls share-link,带 SNI):
```
anytls://8b7b01267b6440aa@198.51.100.50:4048?sni=cdn.example.com#anytls-hk-01
```

- [ ] **Step 2: 加 anytls 断言到 run.sh**

在 `test/chain/run.sh:37`(via-interface 断言后)插入:
```bash
check "quanx anytls line"     "$QUANX" "anytls = 198.51.100.50:4048"
check "quanx anytls over-tls" "$QUANX" "over-tls=true"
check "quanx anytls tls-host" "$QUANX" "tls-host=cdn.example.com"
```

- [ ] **Step 3: 编译并跑 golden 看 anytls 断言通过**

Run:
```bash
cd build && make -j && cd .. && bash test/chain/run.sh build/subconverter
```
Expected: `PASS`,新增 3 条 anytls 断言 ok。若 FAIL 说明 anytls 解析/输出有问题,对照 `subexport.cpp:1888-1902` 与 `subparser.cpp` anyTlSConstruct 排查。

- [ ] **Step 4: Commit**

```bash
git add test/chain/nodes.txt test/chain/run.sh
git commit -m "test(chain): assert QuanX anytls line output (BF1)"
```

---

## Task 2: ResolvedChain 改 landingNodes + 组引用解析(BF2)

**Files:**
- Modify: `src/generator/config/chaingen.h`
- Modify: `src/generator/config/chaingen.cpp`
- Modify: `src/generator/config/subexport.cpp`(调用点 :734、:2000 改传 proxyGroups)

- [ ] **Step 1: 改 ResolvedChain 结构(chaingen.h)**

把 `chaingen.h` 的 `ResolvedChain` 里 `landingTag`/`landingServer`/`landingIsIP` 三行替换为:
```cpp
    bool        landingIsGroupRef = false;   // landing 写法是 "[]组名"
    std::string landingGroup;                // 组名(QuanX policy 位用)
    struct LandingNode { std::string tag; std::string server; bool isIP; };
    std::vector<LandingNode> landingNodes;   // 落地节点集(单节点时 size=1)
```
并改 `resolveChains` 声明加 proxyGroups 参数:
```cpp
std::vector<ResolvedChain> resolveChains(const ChainConfigs &chains,
                                         std::vector<Proxy> &nodes,
                                         const ProxyGroupConfigs &proxyGroups);
```

- [ ] **Step 2: 重写 resolveChains 的 landing 解析(chaingen.cpp)**

把 `chaingen.cpp` 现有 landing 解析段(原 32-55 行,`// landing: resolve to exactly one node` 起到 `rc.valid=true` 止)替换为:
```cpp
        // landing: "[]Group" 引用 vs 单节点
        if (startsWith(c.Landing, "[]")) {
            rc.landingIsGroupRef = true;
            rc.landingGroup = c.Landing.substr(2);
            // 在 custom_proxy_group 定位该组
            const ProxyGroupConfig *grp = nullptr;
            for (const ProxyGroupConfig &g : proxyGroups)
                if (g.Name == rc.landingGroup) { grp = &g; break; }
            if (!grp) {
                writeLog(0, "Chain '" + c.Name + "' landing group '" + rc.landingGroup + "' not found; skipping.", LOG_LEVEL_WARNING);
                rc.valid = false; out.emplace_back(std::move(rc)); continue;
            }
            // 只支持"普通 remark 正则筛选型":拒绝含 []/script:/!! 条目的组
            bool bad = false;
            for (const std::string &rule : grp->Proxies)
                if (startsWith(rule, "[]") || startsWith(rule, "script:") || startsWith(rule, "!!")) { bad = true; break; }
            if (bad) {
                writeLog(0, "Chain '" + c.Name + "' landing group '" + rc.landingGroup + "' not regex-filtered; skipping.", LOG_LEVEL_WARNING);
                rc.valid = false; out.emplace_back(std::move(rc)); continue;
            }
            // 用组的每条正则并集匹配节点池
            for (Proxy &n : nodes)
                for (const std::string &rule : grp->Proxies)
                    if (regFind(n.Remark, rule)) {
                        rc.landingNodes.push_back({n.Remark, n.Hostname, isIPv4(n.Hostname) || isIPv6(n.Hostname)});
                        break;
                    }
        } else {
            // 单节点 landing(旧用法):精确 remark 优先,否则正则,要求唯一
            std::vector<Proxy*> matched;
            for (Proxy &n : nodes) if (n.Remark == c.Landing) matched.push_back(&n);
            if (matched.empty())
                for (Proxy &n : nodes) if (regFind(n.Remark, c.Landing)) matched.push_back(&n);
            if (matched.size() != 1) {
                writeLog(0, "Chain '" + c.Name + "' landing '" + c.Landing + "' resolved to "
                            + std::to_string(matched.size()) + " nodes; skipping.", LOG_LEVEL_WARNING);
                rc.valid = false; out.emplace_back(std::move(rc)); continue;
            }
            rc.landingNodes.push_back({matched[0]->Remark, matched[0]->Hostname, isIPv4(matched[0]->Hostname) || isIPv6(matched[0]->Hostname)});
        }
        if (rc.landingNodes.empty()) {
            writeLog(0, "Chain '" + c.Name + "' landing has no matching node; skipping.", LOG_LEVEL_WARNING);
            rc.valid = false; out.emplace_back(std::move(rc)); continue;
        }
        rc.valid = true;
```
改函数签名同步加 `const ProxyGroupConfigs &proxyGroups`。

- [ ] **Step 3: 适配 backhaul 用 landingNodes(chaingen.cpp quanXBackhaulRules)**

把 `quanXBackhaulRules` 内单 server 逻辑改为遍历 `landingNodes`(原按 `c.landingServer` 单条 → 改双层循环):
```cpp
    for (const ResolvedChain &c : chains) {
        if (!c.valid) continue;
        for (const auto &ln : c.landingNodes) {
            std::string key = ln.server + "|" + c.frontGroup;
            if (seen.count(key)) continue;
            seen.insert(key);
            if (isIPv4(ln.server))      out.push_back("ip-cidr, " + ln.server + "/32, " + c.frontGroup);
            else if (isIPv6(ln.server)) out.push_back("ip6-cidr, " + ln.server + "/128, " + c.frontGroup);
            else                        out.push_back("host, " + ln.server + ", " + c.frontGroup);
        }
    }
```

- [ ] **Step 4: 适配 rewriteQuanXChainRules 的 policy 改写(chaingen.cpp)**

`landingOf` lambda 改为:组引用返回 `landingGroup`,否则返回 `landingNodes[0].tag`:
```cpp
    auto landingOf = [&](const std::string &name) -> std::string {
        for (const ResolvedChain &c : chains)
            if (c.valid && c.name == name)
                return c.landingIsGroupRef ? c.landingGroup : c.landingNodes[0].tag;
        return std::string();
    };
```

- [ ] **Step 5: 改 Clash/QuanX 两个调用点传 proxyGroups(subexport.cpp)**

`subexport.cpp:734`(Clash)当前:
```cpp
appendClashChains(resolveChains(ext.chains, nodelist), merged_groups);
```
本任务先只改 resolveChains 传参(appendClashChains 拆分在 Task 3 做),临时改为:
```cpp
appendClashChains(resolveChains(ext.chains, nodelist, extra_proxy_group), merged_groups);
```
`subexport.cpp:~2000`(QuanX)找到 `resolveChains(ext.chains, nodelist)` 调用,改为 `resolveChains(ext.chains, nodelist, extra_proxy_group)`。

- [ ] **Step 6: 编译并跑 golden(行为不变,单节点仍走 relay)**

Run:
```bash
cd build && make -j && cd .. && bash test/chain/run.sh build/subconverter
```
Expected: `PASS`(现有 fixture 是单节点 landing,landingNodes size=1,relay 仍生成,所有旧断言 + Task1 anytls 断言全过)。编译错优先修 `appendClashChains` 内对 `landingTag`/`landingServer` 的引用——它们已不存在,需改用 `landingNodes[0].tag`/`.server`(见 chaingen.cpp appendClashChains:82-83)。

- [ ] **Step 7: Commit**

```bash
git add src/generator/config/chaingen.h src/generator/config/chaingen.cpp src/generator/config/subexport.cpp
git commit -m "feat(chain): landing supports []group ref → landingNodes set (BF2)"
```

---

## Task 3: Clash dialer-proxy + relay 退役(BF3)

**Files:**
- Modify: `src/generator/config/chaingen.h`(拆分声明)
- Modify: `src/generator/config/chaingen.cpp`(injectClashChains + appendClashFrontGroups)
- Modify: `src/generator/config/subexport.cpp`(proxyToClash:注入提前 + dialer-proxy 输出)
- Modify: `test/chain/run.sh`(relay 断言 → dialer-proxy)

- [ ] **Step 1: 先改 golden 断言(TDD:期望 dialer-proxy)**

`test/chain/run.sh:33` 把:
```bash
check "clash relay group"      "$CLASH" "type: relay"
```
改为:
```bash
check "clash dialer-proxy"     "$CLASH" "dialer-proxy: JP-Chain-front"
check "clash no relay group"   "$CLASH" "type: relay" && { echo "FAIL: relay 未退役"; }  # 见下注
```
> 注:`check` 是"存在即 ok"语义,"不应存在"要反向写。把第二条改为内联反向断言:
```bash
if echo "$CLASH" | grep -q "type: relay"; then echo "FAIL: relay 仍生成"; fail=1; else echo "ok: clash no relay group"; fi
```

- [ ] **Step 2: 编译并跑 golden 看失败**

Run:
```bash
cd build && make -j && cd .. && bash test/chain/run.sh build/subconverter
```
Expected: FAIL —— `dialer-proxy: JP-Chain-front` 缺失(代码还在生成 relay)。确认红。

- [ ] **Step 3: 拆分 chaingen 声明(chaingen.h)**

把 `appendClashChains` 声明替换为两个:
```cpp
// 注入(节点循环前调):给落地节点写 UnderlyingProxy,outResolved 回传给加组步骤
void injectClashChains(const ChainConfigs &chains, std::vector<Proxy> &nodes,
                       const ProxyGroupConfigs &proxyGroups,
                       std::vector<ResolvedChain> &outResolved);
// 加 front 辅助组(节点循环后调);不再生成 relay
void appendClashFrontGroups(const std::vector<ResolvedChain> &chains, ProxyGroupConfigs &groups);
```

- [ ] **Step 4: 实现 injectClashChains + appendClashFrontGroups(chaingen.cpp)**

删掉原 `appendClashChains`,替换为:
```cpp
void injectClashChains(const ChainConfigs &chains, std::vector<Proxy> &nodes,
                       const ProxyGroupConfigs &proxyGroups,
                       std::vector<ResolvedChain> &outResolved)
{
    outResolved = resolveChains(chains, nodes, proxyGroups);
    for (const ResolvedChain &c : outResolved) {
        if (!c.valid) continue;
        for (const auto &ln : c.landingNodes) {
            for (Proxy &n : nodes)                       // 拿原 nodes 里的引用
                if (n.Remark == ln.tag) { n.UnderlyingProxy = c.frontGroup; break; }
        }
    }
}

void appendClashFrontGroups(const std::vector<ResolvedChain> &chains, ProxyGroupConfigs &groups)
{
    for (const ResolvedChain &c : chains) {
        if (!c.valid || c.frontIsRef) continue;          // front 是 []组引用则无需建辅助组
        ProxyGroupConfig front;
        front.Name = c.frontGroup;
        front.Type = c.frontType == "url-test" ? ProxyGroupType::URLTest : ProxyGroupType::Select;
        if (front.Type == ProxyGroupType::URLTest) {
            front.Url = "http://www.gstatic.com/generate_204";
            front.Interval = 300;
        }
        front.Proxies.push_back(c.frontFilter);
        groups.push_back(front);
    }
    // 不再生成 relay 组(relay 退役)
}
```

- [ ] **Step 5: proxyToClash 输出 dialer-proxy(subexport.cpp 节点循环通用字段区)**

在 `subexport.cpp` proxyToClash 节点循环内、`udp` 字段附近(:707 之后)加:
```cpp
        if (!x.UnderlyingProxy.empty())
            singleproxy["dialer-proxy"] = x.UnderlyingProxy;   // 链式落地:拨号先走 front
```

- [ ] **Step 6: proxyToClash 注入提前 + 加组改调用(subexport.cpp)**

在 proxyToClash **节点循环之前**(:255 `for (Proxy &x : nodes)` 之前)加:
```cpp
    std::vector<ResolvedChain> resolvedChains;
    injectClashChains(ext.chains, nodes, extra_proxy_group, resolvedChains);  // 写 UnderlyingProxy,循环随后输出 dialer-proxy
```
把原 :734 的:
```cpp
appendClashChains(resolveChains(ext.chains, nodelist, extra_proxy_group), merged_groups);
```
改为:
```cpp
appendClashFrontGroups(resolvedChains, merged_groups);
```

- [ ] **Step 7: 编译并跑 golden 看通过**

Run:
```bash
cd build && make -j && cd .. && bash test/chain/run.sh build/subconverter
```
Expected: `PASS` —— `dialer-proxy: JP-Chain-front` ok、`clash no relay group` ok、`my-jp-vps` 仍在、其余不变。

- [ ] **Step 8: Commit**

```bash
git add src/generator/config/chaingen.h src/generator/config/chaingen.cpp src/generator/config/subexport.cpp test/chain/run.sh
git commit -m "feat(chain): Clash uses dialer-proxy, retire relay group (BF3)"
```

---

## Task 4: 多节点 landing 端到端 golden(BF2+BF3+BF4 整合)

**Files:**
- Modify: `test/chain/nodes.txt`(加一组 LD-US 落地节点)
- Modify: `test/chain/external_chain.ini`(加 usa-landing 组 + landing=[]组 的 chain)
- Modify: `test/chain/run.sh`(加多节点断言)

- [ ] **Step 1: 加两台落地 VPS 到 nodes.txt**

末尾追加(独立 IP,LD-US 前缀):
```
ss://YWVzLTI1Ni1nY206cGFzcw==@192.0.2.11:8388#LD-US-01
ss://YWVzLTI1Ni1nY206cGFzcw==@192.0.2.12:8388#LD-US-02
```

- [ ] **Step 2: 加 usa-landing 组 + chain 到 external_chain.ini**

在 `[custom]` 段内追加:
```ini
custom_proxy_group=usa-landing`url-test`(LD-US)`http://www.gstatic.com/generate_204`300,5
chain=US-Chain`(JP|日本)`[]usa-landing
ruleset=US-Chain,[]DOMAIN-SUFFIX,openai.com
```

- [ ] **Step 3: 加多节点断言到 run.sh**

在断言区追加:
```bash
check "clash LD-US-01 dialer"  "$CLASH" "dialer-proxy: US-Chain-front"
check "quanx backhaul vps1"    "$QUANX" "ip-cidr, 192.0.2.11/32"
check "quanx backhaul vps2"    "$QUANX" "ip-cidr, 192.0.2.12/32"
check "quanx landing group"    "$QUANX" "usa-landing, via-interface=%TUN%"
```

- [ ] **Step 4: 编译并跑 golden 看通过**

Run:
```bash
cd build && make -j && cd .. && bash test/chain/run.sh build/subconverter
```
Expected: `PASS` —— 两台 LD-US 各带 `dialer-proxy: US-Chain-front`、两条 backhaul(192.0.2.11/12)、QuanX 规则改写指向 `usa-landing` + via-interface。若某条 FAIL,对照 BF2(组解析)/BF4(backhaul 遍历)排查。

- [ ] **Step 5: Commit**

```bash
git add test/chain/nodes.txt test/chain/external_chain.ini test/chain/run.sh
git commit -m "test(chain): end-to-end multi-node landing via []group ref (BF2+BF3+BF4)"
```

---

## Task 5: 收尾验证

- [ ] **Step 1: 全量 golden + 手工核对一次真实订阅**

Run:
```bash
cd build && make -j && cd .. && bash test/chain/run.sh build/subconverter
```
Expected: `PASS`(全部断言:旧 5 条改造后 + anytls 3 条 + 多节点 4 条)。

- [ ] **Step 2: 更新设计文档 status 为 implemented**

把 `docs/superpowers/specs/3dot141/260603-quanx-anytls-chain-landing-design.md` frontmatter `status: draft` 改 `status: implemented`,`last_updated` 改当天。

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/3dot141/260603-quanx-anytls-chain-landing-design.md
git commit -m "docs(chain): mark design implemented"
```

- [ ] **Step 4: 部署(使用方,非本 repo 自动化)**

按设计文档「其他.部署」:替换 `service.yes365.fun:40008` 二进制前,本地手工拉一次 `target=quanx` 与 `target=clash` 订阅,核对 anytls 行数 == 节点数、Clash 落地节点含 dialer-proxy、QuanX backhaul 行数 == 落地 server 去重数;并在配置仓库 `Rules-For-Quantumult-X` 加 `usa-landing` 组 / `chain=` / 删 `all_base.tpl` quanx 死代码。

---

## 风险与未决(实施时注意)

- **mihomo relay 弃用版本**(设计 Q2):relay 退役后,确认目标 mihomo 版本对 `dialer-proxy` 的支持(≥ 1.x 均支持);若用户客户端过旧需提示升级。
- **dialer-proxy 指向 url-test 组**:落地组若用 url-test,`dialer-proxy: US-Chain-front` 指向的是 front 组;落地选节点由 `usa-landing` 组自身在 rules 中作 policy 决定。实施后用真实 mihomo 跑一次确认链路通(设计未联网验证的两点之一)。
- **anytls 解析 share-link 格式**:Task 1 的 `anytls://` fixture 需与 `subparser.cpp` explodeAnyTLS 解析格式一致;若 fixture 解析失败,先单独 curl 验证该节点能被解析进池。
