# 链式代理覆盖（Chain Proxy Override）设计文档

- 日期：2026-06-02
- 目标仓库：`3dot141/subconverter`（fork of tindy2013/subconverter）
- 状态：已批准，待实现

## 1. 背景与目标

subconverter 把机场订阅转换成 Clash / Quantumult X（下称 QuanX）等客户端配置。用户希望在一份源订阅之外，用自己的**覆盖配置**定义"链式代理（chained proxy / relay）"——让流量先经机场节点中转、再从自己的境外 VPS 落地，以规避机场审计、获得稳定落地 IP。

诉求形态（已与用户确认）：**多条命名链 + 按域名路由到不同链 + Clash 与 QuanX 两端从同一份定义自动派生**。

### 现状（已验证，来自代码勘探）

| 能力 | Clash 输出 | QuanX 输出 |
|---|---|---|
| `relay` 类型策略组 | ✅ 已支持（`src/generator/config/subexport.cpp:745`） | ❌ 被丢弃（`default: continue`，`subexport.cpp:1935`） |
| 单节点 `dialer-proxy`/`underlying-proxy` | ❌ 未生成 | ❌ 无 |
| `Proxy.UnderlyingProxy` 字段 | 存在（`src/parser/config/proxy.h:145`），仅 Surge 输出消费 | — |
| `via-interface=%TUN%` 回流脚手架 | — | ❌ 完全没有 |

覆盖机制现成：`ExternalConfig`（`src/handler/settings.h:76`），经 `config=` URL 参数加载，已支持 `custom_proxy_group`、`*_rule_base`、`ruleset`、`rename`、`emoji` 等，并注入到各目标输出（`src/handler/interfaces.cpp:516-555`）。**仓库无单元测试套件。**

### 非目标（YAGNI）

- 不支持 3 跳及以上的任意长链（仅"前置 → 落地"两跳；前置本身可以是一个已包含中转的策略组，但本特性不主动生成 3+ 跳）。
- 不新造"在 override 配置里内联完整节点定义"的机制——落地节点复用现有节点入口（见 §6）。
- 不改 Surge/Loon/SingBox 等其它目标的链式行为。

## 2. 总体设计

新增一等公民 **chain** 覆盖段：只声明链的**拓扑**（链名 / 前置 / 落地），"哪些域名走哪条链"复用 subconverter 现成的 `ruleset=` 机制（policy 名 = 链名）。

```
                        ┌─ Clash emitter ──→ relay 策略组 + 复用 ruleset 规则
chain 定义 (拓扑) ──────┤
                        └─ QuanX emitter ──→ 前置策略组 + ip-cidr/host 回流 + 规则改写(via-interface=%TUN%)
```

一次定义、两端派生。域名路由白嫖现有 `ruleset`，新增配置面最小。

## 3. 配置 Schema

### 3.1 字段语义

| 字段 | 含义 | 取值 |
|---|---|---|
| `name` | 链名（= 派生出的策略组名 / ruleset policy 名） | 字符串，唯一 |
| `front` | 前置（中转，机场节点） | `[]组名` 引用已有策略组；否则按**节点 remark 正则**生成辅助组 `<name>-front` |
| `landing` | 落地（VPS 节点引用） | 节点 tag 或正则，**必须解析到唯一节点** |
| `front_type` | 辅助前置组类型，可选 | `select`(默认) / `url-test` / `fallback` / `load-balance`；仅当 `front` 为正则时生效 |

域名路由用现有 `ruleset`，policy 名填链名：
```ini
ruleset=JP-Chain,[]DOMAIN-SUFFIX,google.com
ruleset=US-Chain,https://example.com/US-domains.list
```

### 3.2 三种格式

INI（反引号分隔，贴合 `example_external_config.ini`）：
```ini
; chain = 链名 ` 前置 ` 落地 ` [前置组类型]
chain=JP-Chain`[]🇯🇵 节点`my-jp-vps
chain=US-Chain`(美国|US)`my-us-vps`url-test
```

TOML：
```toml
[[chain]]
name = "JP-Chain"
front = "[]🇯🇵 节点"
landing = "my-jp-vps"

[[chain]]
name = "US-Chain"
front = "(美国|US)"
landing = "my-us-vps"
front_type = "url-test"
```

YAML：
```yaml
chain:
  - name: JP-Chain
    front: "[]🇯🇵 节点"
    landing: my-jp-vps
  - name: US-Chain
    front: "(美国|US)"
    landing: my-us-vps
    front_type: url-test
```

## 4. 数据模型

新增 `src/config/chain.h`：
```cpp
#include "utils/string.h"

struct ChainConfig {
    String Name;
    String Front;       // "[]Group" 或 节点 remark 正则
    String Landing;     // 节点 tag 或正则（须唯一）
    String FrontType;   // 可选，默认 "select"
};
using ChainConfigs = std::vector<ChainConfig>;
```

挂载点：
- `ExternalConfig`（`settings.h:76`）新增 `ChainConfigs chains;`
- `Settings`（`settings.h:18`，可选）新增全局默认 `ChainConfigs customChains;`，与 `customProxyGroups` 同级，供无 external config 时的全局默认（本期可只接 ExternalConfig，全局默认列为后续）。

解析（`src/config/binding.h` 新增 `from<ChainConfig>` 的 INI/TOML 绑定；YAML 在 `settings.cpp` 的 `loadExternalYAML` 内解析）。

## 5. 生成逻辑

### 5.1 公共：解析链拓扑

新增 helper（建议 `src/generator/config/nodemanip.cpp` 或新文件 `chaingen.cpp`）：
```cpp
struct ResolvedChain {
    String name;
    String frontGroup;     // 实际前置组名（引用的或生成的 <name>-front）
    bool   frontIsRef;     // front 是否为已有组引用
    String frontFilter;    // 当 frontIsRef=false 时的节点正则
    String frontType;
    String landingTag;     // 解析后的唯一落地节点 tag
    String landingServer;  // 落地节点 server（IP 或域名）
    bool   landingIsIP;    // server 是否为 IP 字面量
    bool   valid;          // 解析失败(landing 非唯一/缺失)则 false
};
std::vector<ResolvedChain> resolveChains(const ChainConfigs&, const std::vector<Proxy>& nodes);
```
- landing 正则在 `nodes` 上匹配，命中数 != 1 → `valid=false` + `writeLog` 警告，跳过该链。
- `landingIsIP` 用现成的 IP 判定（`isIPv4`/`isIPv6`，`src/utils/network.*`，若无则正则）。

### 5.2 Clash（`proxyToClash`，`subexport.cpp`）

在构建 `proxy-groups`（733-802 行附近）后，对每条 valid chain：
1. front 为正则 → 追加辅助组 `{name: <name>-front, type: <front_type>, proxies: [匹配节点...]}`（复用 `groupGenerate`）。
2. 追加 relay 组 `{name: <name>, type: relay, proxies: [<frontGroup>, <landingTag>]}`。
3. 与已有同名组冲突时**链覆盖**（复用 733-802 已有的"同名替换"逻辑）。

域名规则由现有 ruleset 机制输出 `DOMAIN-SUFFIX,google.com,JP-Chain`，无需额外处理。

输出示例：
```yaml
proxy-groups:
  - {name: JP-Chain-front, type: select, proxies: [日本01, 日本02]}
  - {name: JP-Chain, type: relay, proxies: [JP-Chain-front, my-jp-vps]}
rules:
  - DOMAIN-SUFFIX,google.com,JP-Chain
```

### 5.3 QuanX（`proxyToQuanX`，`subexport.cpp`）

QuanX 无 relay，每条 valid chain 派生：
1. **前置策略组**（写入 `[policy]`，1908-1976 行附近）：`front` 为 `[]组`→复用；否则建 `static=<name>-front, server-tag-regex=<frontFilter>, ...`（或按 front_type 用 `url-latency-benchmark`）。
2. **修复 relay 组丢弃**：把 `subexport.cpp:1935` 的 `default: continue` 增加 `case ProxyGroupType::Relay`，让手写 relay 组退化为 `static`（成员即链序），避免静默消失。
3. **回流规则**（写入 `[filter_local]`，`rulesetToSurge` 调用即 1978 行**之前**插入，确保优先级）：
   - `landingIsIP` → `ip-cidr, <landingServer>/32, <frontGroup>`
   - 否则 → `host, <landingServer>, <frontGroup>`
   - 多条链共用同一 landing → 回流规则**去重**。
4. **规则改写**（`rulesetToSurge` 之后，后处理 `[filter_local]`）：凡 policy == 某链名的规则，重写为
   `<rule-type>, <pattern>, <landingTag>, via-interface=%TUN%`。
   未匹配任何链名的规则保持原样。

输出示例：
```
[policy]
static=JP-Chain-front, 日本01, 日本02
[filter_local]
ip-cidr, 1.2.3.4/32, JP-Chain-front
host-suffix, google.com, my-jp-vps, via-interface=%TUN%
```

### 5.4 落地节点来源（§6 呼应"两者都要"）

落地统一为**节点引用**：
- 订阅内已有节点：`landing` 直接按 tag/正则引用。
- 自己的 VPS：作为节点 share-link（`ss://`、`vmess://` 等）放进 `url=` 列表（subconverter 已支持 `|` 拼接多 URL / 单节点链接），再用 `landing` 引用其 tag。

不新增"override 内联节点定义"机制；server/IP 从已解析节点的 `Hostname`/`Server` 读取。

## 6. 接线（Threading）

1. `src/config/chain.h`：新增结构。
2. `src/handler/settings.h`：`ExternalConfig` 加 `ChainConfigs chains;`。
3. `src/config/binding.h`：`from<ChainConfig>` 的 INI / TOML 绑定。
4. `src/handler/settings.cpp`：
   - `loadExternalConfig` INI 分支读取 `chain=`（1224-1288 附近）。
   - `loadExternalTOML` 读 `[[chain]]`。
   - `loadExternalYAML` 读 `chain:`。
5. `src/handler/interfaces.cpp`：把 `ext.chains` 透传，调用处（768 `proxyToClash`、913 `proxyToQuanX`）签名扩展。
6. `src/generator/config/subexport.h`/`.cpp`：两 emitter 签名加 `const ChainConfigs& chains`（或经 `ext` 已有引用传入——优先复用 `ext`），新增 `resolveChains` 与 QuanX 后处理 helper。

> 实现注意：若两 emitter 已能拿到 `ext`/`extra_settings`，优先把 chains 收纳进既有的设置载体，少改签名。

## 7. 测试

仓库无测试套件。新增**golden-file 集成测试**（`test/chain/`）：
- fixture：2~3 个节点 share-link（含一个充当 VPS 的落地节点）+ 一份带 2 条 chain 的 external config + 对应 ruleset。
- 跑构建产物做转换，断言：
  - Clash 输出含 `type: relay` 且 `proxies: [<front>, <landing>]`；含 `...,<chain>` 规则。
  - QuanX 输出含 `ip-cidr, <ip>/32, <front>`（或 `host, <domain>, <front>`）与 `..., <landing>, via-interface=%TUN%`。
- 形式：轻量 shell 脚本驱动二进制 + 断言（grep），不引入重型 C++ 测试框架。
- 单元层面：对 `resolveChains` 的 IP/域名判定、landing 非唯一→跳过，加最小 C++ 断言或 golden 覆盖。

## 8. 边界与失败处理

| 场景 | 处理 |
|---|---|
| `landing` 解析到 0 或多个节点 | warn（`writeLog`）+ 跳过该链 |
| `front` 为 `[]组` 但组不存在 | warn + 仍生成（交客户端报错），或跳过——实现取 warn+生成 |
| 多条链共用同一 landing | QuanX 回流规则去重；Clash 各自 relay 组 |
| 域名 ruleset 指向未定义链名 | 退化为普通规则，不加 via / 不回流 |
| 链名与已有策略组同名 | 链覆盖 + warn（复用现有同名替换） |
| landing server 是域名而非 IP | QuanX 回流用 `host,` 而非 `ip-cidr,` |
| 节点名/正则含 QuanX/YAML 特殊字符 | 复用各 emitter 现有转义路径 |

## 9. 验收标准

1. 给定 §3 样例配置，`target=clash` 输出含 relay 组与链路规则；`target=quanx` 输出含回流规则与 `via-interface=%TUN%` 改写规则。
2. landing 非唯一时不崩溃、有 warn、其余链正常。
3. 现有非链路转换行为不回归（手写 relay 组在 QuanX 不再静默消失）。
4. golden 测试通过；项目可正常构建。
