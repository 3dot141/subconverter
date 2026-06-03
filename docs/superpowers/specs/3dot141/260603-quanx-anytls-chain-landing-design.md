---
type: design-doc
topic: QuanX 输出 anytls 节点 + chain 落地支持引用 policy 组(一组 VPS)+ 分组配置收尾
date: 260603
author: 3dot141
status: implemented
last_updated: 260603
---

# QuanX anytls 输出 + chain 多节点落地设计

## 背景

用户(3dot141)自部署的 subconverter(`service.yes365.fun:40008`,即本 fork)把 anytls 机场订阅转成 QuantumultX / Clash 多端配置。围绕落地链式代理,当前有三组问题:

**核心问题:QuanX 输出端丢弃 anytls 节点。** `proxyToQuanX`(`src/generator/config/subexport.cpp:1731`)的协议 switch 只覆盖 VMess / VLESS / SS / SSR / HTTP(S) / Trojan / SOCKS5,anytls 落到 `default: continue` 被静默丢弃。用户机场全是 anytls 节点,后果是 QuanX 订阅里地区组(香港/日本/…)匹配不到任何节点、近乎空——而 Clash 端早已支持(subexport.cpp:569)。QuanX 1.5.6(914)起官方已支持 anytls 行格式,目标语法存在,补 emitter 即可。

> 状态说明:本设计的 anytls emitter 改动**已写入工作区代码但尚未编译、尚未部署**(`subexport.cpp:1888-1916`)。线上 `service.yes365.fun` 跑的二进制仍是旧版,**当前仍在丢弃 anytls**。"已写入待编译验证"贯穿全文,不等于"已生效"。

**主要问题:chain 落地只能是单个节点,无法用一组 VPS。** `resolveChains`(`src/generator/config/chaingen.cpp:42`)硬性要求 landing 正则恰好命中 1 个节点,否则整条链作废。用户的诉求是把"一组美国 VPS"定义成一个独立 policy(如 `usa-landing`,容灾/负载均衡),让链式终点落到这个组上——当前能力做不到。

**辅因(随上面一起处理,不单独立项):**
- Clash 链式现在生成 `relay` proxy-group(chaingen.cpp:81),而 mihomo 的 `relay` 已标记弃用、部分版本可能已移除(见方案选型 Q2),官方推荐改用 `dialer-proxy`。借这次把 relay 退役。
- 配置仓库 `all_base.tpl` 里有一段 `request.target == "quanx"` 是死代码(见方案选型 Q5 / 部署节)。

> 配置仓库(`Rules-For-Quantumult-X`)与 subconverter fork 是两个 repo。本设计的代码改动在 fork;配置仓库的改动(chain= / groups.txt / 删死代码)是使用方调整,列在「其他.部署」。

## 目标

1. QuanX 订阅正确输出 anytls 节点,字段对齐官方 sample.conf(`anytls = host:port, password=…, over-tls=true, tls-host=…, tls-verification=…, fast-open/udp-relay/tag`,Reality 时含 `reality-base64-pubkey/reality-hex-shortid`)。
2. chain 的 `landing` 支持 `[]组名` 引用一个已定义的 policy 组;该组可含一组 VPS,组类型(容灾/负载均衡/优选)由使用方在 custom_proxy_group 中自定义,**不引入新的 `landing_type` 参数**。
3. 两端正确生成链式:Clash 用 `dialer-proxy`(relay 退役),QuanX 用 `via-interface=%TUN%` + 对落地组**每个 server** 各一条 backhaul。
4. 单节点 landing 的旧用法保持兼容;现有 `test/chain` golden 测试同步更新。

非目标:落地组为"手写节点列表型 / 非普通 regex matcher"(只支持普通 remark 正则筛选型);Surge 链式落地(`UnderlyingProxy` 虽可复用,但 Surge 无 chain 注入点,本期不接);地区组全量细分;详细部署 runbook。

## 架构

### 流程图

chain 解析不在 `interfaces.cpp` 主流程,而是**嵌在两个 emitter 内部各独立调一次**——这是上一版初稿画错的地方,据 `subexport.cpp` 实际调用点修正:

```
 proxyToClash(nodes, …, extra_proxy_group)            proxyToQuanX(nodes, …, extra_proxy_group)
 subexport.cpp:~255 节点循环之前:                       subexport.cpp:~2000:
   resolveChains(chains, nodes, proxyGroups)             resolveChains(chains, nodelist, proxyGroups)
   landing="[]usa-landing" → 查组正则 → 落地节点集          landing="[]usa-landing" → 同
     └─ 给 nodes 中落地节点写 UnderlyingProxy=front         ├─ quanXBackhaulRules: 每个落地 server 一条 → front
 节点循环(:255-715)读 nodes:                              └─ rewriteQuanXChainRules: 命中链名规则
   UnderlyingProxy 非空 → singleproxy["dialer-proxy"]          policy 改写为 usa-landing + via-interface=%TUN%
 循环后(:734):appendClashFrontGroups 只加 front 辅助组
   (不再生成 relay 组)
```

### 文本总结

整体改造分两类。**协议输出层**:`proxyToQuanX` 增加 `AnyTLS` 分支,与 VLESS/Trojan 分支同构(已写入待编译)。**链式生成层**:`resolveChains` 从"landing 解析为单节点"升级为"landing 可为 `[]组引用`,解析为一组落地节点(`landingNodes[]`)"。关键约束是 **Clash 的注入时序**——`proxyToClash` 在节点循环(`subexport.cpp:255-715`)里就把每个节点固化进 proxies YAML(`:729`),所以"给落地节点写底层代理"必须在该循环**之前**完成,循环时才能输出 `dialer-proxy`;原 `appendClashChains` 在 `:734`(循环后)调用、且收到的是循环内复制的 `nodelist` 副本,改它影响不到已生成的 YAML——故拆成"循环前注入 nodes"+"循环后加 front 组"两步。QuanX 端无此时序约束(它是事后规则改写 + backhaul),保持在 `:2000`。落地组成员通过复用 `groupGenerate` 的匹配语义、用组的 remark 正则从节点池筛出。核心数据约束:落地 VPS 必须有独立 IP/域名且备注可正则区分(如 `LD-US` 前缀),否则 backhaul 按 server 去重会波及共享地址的机场节点。

## 实现

### 影响

```
src/generator/config/
├── subexport.cpp                        (改)
│     proxyToQuanX:
│       ① case AnyTLS 协议分支(已写入待编译, 1888-1902)
│       ② 尾部 tls-verification 排除列表加 AnyTLS(已写入待编译, 1914)
│       ③ ~2000 chain 处:resolveChains 传 extra_proxy_group;backhaul 遍历 landingNodes;规则改写指向 landingGroup
│     proxyToClash:
│       ④ 节点循环(255)之前:新增 injectClashChains(resolveChains + 给 nodes 落地节点写 UnderlyingProxy)
│       ⑤ 节点循环通用字段区:UnderlyingProxy 非空 → singleproxy["dialer-proxy"]
│       ⑥ :734 处:appendClashChains 改为 appendClashFrontGroups(只加 front 辅助组, 不再 push relay 组)
├── chaingen.h                           (改)
│       ⑦ ResolvedChain:landingNodes[]、landingIsGroupRef、landingGroup(替换单值 landingTag/landingServer)
│       ⑧ resolveChains 签名 +const ProxyGroupConfigs&
│       ⑨ appendClashChains 拆为 injectClashChains(chains, nodes, groups, proxyGroups) + appendClashFrontGroups(chains, groups)
└── chaingen.cpp                         (改)
        ⑩ resolveChains:landing 为 []组引用 → 查组 remark 正则、筛落地节点集;单节点 landing 退化为 landingNodes size=1
        ⑪ injectClashChains:给落地节点写 UnderlyingProxy=frontGroup(不再生成 relay)
        ⑫ quanXBackhaulRules:遍历 landingNodes 每个 server 各一条(原单 server 逻辑泛化)
        ⑬ rewriteQuanXChainRules:policy 改写为 landingGroup(组引用)或 landingNodes[0].tag(单节点)

src/handler/interfaces.cpp               (不改)  已把 lCustomProxyGroups 传给 proxyToClash(:770)/proxyToQuanX(:915);
                                                  proxyToClash/QuanX 签名已含 extra_proxy_group + nodes,无需动入口

src/parser/config/proxy.h                (不改)  复用现有 UnderlyingProxy 字段(:145),无需新增

test/chain/
├── run.sh                               (改)  ⑭ 现有断言 `type: relay`(run.sh:33)改为断言 dialer-proxy;
│                                               ⑮ 新增多节点 landing 引用组场景断言(Clash dialer-proxy×N + QuanX 多 backhaul)
└── external_chain.ini / nodes.txt       (改)  ⑯ fixture 加一组 LD-* 落地节点 + 一个落地组 + landing=[]组 的 chain
```

### 接口设计

#### 内部接口(chaingen)

```cpp
struct ResolvedChain {
    std::string name;
    bool        frontIsRef = false;
    std::string frontGroup, frontFilter, frontType;          // 不变
    bool        landingIsGroupRef = false;                    // landing 写法是 "[]组名"
    std::string landingGroup;                                 // 组名(QuanX policy 位用)
    struct LandingNode { std::string tag; std::string server; bool isIP; };
    std::vector<LandingNode> landingNodes;                    // 落地节点集(单节点时 size=1)
    bool        valid = false;
};

// +proxyGroups:landing 为 []组引用时据此查组的 remark 正则
std::vector<ResolvedChain> resolveChains(const ChainConfigs &chains,
                                         std::vector<Proxy> &nodes,
                                         const ProxyGroupConfigs &proxyGroups);

// 拆分:注入(循环前,改 nodes)与加组(循环后,改 groups)分离 —— 见方案选型 Q6
void injectClashChains(const ChainConfigs &chains, std::vector<Proxy> &nodes,
                       const ProxyGroupConfigs &proxyGroups,
                       std::vector<ResolvedChain> &outResolved);   // 给落地节点写 UnderlyingProxy
void appendClashFrontGroups(const std::vector<ResolvedChain> &chains, ProxyGroupConfigs &groups);
```

`quanXBackhaulRules` / `rewriteQuanXChainRules` 签名不变,内部改为遍历 `landingNodes`。

**`filterOfGroup` 语义(W1 修正)**:`ProxyGroupConfig.Proxies`(binding.h:266)把组的所有规则统一存为字符串列表,结构上不区分类型;`groupGenerate`(subexport.cpp:193-214)运行时按前缀分派:`[]`=组引用、`script:`=脚本、`!!TYPE/!!SERVER/...`=matcher、其余=普通 remark 正则。本设计**只接受"全部条目均为普通 remark 正则"的落地组**:复用 `groupGenerate` 的匹配路径取出命中节点;若组含 `[]`/`script:`/`!!` 任一条目 → 视为不支持,warn + 该链作废(非目标)。多条普通正则取并集。

#### Clash 节点输出(subexport.cpp,proxyToClash 节点循环通用字段区)

```cpp
if (!x.UnderlyingProxy.empty())
    singleproxy["dialer-proxy"] = x.UnderlyingProxy;   // 与 Surge underlying-proxy(:1150)同语义
```

### 业务流

**BF1 — anytls 节点输出为 QuanX 行(已写入待编译验证)**

```cpp
// proxyToQuanX 协议 switch 内,default 之前
case ProxyType::AnyTLS:                                          // anytls 节点
    proxyStr = "anytls = " + hostname + ":" + port +             // host:port
               ", password=" + password;                        // 凭据
    proxyStr += ", over-tls=true";                              // anytls 恒走 TLS,固定 true
    if (!x.SNI.empty())                                          // 解析时 tls-host 存入 SNI(非 Host)
        proxyStr += ", tls-host=" + x.SNI;
    if (!x.PublicKey.empty())                                    // Reality:QuanX 官方 sample.conf 的 anytls 行支持
        proxyStr += ", reality-base64-pubkey=" + x.PublicKey;   //   reality-base64-pubkey(非 dead branch,见 Q 答复)
    if (!x.ShortId.empty())
        proxyStr += ", reality-hex-shortid=" + x.ShortId;
    if (!scv.is_undef())                                         // 证书校验:scv 反转
        proxyStr += ", tls-verification=" + scv.reverse().get_str();
    else
        proxyStr += ", tls-verification=false";                 // 缺省不校验,贴合机场样例
    break;                                                       // fast-open/udp-relay/tag 由尾部统一追加
```

**BF2 — chain landing 解析:支持 []组引用,反查落地节点集**

```cpp
function resolveChains(chains, nodes, proxyGroups):
    for c in chains:                                             // 逐条链解析
        rc.name = c.Name
        parseFront(rc, c)                                        // front 逻辑不变([]组 / 正则)
        if startsWith(c.Landing, "[]"):                          // landing 是组引用
            rc.landingIsGroupRef = true
            rc.landingGroup = c.Landing.substr(2)               // 去 "[]" 取组名
            grp = findGroup(proxyGroups, rc.landingGroup)       // 在 custom_proxy_group 定位该组
            if grp not found: warn(...); rc.valid=false; continue
            if grp.Proxies 含 []/script:/!! 条目:               // 非"普通正则筛选型"→ 不支持(W1)
                warn("landing group not regex-filtered"); rc.valid=false; continue
            members = groupGenerate(grp, nodes)                 // 复用现有匹配:普通正则并集筛节点
            for n in members:
                rc.landingNodes.push({n.Remark, n.Hostname, isIP(n.Hostname)})
        else:                                                    // 单节点 landing(旧用法)
            matched = matchUnique(nodes, c.Landing)             // 原精确/正则匹配,要求唯一
            if matched.size != 1: warn(...); rc.valid=false; continue
            rc.landingNodes.push({matched.Remark, matched.Hostname, isIP(...)})
        if rc.landingNodes.empty(): warn("no landing node"); rc.valid=false; continue
        rc.valid = true
```

**BF3 — Clash 端:落地注入提前到节点循环之前(时序修正)**

```cpp
// 调用点:proxyToClash 内,节点循环(subexport.cpp:255)之前
function injectClashChains(chains, nodes, proxyGroups, outResolved):
    outResolved = resolveChains(chains, nodes, proxyGroups)      // 解析(此处 nodes=函数入参,非副本)
    for c in outResolved where c.valid:
        for ln in c.landingNodes:                               // 给每个落地节点设底层代理
            node = findNodeRef(nodes, ln.tag)                   // 拿 nodes 里 Proxy 的引用(同一对象)
            if node: node.UnderlyingProxy = c.frontGroup        // 循环随后读 nodes → 输出 dialer-proxy
    // 不在此加任何 group;front 辅助组留到循环后

// 节点循环(:255-715)照常遍历 nodes 构建 singleproxy;通用字段区按上文输出 dialer-proxy
// 循环后(:734):
function appendClashFrontGroups(resolved, groups):
    for c in resolved where c.valid and not c.frontIsRef:       // front 是正则才建辅助组
        groups.push(buildFrontGroup(c))                         // 落地组本身由 custom_proxy_group 正常生成
    // 不再 push relay 组(relay 退役)
```

**BF4 — QuanX 端:每个落地 server 一条 backhaul + 规则改写指向落地组**

```cpp
function quanXBackhaulRules(chains):                             // 调用点 subexport.cpp:~2000
    seen = set(); out = []
    for c in chains where c.valid:
        for ln in c.landingNodes:                               // 遍历落地节点集(多 VPS → 多条)
            key = ln.server + "|" + c.frontGroup
            if key in seen: continue                            // 同 server+front 去重
            seen.add(key)
            if isIPv4(ln.server): out.push("ip-cidr, "+ln.server+"/32, "+c.frontGroup)
            elif isIPv6(ln.server): out.push("ip6-cidr, "+ln.server+"/128, "+c.frontGroup)
            else: out.push("host, "+ln.server+", "+c.frontGroup)
    return out

function rewriteQuanXChainRules(ini, chains):
    // 命中链名的 filter 规则,policy 改写:组引用→landingGroup;单节点→landingNodes[0].tag
    // 末尾追加 via-interface=%TUN%;backhaul 插在改写规则之前(先匹配,保回程优先)
```

### 异常与失败模式

| BF | 场景 | 触发 | 处理 | 上抛/吞 |
|---|---|---|---|---|
| BF1 | anytls 缺 SNI / password | 解析残缺节点 | 缺 SNI 不输出 tls-host、缺 password 输出空值(走 happy path,不特判) | 吞 |
| BF2 | landing 组不存在 | `[]组名` 在 custom_proxy_group 查不到 | warn + 该链 valid=false 跳过 | 吞 |
| BF2 | landing 组非普通正则型 | 组含 `[]`/`script:`/`!!` 条目 | warn + 跳过(非目标) | 吞 |
| BF2 | 组正则匹配 0 节点 | 落地 VPS 未进节点池 / 正则不匹配 | warn + 跳过 | 吞 |
| BF2 | 单节点 landing 命中 ≠1 | 旧用法正则歧义 | warn + 跳过(行为不变) | 吞 |
| BF3 | findNodeRef 找不到 tag | landingNodes 与 nodes 不一致 | 跳过该节点注入 + warn | 吞 |
| BF4 | 落地 VPS 共享 server 地址 | VPS 用同域名+端口 | backhaul 去重合并,波及共享地址节点 | 不处理(设计约束,见 Q4) |

### 单测设计

载体:扩展现有 `test/chain/` 的 shell golden(`run.sh` 跑订阅 diff 对 fixture)。subconverter 无单元测试框架,纯函数(如 `resolveChains` 返回值)通过**最终产物**间接验证;不引入新框架(见 Q3)。每条 case 用 Given/When/Then 三行。

**BF1 — anytls QuanX 行**
- case 1.1 主路径:Given anytls 节点(SNI=cdn.x, scv 未定义);When 转 QuanX;Then 产物含 `anytls = host:port`、`over-tls=true`、`tls-host=cdn.x`、`tls-verification=false`,且 tls-verification 仅一次。
- case 1.2 Reality:Given 节点含 PublicKey+ShortId;When 转 QuanX;Then 产物含 `reality-base64-pubkey=`、`reality-hex-shortid=`。
- case 1.3 缺 SNI:Given anytls 节点 SNI 为空;When 转 QuanX;Then 产物不含 `tls-host=`,其余字段正常。

**BF2 — landing 组引用解析**
- case 2.1 主路径:Given 节点池含 LD-US-01/02 + 机场节点,组 `usa-landing` 正则 `(LD-US)`;When 转换 landing=`[]usa-landing`;Then 链 valid,落地节点集=两 LD-US。
- case 2.2 组不存在:Given landing=`[]nope`;When 转换;Then 链跳过 + warn,产物无该链相关规则。
- case 2.3 组非正则型:Given `usa-landing` 含 `[]其他` 条目;When 转换;Then 链跳过 + warn。
- case 2.4 组匹配 0 节点:Given 正则 `(LD-XX)` 无匹配;When 转换;Then 链跳过 + warn。
- case 2.5 单节点兼容:Given landing=`my-vps`(精确命中 1);When 转换;Then 链 valid,落地集 size=1。

**BF3 — Clash dialer-proxy**
- case 3.1 主路径:Given 有效链 front=`日本`,落地 LD-US-01/02;When 转 Clash;Then 产物两节点各含 `dialer-proxy: 日本`,且**无 `type: relay` 组**。
- case 3.2 findNode 失败:Given landingNodes 含一个 nodes 里不存在的 tag;When 转 Clash;Then 该节点跳过注入(无 dialer-proxy)+ warn,转换不崩。

**BF4 — QuanX backhaul + 改写**
- case 4.1 多节点:Given 落地 1.1.1.1 / 2.2.2.2;When 转 QuanX;Then backhaul 两条 `ip-cidr,1.1.1.1/32,日本`、`…2.2.2.2…`。
- case 4.2 规则改写:Given filter 规则 policy==`USA-Chain`;When 转 QuanX;Then 改写为 `…, usa-landing, via-interface=%TUN%`,且 backhaul 行在其之前。
- case 4.3 共享 server 去重:Given 两落地节点同 server;When 转 QuanX;Then backhaul 仅一条。

**回归 — relay 退役**
- case R.1:Given 现有 `external_chain.ini` 单节点 chain;When 转 Clash;Then 产物用 `dialer-proxy` 而非 `type: relay`(更新 run.sh:33 原断言)。

## 方案选型

### Q1: landing 多节点怎么表达?
**选项**: 新增 `landing_type` 参数(chain 自己生成落地组) vs 复用 `[]组名` 引用 custom_proxy_group。
**定**: 选 `[]组名`。因 front 已支持 `[]组` 引用,landing 对称;组类型由使用方在 groups 自定义,改动更小。→ 影响 BF2。

### Q2: Clash 链式用 relay 还是 dialer-proxy?
**选项**: 继续生成 `relay` 组 vs 迁 `dialer-proxy`。
**定**: 迁 `dialer-proxy`。因 mihomo relay 已弃用(官方 wiki 标 "about to be deprecated";有第三方资料称新版启动即报错——**来源冲突,实施前按目标 mihomo 版本确认**),且 relay 第二跳为组的支持未证实;dialer-proxy 是 per-node 字段,天然支持"一组落地 + 任意组类型"。→ 影响 BF3。

### Q3: dialer-proxy 用什么字段承载?
**选项**: 新增 `DialerProxy` 字段 vs 复用现有 `UnderlyingProxy`。
**定**: 复用 `UnderlyingProxy`(proxy.h:145)。因其语义就是"底层代理",Surge 已输出 `underlying-proxy`(subexport.cpp:1150);Clash emitter 补一行输出 `dialer-proxy` 即可。**注**:Surge 虽共用此字段,但 Surge emitter 当前**没有 chain 注入点**,本期不接 Surge 链式(非目标),不存在"自动顺带支持"。→ 影响 BF3、proxyToClash。

### Q4: 落地 VPS 共享 server 地址怎么办?
**选项**: 自动处理(per-node 路由) vs 设为设计约束。
**定**: 设为约束。因 QuanX backhaul 按 server 地址匹配,同地址无法区分;要求落地 VPS 独立 IP/域名 + 备注可正则区分。用户已确认 VPS 独立。共享地址时 warn 提示,不强行处理。

### Q5: 落地组为"手写列表型 / 非普通 regex matcher"是否支持?
**选项**: 支持(回传组成员列表 / 解析所有 matcher) vs 只支持普通 remark 正则筛选型。
**定**: 只支持普通正则筛选型(YAGNI)。因 `ProxyGroupConfig` 结构不带类型元信息,组条目可能是 `[]`/`script:`/`!!SERVER`/regex 混合;全量解析复杂度不值。含非普通正则条目的组 warn 跳过。→ 影响 BF2、`filterOfGroup` 语义。

### Q6: Clash 注入为什么要拆"循环前注入 + 循环后加组"?
**选项**: 保持单个 `appendClashChains`(循环后) vs 拆成注入(循环前)+ 加组(循环后)。
**定**: 拆。因 proxies YAML 在节点循环(`:255-715`)就固化、`:734` 收到的是 `nodelist` 副本,循环后改节点影响不到 dialer-proxy 输出;注入必须前置到循环前写原 `nodes`,而 front 辅助组操作的是 `merged_groups`(`:736` 才遍历),保留在循环后。→ 影响 BF3。

## 其他

### 部署

本 fork 改动 + 使用方配置调整,顺序:

1. **编译验证**:本 fork 用 CMake,`mkdir build && cd build && cmake .. && make -j`。**所有改动(含已写入的 anytls emitter)此前从未编译过(开发环境缺 cmake),实施第一步必须先编译通过 + 跑 `test/chain/run.sh` golden。**
2. **灰度**:单机部署,无多实例。先用本地 subconverter 跑 golden + 手工拉一次 QuanX/Clash 订阅核对节点数与链式规则,再替换 `service.yes365.fun:40008` 二进制。
3. **回滚预案**:保留旧二进制;新订阅异常(QuanX 节点数骤降 / Clash 出现 `type: relay` 报错 / 链式不通)即换回旧二进制。
4. **监控指标**:无 metrics 系统,以人工核对替代——关键观测项:QuanX 订阅 `[server_local]` 段 anytls 行数 == 节点数(不为 0);Clash 落地节点含 `dialer-proxy` 字段;QuanX backhaul 行数 == 落地组去重后 server 数。
5. **使用方配置**(`Rules-For-Quantumult-X`,非本 repo):
   - `groups.txt` 加落地组,如 `usa-landing\`load-balance\`(LD-US)\`http://www.gstatic.com/generate_204\`300,5`
   - `my.ini [custom]` 加 `chain=USA-Chain\`[]日本\`[]usa-landing`
   - `rulesets.txt` 把目标域名指到链名:`ruleset=USA-Chain,[]DOMAIN-SUFFIX,openai.com`
   - 删 `all_base.tpl` 的 `request.target == "quanx"` 死代码段(quanx 走 `quanx_rule_base`→`quanx.conf`,interfaces.cpp:533 确认,那段永不渲染)
   - 落地 VPS 备注统一加 `LD-US` 前缀,确保与机场节点正则可区分

## Review Log

### Review 1 — 2026-06-03(交叉验证:general-purpose subagent + codex 双跑)

合并 Report(标注「双方都提=高置信」/单方来源;Evidence Gate 命中处带 `path:line`):

**Critical**
- C1〔双方·已核实〕`实现.影响`:调用点写错成 `interfaces.cpp`,真实调用点在 `subexport.cpp:734`(Clash)/`:2000`(QuanX);interfaces.cpp 仅把 lCustomProxyGroups 传给 emitter(`:770/:915`)。
- C2〔双方·已核实〕`BF3`:Clash dialer-proxy 注入时序不可行——proxies YAML 在 `:713/:729` 固化,`appendClashChains` 在 `:734` 才调且收 `nodelist` 副本,改它影响不到输出。需把注入提前到节点循环之前。
- C3〔codex〕`单测设计`:异常表每行无对应 case,未按 Given/When/Then 三行展开。

**Warning**
- W1〔双方·codex 更深〕`BF2/filterOfGroup`:`ProxyGroupConfig.Proxies`(binding.h:266)不带类型元信息,混 `[]`/`script:`/`!!`/regex;需定义"正则筛选型"判定 + 多条规则处理。
- W2〔codex〕`UnderlyingProxy`:Surge 无 chain 注入点,"顺带 Surge 链式"结论不成立。
- W3〔codex〕`测试`:现有 `run.sh:33` 仍断言 `type: relay`,relay 退役后会失败,需更新。
- W4〔codex〕`部署`:缺监控指标(三件套缺一)。
- W5〔双方·同根〕`已落地`标签:BF1 代码从未编译,标"已落地"误导;应拆"当前阻塞"vs"已写入待验证"。
- W6〔gp〕`BF1`:anytls+Reality 是否真实存在存疑(否则 `:1893-1896` 为 dead branch)。

**Suggestion**
- S1〔gp〕异常表缺 BF1(anytls 字段缺失)行。
- S2〔gp〕流程图把 resolveChains 画进主流程,未体现嵌在两 emitter 内各调一次,强化"改 interfaces.cpp"误读。

**Open Questions**
- Q1〔gp〕`ProxyGroupType::Relay` 枚举是否清理?
- Q2〔双方〕`all_base.tpl` quanx 死代码在外部 repo,无法本仓核实。
- Q3〔双方〕测试载体:现有是 shell golden,纯函数断言难表达,引入新框架?
- Q4〔codex〕mihomo relay 弃用是外部事实,未联网核实。

**Self-Audit**
- SA1〔双方·与 C1/C2 同根〕照影响节去 interfaces.cpp 找不到调用点 + 注入时序晚于 YAML,第一步实施不下去。
- SA2〔gp·与 C2 同根〕findNode 回写对象身份(nodelist 副本 vs 原 nodes)未澄清。
- SA3〔gp〕QuanX 端 `:2000` 也独立调一次,影响节未列(proxyToQuanX 已有 extra_proxy_group 参数,可行)。

**Verdict(双方一致)**:Fail,至少修 C1/C2/C3。

**用户决定**:全修(C1-C3 / W1-W6 / S1-S2 / SA1-SA3),Open Questions Q1-Q4 按作者草案答。

**本轮修订**:
- C1 + SA3:影响节调用点改正——Clash 在 `subexport.cpp:255` 循环前 + `:734`,QuanX 在 `:2000`;interfaces.cpp 标(不改)。
- C2 + SA1 + SA2:重写 BF3,注入提前到节点循环前写原 `nodes`(非 `nodelist` 副本);新增方案选型 Q6 记录拆分决策;架构.文本总结 + 流程图同步。
- C3:单测设计扩为每条 BF 主路径 + 异常表每行对应 case,全部 Given/When/Then 三行;加回归 case R.1。
- W1:接口设计补 `filterOfGroup` 精确语义(只接受全普通正则条目,复用 groupGenerate,多条取并集);BF2 同步;方案选型加 Q5。
- W2:Q3「定」加注 Surge 无注入点、本期不接;非目标补一条。
- W3:影响节 + 单测加 `run.sh:33` 断言更新(case R.1)。
- W4:部署节加监控指标(人工核对项)。
- W5:背景加状态说明块;BF1 / 影响节"已落地"改"已写入待编译验证";部署节强调首步必编译。
- W6:经查 QuanX 官方 sample.conf 的 anytls 行支持 `reality-base64-pubkey/reality-hex-shortid`,非 dead branch;BF1 注释补依据。
- S1:异常表加 BF1 行。
- S2:流程图重画为两 emitter 内各调一次。

**Open Questions 答复**:
- Q1:只删 chaingen 生成 relay 的 push,**保留 `ProxyGroupType::Relay` 枚举**(用户可能手写 relay 组,动枚举波及解析)——已写入影响节⑥措辞。
- Q2:已读 `all_base.tpl` 确有 `{% if request.target == "quanx" %}` 段;结合 base 选择逻辑(interfaces.cpp:533,quanx 走 quanx.conf),该段在使用方 my.ini 配置下不渲染——已写入部署节作为已核实结论。
- Q3:仍用 `test/chain` shell golden,不引入新框架,纯函数逻辑经最终产物覆盖——已写入单测设计节首。
- Q4:已 web 查证,官方 wiki 称"即将弃用"、第三方称新版已移除(冲突),实施前按目标 mihomo 版本确认——已写入方案选型 Q2。
